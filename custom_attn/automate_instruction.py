#!/usr/bin/env python3
"""
automate_instruction.py — Automated Custom RISC-V Instruction Generator
========================================================================
Group 9 | RISC-V GNU Toolchain

Automates the complete process of adding/removing custom RISC-V instructions:
  1. Scans riscv-opc.h to find free opcode slots (custom-0 … custom-3)
  2. Modifies all 5 toolchain source files
  3. Rebuilds binutils + GCC (incremental if already built)
  4. Generates a demo C program
  5. Compiles the demo and runs objdump to verify

Supports:
  - 0, 1, 2, or 3 input instructions (all R-type except 3 → R4-type)
  - Batch mode: add up to 4 instructions at once
  - Delete mode: remove previously added custom instructions
  - Scan mode: show free opcode slots

Usage:
  python3 automate_instruction.py                                   # Interactive
  python3 automate_instruction.py --name my_pow --inputs 2          # Add one
  python3 automate_instruction.py --batch instructions.txt          # Batch add
  python3 automate_instruction.py --delete my_pow                   # Delete one
  python3 automate_instruction.py --delete my_pow fused_mac         # Delete multiple
  python3 automate_instruction.py --list                            # List custom instructions
  python3 automate_instruction.py --scan 2                          # Show free opcodes

Requirements:
  - The riscv-gnu-toolchain repo must be present at ../  (relative to this script)
  - binutils and gcc submodules must be initialized
  - Build environment (micromamba / system gcc, etc.) must be available
"""

import argparse
import os
import re
import subprocess
import sys
import textwrap
from pathlib import Path

# ── Paths ────────────────────────────────────────────────────────────────
SCRIPT_DIR   = Path(__file__).resolve().parent          # custom_attn/
REPO_ROOT    = SCRIPT_DIR.parent                        # riscv-gnu-toolchain/
BINUTILS_DIR = REPO_ROOT / "binutils"
GCC_DIR      = REPO_ROOT / "gcc"

OPC_H       = BINUTILS_DIR / "include" / "opcode" / "riscv-opc.h"
OPC_C       = BINUTILS_DIR / "opcodes"  / "riscv-opc.c"
FTYPES_DEF  = GCC_DIR / "gcc" / "config" / "riscv" / "riscv-ftypes.def"
BUILTINS_CC = GCC_DIR / "gcc" / "config" / "riscv" / "riscv-builtins.cc"
RISCV_MD    = GCC_DIR / "gcc" / "config" / "riscv" / "riscv.md"

INSTALL_PREFIX = Path.home() / "riscv_custom"
BUILD_DIR      = REPO_ROOT / "build_custom"

# Auto-generated marker used to identify our additions
MARKER = "(auto-generated)"

# ── RISC-V Custom Opcode Bases ──────────────────────────────────────────
CUSTOM_BASES = {
    "custom-0": 0x0B,
    "custom-1": 0x2B,
    "custom-2": 0x5B,
    "custom-3": 0x7B,
}

# ── Input count → encoding config ──────────────────────────────────────
INPUT_CONFIG = {
    0: {"format": "R-type", "operand_fmt": "d",       "ftype_args": 0, "ftype_tuple": "(DI)",             "ftype_name": "RISCV_DI_FTYPE",          "asm_operands": "%0"},
    1: {"format": "R-type", "operand_fmt": "d,s",     "ftype_args": 1, "ftype_tuple": "(DI, DI)",         "ftype_name": "RISCV_DI_FTYPE_DI",       "asm_operands": "%0,%1"},
    2: {"format": "R-type", "operand_fmt": "d,s,t",   "ftype_args": 2, "ftype_tuple": "(DI, DI, DI)",     "ftype_name": "RISCV_DI_FTYPE_DI_DI",    "asm_operands": "%0,%1,%2"},
    3: {"format": "R4-type","operand_fmt": "d,s,t,r",  "ftype_args": 3, "ftype_tuple": "(DI, DI, DI, DI)", "ftype_name": "RISCV_DI_FTYPE_DI_DI_DI", "asm_operands": "%0,%1,%2,%3"},
}


# ════════════════════════════════════════════════════════════════════════
#  STEP 1 — Opcode Scanning & Allocation
# ════════════════════════════════════════════════════════════════════════

def parse_existing_match_values(opc_h_path):
    """Read riscv-opc.h and extract all #define MATCH_* hex values."""
    matches = set()
    pattern = re.compile(r'#define\s+MATCH_\w+\s+(0x[0-9a-fA-F]+)')
    with open(opc_h_path, 'r') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                matches.add(int(m.group(1), 16))
    return matches


def compute_match_mask(base_opcode, funct3, funct7=0, funct2=0, num_inputs=2):
    """
    Compute MATCH and MASK values.
    0/1/2 inputs → R-type:  funct3 + funct7
    3 inputs     → R4-type: funct3 + funct2
    """
    if num_inputs <= 2:
        match_val = base_opcode | (funct3 << 12) | (funct7 << 25)
        mask_val  = 0xFE00707F   # opcode[6:0] + funct3[14:12] + funct7[31:25]
    else:  # 3 inputs, R4-type
        match_val = base_opcode | (funct3 << 12) | (funct2 << 25)
        mask_val  = 0x0600707F   # opcode[6:0] + funct3[14:12] + funct2[26:25]
    return match_val, mask_val


def find_free_opcode(num_inputs):
    """
    Find the first free (base, funct3, funct7/funct2) combination
    across custom-0 → custom-3.

    Returns: (base_name, base_opcode, funct3, funct7_or_funct2, match, mask)
             or None if all slots occupied.
    """
    existing = parse_existing_match_values(OPC_H)

    for base_name, base_opc in CUSTOM_BASES.items():
        if num_inputs <= 2:
            for f3 in range(8):
                for f7 in range(128):
                    match_val, mask_val = compute_match_mask(base_opc, f3, funct7=f7, num_inputs=num_inputs)
                    if match_val not in existing:
                        return (base_name, base_opc, f3, f7, match_val, mask_val)
        else:
            for f3 in range(8):
                for f2 in range(4):
                    match_val, mask_val = compute_match_mask(base_opc, f3, funct2=f2, num_inputs=3)
                    if match_val not in existing:
                        return (base_name, base_opc, f3, f2, match_val, mask_val)

    return None


def show_all_free_opcodes(num_inputs, limit=20):
    """Print a sample of free opcode slots."""
    existing = parse_existing_match_values(OPC_H)
    found = 0
    funct_label = "funct7" if num_inputs <= 2 else "funct2"
    print(f"\n{'Slot':<12} {'funct3':<8} {funct_label:<10} {'MATCH':<14} {'MASK':<14}")
    print("-" * 60)

    for base_name, base_opc in CUSTOM_BASES.items():
        if num_inputs <= 2:
            for f3 in range(8):
                for f7 in range(128):
                    m, mask = compute_match_mask(base_opc, f3, funct7=f7, num_inputs=num_inputs)
                    if m not in existing:
                        print(f"{base_name:<12} {f3:<8} {f7:<10} 0x{m:08x}    0x{mask:08x}")
                        found += 1
                        if found >= limit:
                            print(f"... (showing first {limit} of many)")
                            return found
        else:
            for f3 in range(8):
                for f2 in range(4):
                    m, mask = compute_match_mask(base_opc, f3, funct2=f2, num_inputs=3)
                    if m not in existing:
                        print(f"{base_name:<12} {f3:<8} {f2:<10} 0x{m:08x}    0x{mask:08x}")
                        found += 1
                        if found >= limit:
                            print(f"... (showing first {limit} of many)")
                            return found
    return found


def list_custom_instructions():
    """List all auto-generated custom instructions found in riscv-opc.h."""
    if not OPC_H.exists():
        print("ERROR: riscv-opc.h not found")
        return []

    text = OPC_H.read_text()
    # Find all auto-generated MATCH defines
    pattern = re.compile(
        r'/\*\s*Custom Instruction\s*[-—]\s*(\w+)\s*\(auto-generated\)\s*\*/\s*\n'
        r'#define\s+MATCH_\w+\s+(0x[0-9a-fA-F]+)\s*\n'
        r'#define\s+MASK_\w+\s+(0x[0-9a-fA-F]+)',
        re.MULTILINE
    )

    instructions = []
    for m in pattern.finditer(text):
        name = m.group(1)
        match_val = m.group(2)
        mask_val = m.group(3)

        # Determine num_inputs from riscv-opc.c operand format
        opc_c_text = OPC_C.read_text() if OPC_C.exists() else ""
        fmt_match = re.search(rf'"{name}".*?"([^"]*)".*?MATCH_{name.upper()}', opc_c_text)
        fmt = fmt_match.group(1) if fmt_match else "?"
        num_inputs = fmt.count(",")

        instructions.append({
            "name": name,
            "match": match_val,
            "mask": mask_val,
            "format": fmt,
            "inputs": num_inputs,
        })

    if not instructions:
        print("\n  No auto-generated custom instructions found.")
    else:
        print(f"\n  {'Name':<20} {'Inputs':<8} {'Format':<10} {'MATCH':<14} {'MASK':<14}")
        print("  " + "-" * 66)
        for insn in instructions:
            print(f"  {insn['name']:<20} {insn['inputs']:<8} {insn['format']:<10} {insn['match']:<14} {insn['mask']:<14}")

    return instructions


# ════════════════════════════════════════════════════════════════════════
#  STEP 2 — Modify Toolchain Source Files (ADD)
# ════════════════════════════════════════════════════════════════════════

def modify_riscv_opc_h(insn_name, match_val, mask_val):
    """Add MATCH_/MASK_ defines and DECLARE_INSN to riscv-opc.h."""
    upper = insn_name.upper()
    text = OPC_H.read_text()

    define_block = (
        f"\n/* Custom Instruction — {insn_name} {MARKER} */\n"
        f"#define MATCH_{upper}  0x{match_val:08x}\n"
        f"#define MASK_{upper}   0x{mask_val:08x}\n"
    )
    declare_line = f"DECLARE_INSN({insn_name}, MATCH_{upper}, MASK_{upper})\n"

    if f"MATCH_{upper}" in text:
        print(f"  [opc.h] MATCH_{upper} already defined — skipping")
        return

    anchor_match = re.search(r'/\*\s*Instruction opcode macros\.\s*\*/', text)
    if anchor_match:
        end = text.index("\n", anchor_match.end()) + 1
        text = text[:end] + define_block + text[end:]
    else:
        first_match = re.search(r'^#define\s+MATCH_', text, re.MULTILINE)
        if first_match:
            text = text[:first_match.start()] + define_block + "\n" + text[first_match.start():]
        else:
            print("  [opc.h] ERROR: Cannot find insertion point for defines")
            return

    last_declare = text.rfind("DECLARE_INSN(")
    if last_declare >= 0:
        end_of_line = text.index("\n", last_declare) + 1
        text = text[:end_of_line] + declare_line + text[end_of_line:]

    OPC_H.write_text(text)
    print(f"  [opc.h] Added MATCH_{upper}, MASK_{upper}, DECLARE_INSN")


def modify_riscv_opc_c(insn_name, match_val, mask_val, num_inputs):
    """Add entry to riscv_opcodes[] in riscv-opc.c."""
    text = OPC_C.read_text()
    upper = insn_name.upper()

    if f'"{insn_name}"' in text:
        print(f"  [opc.c] \"{insn_name}\" already in riscv_opcodes — skipping")
        return

    cfg = INPUT_CONFIG[num_inputs]
    fmt = cfg["operand_fmt"]

    entry = (
        f'\n/* Custom Instruction — {insn_name} {MARKER} */\n'
        f'{{"{insn_name}", 0, INSN_CLASS_I, "{fmt}", '
        f'MATCH_{upper}, MASK_{upper}, match_opcode, 0}},\n'
    )

    anchor = "const struct riscv_opcode riscv_opcodes[] ="
    idx = text.find(anchor)
    if idx < 0:
        print("  [opc.c] ERROR: Cannot find riscv_opcodes[] — manual edit needed")
        return
    brace = text.index("{", idx + len(anchor))
    insert_pos = brace + 1
    text = text[:insert_pos] + entry + text[insert_pos:]

    OPC_C.write_text(text)
    print(f"  [opc.c] Added \"{insn_name}\" to riscv_opcodes[]")


def modify_riscv_ftypes_def(num_inputs):
    """Add the function type DEF_RISCV_FTYPE to riscv-ftypes.def if not present."""
    text = FTYPES_DEF.read_text()
    cfg = INPUT_CONFIG[num_inputs]

    ftype_line = f"DEF_RISCV_FTYPE ({cfg['ftype_args']}, {cfg['ftype_tuple']})"
    ftype_name = cfg["ftype_name"]

    if ftype_line in text:
        print(f"  [ftypes.def] {ftype_name} already defined — skipping")
        return

    text += f"\n/* Custom instruction ftype {MARKER}: {ftype_name} */\n{ftype_line}\n"
    FTYPES_DEF.write_text(text)
    print(f"  [ftypes.def] Added {ftype_name}")


def modify_riscv_builtins_cc(insn_name, num_inputs):
    """Add RISCV_ATYPE_DI, AVAIL(always_enabled), and DIRECT_BUILTIN entry."""
    text = BUILTINS_CC.read_text()
    cfg = INPUT_CONFIG[num_inputs]

    # 1. RISCV_ATYPE_DI
    if "RISCV_ATYPE_DI" not in text:
        anchor = "#define RISCV_ATYPE_SI"
        idx = text.find(anchor)
        if idx >= 0:
            end = text.index("\n", idx) + 1
            text = text[:end] + "#define RISCV_ATYPE_DI long_integer_type_node\n" + text[end:]
            print("  [builtins.cc] Added RISCV_ATYPE_DI")
        else:
            print("  [builtins.cc] WARNING: Could not find RISCV_ATYPE_SI anchor")
    else:
        print("  [builtins.cc] RISCV_ATYPE_DI already present — skipping")

    # 2. AVAIL(always_enabled)
    if "always_enabled" not in text:
        anchor = "AVAIL (hint_pause"
        idx = text.find(anchor)
        if idx >= 0:
            end = text.index("\n", idx) + 1
            text = text[:end] + "AVAIL (always_enabled, (!0))\n" + text[end:]
            print("  [builtins.cc] Added AVAIL(always_enabled)")
        else:
            print("  [builtins.cc] WARNING: Could not find AVAIL(hint_pause) anchor")
    else:
        print("  [builtins.cc] AVAIL(always_enabled) already present — skipping")

    # 3. DIRECT_BUILTIN entry
    ftype = cfg["ftype_name"]
    builtin_line = f'DIRECT_BUILTIN ({insn_name}, {ftype}, always_enabled),'

    if f"DIRECT_BUILTIN ({insn_name}," in text:
        print(f"  [builtins.cc] DIRECT_BUILTIN({insn_name}) already present — skipping")
    else:
        anchor = '#include "corev.def"'
        idx = text.find(anchor)
        if idx >= 0:
            end = text.index("\n", idx) + 1
        else:
            idx = text.rfind("DIRECT_BUILTIN")
            end = text.index("\n", idx) + 1

        insert = (
            f"\n  /* Custom Instruction — {insn_name} {MARKER} */\n"
            f"  {builtin_line}\n"
        )
        text = text[:end] + insert + text[end:]
        print(f"  [builtins.cc] Added DIRECT_BUILTIN({insn_name})")

    BUILTINS_CC.write_text(text)


def modify_riscv_md(insn_name, num_inputs):
    """Add UNSPEC_<NAME> and define_insn pattern to riscv.md."""
    text = RISCV_MD.read_text()
    upper = insn_name.upper()
    unspec_name = f"UNSPEC_{upper}"
    cfg = INPUT_CONFIG[num_inputs]

    # 1. Add to unspec enum
    if unspec_name not in text:
        enum_pattern = r'(\(define_c_enum\s+"unspec"\s*\[)'
        m = re.search(enum_pattern, text)
        if m:
            insert_pos = m.end()
            text = text[:insert_pos] + f"\n  ;; Custom Instruction — {insn_name} {MARKER}\n  {unspec_name}\n" + text[insert_pos:]
            print(f"  [riscv.md] Added {unspec_name} to unspec enum")
        else:
            print("  [riscv.md] WARNING: Could not find define_c_enum \"unspec\"")
    else:
        print(f"  [riscv.md] {unspec_name} already in unspec enum — skipping")

    # 2. Add define_insn pattern
    pattern_name = f"riscv_{insn_name}"
    if pattern_name in text:
        print(f"  [riscv.md] define_insn \"{pattern_name}\" already present — skipping")
    else:
        # Build operand list based on num_inputs
        operands = []
        for i in range(1, num_inputs + 1):
            operands.append(f'(match_operand:DI {i} "register_operand" "r")')

        if num_inputs == 0:
            unspec_body = f"(unspec:DI [] {unspec_name})"
        else:
            inner = "\n                                ".join(operands)
            unspec_body = f"(unspec:DI [{inner}]\n                               {unspec_name})"

        insn_pattern = textwrap.dedent(f'''\

            ;; Custom Instruction — {insn_name} {MARKER}
            (define_insn "{pattern_name}"
              [(set (match_operand:DI 0 "register_operand" "=r")
                    {unspec_body})]
              ""
              "{insn_name}\\t{cfg['asm_operands']}"
              [(set_attr "type" "arith")
               (set_attr "mode" "DI")])
        ''')

        text += insn_pattern
        print(f"  [riscv.md] Added define_insn \"{pattern_name}\"")

    RISCV_MD.write_text(text)


# ════════════════════════════════════════════════════════════════════════
#  DELETE — Remove a custom instruction from all 5 files
# ════════════════════════════════════════════════════════════════════════

def delete_from_riscv_opc_h(insn_name):
    """Remove MATCH_/MASK_ defines and DECLARE_INSN from riscv-opc.h."""
    upper = insn_name.upper()
    text = OPC_H.read_text()
    original_len = len(text)

    # Remove the comment + MATCH + MASK block
    text = re.sub(
        rf'/\*\s*Custom Instruction\s*[-—]\s*{re.escape(insn_name)}\s*\(auto-generated\)\s*\*/\s*\n'
        rf'#define\s+MATCH_{upper}\s+0x[0-9a-fA-F]+\s*\n'
        rf'#define\s+MASK_{upper}\s+0x[0-9a-fA-F]+\s*\n',
        '', text
    )

    # Remove DECLARE_INSN line
    text = re.sub(
        rf'DECLARE_INSN\({re.escape(insn_name)},\s*MATCH_{upper},\s*MASK_{upper}\)\s*\n',
        '', text
    )

    if len(text) < original_len:
        OPC_H.write_text(text)
        print(f"  [opc.h] Removed MATCH_{upper}, MASK_{upper}, DECLARE_INSN")
    else:
        print(f"  [opc.h] {insn_name} not found — nothing to remove")


def delete_from_riscv_opc_c(insn_name):
    """Remove entry from riscv_opcodes[] in riscv-opc.c."""
    text = OPC_C.read_text()
    original_len = len(text)

    text = re.sub(
        rf'/\*\s*Custom Instruction\s*[-—]\s*{re.escape(insn_name)}\s*\(auto-generated\)\s*\*/\s*\n'
        rf'\{{"{re.escape(insn_name)}"[^\}}]*\}},\s*\n',
        '', text
    )

    if len(text) < original_len:
        OPC_C.write_text(text)
        print(f"  [opc.c] Removed \"{insn_name}\" from riscv_opcodes[]")
    else:
        print(f"  [opc.c] {insn_name} not found — nothing to remove")


def delete_from_riscv_builtins_cc(insn_name):
    """Remove DIRECT_BUILTIN entry from riscv-builtins.cc."""
    text = BUILTINS_CC.read_text()
    original_len = len(text)

    text = re.sub(
        rf'\n\s*/\*\s*Custom Instruction\s*[-—]\s*{re.escape(insn_name)}\s*\(auto-generated\)\s*\*/\s*\n'
        rf'\s*DIRECT_BUILTIN\s*\({re.escape(insn_name)},[^\)]*\),?\s*\n',
        '\n', text
    )

    if len(text) < original_len:
        BUILTINS_CC.write_text(text)
        print(f"  [builtins.cc] Removed DIRECT_BUILTIN({insn_name})")
    else:
        print(f"  [builtins.cc] {insn_name} not found — nothing to remove")


def delete_from_riscv_md(insn_name):
    """Remove UNSPEC_<NAME> and define_insn from riscv.md."""
    upper = insn_name.upper()
    text = RISCV_MD.read_text()
    original_len = len(text)

    # Remove UNSPEC entry from enum
    text = re.sub(
        rf'\s*;;\s*Custom Instruction\s*[-—]\s*{re.escape(insn_name)}\s*\(auto-generated\)\s*\n'
        rf'\s*UNSPEC_{upper}\s*\n',
        '\n', text
    )

    # Remove define_insn block
    text = re.sub(
        rf'\s*;;\s*Custom Instruction\s*[-—]\s*{re.escape(insn_name)}\s*\(auto-generated\)\s*\n'
        rf'\s*\(define_insn\s+"riscv_{re.escape(insn_name)}".*?'
        rf'\(set_attr\s+"mode"\s+"DI"\)\]\)',
        '', text, flags=re.DOTALL
    )

    if len(text) < original_len:
        RISCV_MD.write_text(text)
        print(f"  [riscv.md] Removed UNSPEC_{upper} and define_insn \"riscv_{insn_name}\"")
    else:
        print(f"  [riscv.md] {insn_name} not found — nothing to remove")


def check_ftype_still_used(num_inputs):
    """Check if any other auto-generated instruction still uses this ftype."""
    cfg = INPUT_CONFIG[num_inputs]
    ftype = cfg["ftype_name"]

    if not BUILTINS_CC.exists():
        return False
    text = BUILTINS_CC.read_text()
    matches = re.findall(rf'DIRECT_BUILTIN\s*\(\w+,\s*{re.escape(ftype)},', text)
    return len(matches) > 0


def delete_from_riscv_ftypes_def(num_inputs):
    """Remove ftype only if no other custom instruction uses it."""
    if check_ftype_still_used(num_inputs):
        cfg = INPUT_CONFIG[num_inputs]
        print(f"  [ftypes.def] {cfg['ftype_name']} still used by other instructions — keeping")
        return

    text = FTYPES_DEF.read_text()
    cfg = INPUT_CONFIG[num_inputs]
    original_len = len(text)

    ftype_line = f"DEF_RISCV_FTYPE ({cfg['ftype_args']}, {cfg['ftype_tuple']})"
    text = re.sub(
        rf'/\*\s*Custom instruction\s*\(auto-generated\):\s*{re.escape(cfg["ftype_name"])}\s*\*/\s*\n'
        rf'{re.escape(ftype_line)}\s*\n',
        '', text
    )

    if len(text) < original_len:
        FTYPES_DEF.write_text(text)
        print(f"  [ftypes.def] Removed {cfg['ftype_name']}")
    else:
        print(f"  [ftypes.def] {cfg['ftype_name']} not found — nothing to remove")


def delete_instruction(insn_name):
    """Remove a custom instruction from all 5 toolchain source files."""
    print(f"\n  Deleting custom instruction: {insn_name}")
    print("  " + "-" * 50)

    # Determine num_inputs from opc.c before deleting
    num_inputs = 2  # default
    if OPC_C.exists():
        opc_c_text = OPC_C.read_text()
        fmt_match = re.search(rf'"{re.escape(insn_name)}"[^,]*,[^,]*,[^,]*,\s*"([^"]*)"', opc_c_text)
        if fmt_match:
            fmt = fmt_match.group(1)
            num_inputs = fmt.count(",")

    delete_from_riscv_opc_h(insn_name)
    delete_from_riscv_opc_c(insn_name)
    delete_from_riscv_builtins_cc(insn_name)
    delete_from_riscv_md(insn_name)
    delete_from_riscv_ftypes_def(num_inputs)

    # Clean up demo files and reference copies
    demo_dir = SCRIPT_DIR / "demo"
    src_dir = SCRIPT_DIR / "src"
    for f in [
        demo_dir / f"main_{insn_name}.c",
        demo_dir / f"main_{insn_name}.o",
        demo_dir / f"main_{insn_name}_objdump.txt",
        demo_dir / f"main_{insn_name}_hexdump.txt",
        demo_dir / f"build_{insn_name}.sh",
        src_dir / f"{insn_name}_additions.txt",
    ]:
        if f.exists():
            f.unlink()
            print(f"  [cleanup] Removed {f.name}")

    print(f"  Done — \"{insn_name}\" removed from all toolchain files")


# ════════════════════════════════════════════════════════════════════════
#  STEP 3 — Generate Demo C Code
# ════════════════════════════════════════════════════════════════════════

def generate_demo_c(insn_name, num_inputs, description):
    """Generate demo/main_<name>.c with noinline wrapper + main."""
    demo_dir = SCRIPT_DIR / "demo"
    demo_dir.mkdir(exist_ok=True)
    demo_file = demo_dir / f"main_{insn_name}.c"

    builtin = f"__builtin_riscv_{insn_name}"
    wrapper = f"wrapper_{insn_name}"
    cfg = INPUT_CONFIG[num_inputs]

    if num_inputs == 0:
        wrapper_params = "void"
        wrapper_call = f"{builtin}()"
        main_body = f"volatile unsigned long result = {wrapper}();"
    elif num_inputs == 1:
        wrapper_params = "unsigned long a0"
        wrapper_call = f"{builtin}(a0)"
        main_body = (
            "unsigned long A = 42;\n"
            f"    volatile unsigned long result = {wrapper}((unsigned long)&A);"
        )
    elif num_inputs == 2:
        wrapper_params = "unsigned long a0, unsigned long a1"
        wrapper_call = f"{builtin}(a0, a1)"
        main_body = (
            "float A = 2.0f;\n"
            "    float B = 4.0f;\n"
            f"    volatile unsigned long result = {wrapper}(\n"
            f"        (unsigned long)&A, (unsigned long)&B);"
        )
    else:
        wrapper_params = "unsigned long a0, unsigned long a1, unsigned long a2"
        wrapper_call = f"{builtin}(a0, a1, a2)"
        main_body = (
            "float A = 2.0f;\n"
            "    float B = 4.0f;\n"
            "    float C = 6.0f;\n"
            f"    volatile unsigned long result = {wrapper}(\n"
            f"        (unsigned long)&A, (unsigned long)&B, (unsigned long)&C);"
        )

    code = textwrap.dedent(f"""\
    /*
     * main_{insn_name}.c — Demo for custom RISC-V instruction "{insn_name}"
     * Description: {description}
     * Inputs: {num_inputs} ({cfg['format']})
     * Builtin: {builtin}()
     *
     * Compile:
     *   riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \\
     *       -ffreestanding -nostdinc -c main_{insn_name}.c -o main_{insn_name}.o
     *
     * Disassemble:
     *   riscv64-unknown-elf-objdump -d main_{insn_name}.o
     */

    __attribute__((noinline))
    unsigned long {wrapper}({wrapper_params})
    {{
        return {wrapper_call};
    }}

    int main(void)
    {{
        {main_body}
        (void)result;
        return 0;
    }}
    """)

    demo_file.write_text(code)
    print(f"  [demo] Generated {demo_file}")
    return demo_file


def generate_build_script(insn_name):
    """Generate a build-and-dump shell script for the demo."""
    demo_dir = SCRIPT_DIR / "demo"
    script = demo_dir / f"build_{insn_name}.sh"
    script.write_text(textwrap.dedent(f"""\
    #!/usr/bin/env bash
    set -euo pipefail
    RISCV_PREFIX="${{RISCV_PREFIX:-riscv64-unknown-elf}}"
    DIR="$(cd "$(dirname "$0")" && pwd)"

    echo "=== Compiling main_{insn_name}.c ==="
    "${{RISCV_PREFIX}}-gcc" -O2 -march=rv64imac -mabi=lp64 \\
        -ffreestanding -nostdinc \\
        -c "$DIR/main_{insn_name}.c" -o "$DIR/main_{insn_name}.o"

    echo ""
    echo "=== Full Disassembly ==="
    "${{RISCV_PREFIX}}-objdump" -d "$DIR/main_{insn_name}.o" | tee "$DIR/main_{insn_name}_objdump.txt"

    echo ""
    echo "=== Grep for '{insn_name}' instruction ==="
    "${{RISCV_PREFIX}}-objdump" -d "$DIR/main_{insn_name}.o" | grep -n "{insn_name}" || echo "(not found in objdump)"

    echo ""
    echo "Build complete."
    """))
    script.chmod(0o755)
    print(f"  [demo] Generated {script}")
    return script


# ════════════════════════════════════════════════════════════════════════
#  STEP 4 — Build Toolchain
# ════════════════════════════════════════════════════════════════════════

def run_cmd(cmd, cwd=None, timeout=600):
    """Run a shell command, print output on failure."""
    print(f"  $ {cmd}")
    result = subprocess.run(
        cmd, shell=True, cwd=cwd,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout
    )
    if result.returncode != 0:
        print(f"  FAILED (exit {result.returncode}):")
        print(result.stdout.decode(errors='replace')[-2000:])
        return False
    return True


def rebuild_binutils():
    """Incremental rebuild of binutils."""
    build = BUILD_DIR / "build-binutils-newlib"
    if not build.exists():
        for name in ["build-binutils", "build"]:
            alt = BUILD_DIR / name
            if alt.exists():
                build = alt
                break
        else:
            build = BUILD_DIR / "build-binutils-newlib"
            build.mkdir(parents=True, exist_ok=True)
            print("\n[BUILD] Configuring binutils from scratch...")
            ok = run_cmd(
                f'{BINUTILS_DIR}/configure '
                f'--target=riscv64-unknown-elf '
                f'--prefix={INSTALL_PREFIX} '
                f'--disable-werror',
                cwd=build, timeout=120
            )
            if not ok:
                return False

    print("\n[BUILD] Rebuilding binutils (incremental)...")
    ok = run_cmd(f'make -j{os.cpu_count() or 4}', cwd=build, timeout=600)
    if not ok:
        return False
    ok = run_cmd('make install', cwd=build, timeout=120)
    return ok


def rebuild_gcc():
    """Incremental rebuild of GCC stage 1."""
    build = None
    for candidate in [
        BUILD_DIR / "build-gcc-newlib-stage1",
        BUILD_DIR / "build-gcc-stage1",
        BUILD_DIR / "build-gcc",
    ]:
        if candidate.exists() and (candidate / "Makefile").exists():
            build = candidate
            break

    if build is None:
        build = BUILD_DIR / "build-gcc-newlib-stage1"
        build.mkdir(parents=True, exist_ok=True)
        print("\n[BUILD] Configuring GCC stage 1 from scratch...")
        ok = run_cmd(
            f'{GCC_DIR}/configure '
            f'--target=riscv64-unknown-elf '
            f'--prefix={INSTALL_PREFIX} '
            f'--disable-shared '
            f'--disable-threads '
            f'--disable-multilib '
            f'--disable-libatomic '
            f'--disable-libmudflap '
            f'--disable-libssp '
            f'--disable-libquadmath '
            f'--disable-libgomp '
            f'--disable-nls '
            f'--disable-bootstrap '
            f'--enable-languages=c '
            f'--with-arch=rv64imac '
            f'--with-abi=lp64 '
            f'--with-newlib',
            cwd=build, timeout=120
        )
        if not ok:
            return False

    print("\n[BUILD] Rebuilding GCC (incremental)...")
    ok = run_cmd(f'make all-gcc -j{os.cpu_count() or 4}', cwd=build, timeout=600)
    if not ok:
        return False
    ok = run_cmd('make install-gcc', cwd=build, timeout=120)
    return ok


# ════════════════════════════════════════════════════════════════════════
#  STEP 5 — Compile Demo & Dump
# ════════════════════════════════════════════════════════════════════════

def compile_and_dump(insn_name):
    """Compile the demo C file and run objdump."""
    demo_dir = SCRIPT_DIR / "demo"
    src  = demo_dir / f"main_{insn_name}.c"
    obj  = demo_dir / f"main_{insn_name}.o"

    prefix = f"{INSTALL_PREFIX}/bin/riscv64-unknown-elf"

    print(f"\n[COMPILE] {src.name} → {obj.name}")
    ok = run_cmd(
        f'"{prefix}-gcc" -O2 -march=rv64imac -mabi=lp64 '
        f'-ffreestanding -nostdinc '
        f'-c "{src}" -o "{obj}"'
    )
    if not ok:
        print("  Compilation FAILED")
        return False

    print(f"\n[OBJDUMP] Full disassembly:")
    result = subprocess.run(
        f'"{prefix}-objdump" -d "{obj}"',
        shell=True, capture_output=True, text=True
    )
    print(result.stdout)

    dump_file = demo_dir / f"main_{insn_name}_objdump.txt"
    dump_file.write_text(result.stdout)

    # Grep for the instruction with line numbers
    print(f"[GREP] Lines containing '{insn_name}':")
    for i, line in enumerate(result.stdout.splitlines(), 1):
        if insn_name in line:
            print(f"  line {i}: {line.strip()}")

    if insn_name in result.stdout:
        print(f"\n  SUCCESS: '{insn_name}' instruction found in disassembly!")
        return True
    else:
        print(f"\n  WARNING: '{insn_name}' mnemonic NOT found in disassembly")
        return True


# ════════════════════════════════════════════════════════════════════════
#  STEP 6 — Save Reference Source Copies
# ════════════════════════════════════════════════════════════════════════

def save_reference_copies(insn_name, num_inputs, match_val, mask_val):
    """Save copies of what was added, for documentation."""
    src_dir = SCRIPT_DIR / "src"
    src_dir.mkdir(exist_ok=True)

    upper = insn_name.upper()
    cfg = INPUT_CONFIG[num_inputs]

    ref_file = src_dir / f"{insn_name}_additions.txt"
    ref_file.write_text(textwrap.dedent(f"""\
    ================================================================
    Auto-generated additions for: {insn_name}
    Inputs: {num_inputs} ({cfg['format']})  |  MATCH: 0x{match_val:08x}  |  MASK: 0x{mask_val:08x}
    ================================================================

    --- riscv-opc.h ---
    #define MATCH_{upper}  0x{match_val:08x}
    #define MASK_{upper}   0x{mask_val:08x}
    DECLARE_INSN({insn_name}, MATCH_{upper}, MASK_{upper})

    --- riscv-opc.c ---
    {{"{insn_name}", 0, INSN_CLASS_I, "{cfg['operand_fmt']}", MATCH_{upper}, MASK_{upper}, match_opcode, 0}},

    --- riscv-ftypes.def ---
    DEF_RISCV_FTYPE ({cfg['ftype_args']}, {cfg['ftype_tuple']})

    --- riscv-builtins.cc ---
    DIRECT_BUILTIN ({insn_name}, {cfg['ftype_name']}, always_enabled),

    --- riscv.md ---
    UNSPEC_{upper}  (in define_c_enum "unspec")
    define_insn "riscv_{insn_name}" → "{insn_name}\\t{cfg['asm_operands']}"
    """))
    print(f"  [ref] Saved reference: {ref_file}")


# ════════════════════════════════════════════════════════════════════════
#  BATCH — Process multiple instructions from a file
# ════════════════════════════════════════════════════════════════════════

def parse_batch_file(batch_path):
    """
    Parse a batch file. Format (one instruction per line):
      name  inputs  description

    Example:
      my_pow     2  (a^b)^a
      fused_mac  3  a*b+c fused multiply-add
      hw_status  0  read hardware status register
      my_relu    1  max(0, x)

    Lines starting with # are comments. Max 4 instructions.
    """
    instructions = []
    with open(batch_path, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(None, 2)
            if len(parts) < 2:
                print(f"  WARNING: Line {line_num} malformed (need at least 'name inputs'): {line}")
                continue
            name = re.sub(r'[^a-z0-9_]', '_', parts[0].lower())
            try:
                num_inputs = int(parts[1])
                if num_inputs not in (0, 1, 2, 3):
                    raise ValueError
            except ValueError:
                print(f"  WARNING: Line {line_num} invalid inputs (must be 0-3): {parts[1]}")
                continue
            desc = parts[2] if len(parts) > 2 else "Custom instruction"
            instructions.append((name, num_inputs, desc))

    if len(instructions) > 4:
        print(f"\n  WARNING: Batch file has {len(instructions)} instructions, max is 4.")
        print(f"  Only the first 4 will be processed.")
        instructions = instructions[:4]

    return instructions


# ════════════════════════════════════════════════════════════════════════
#  ADD — Core logic to add one instruction
# ════════════════════════════════════════════════════════════════════════

def add_instruction(insn_name, num_inputs, desc):
    """Add a single custom instruction. Returns (match_val, mask_val) or None."""
    cfg = INPUT_CONFIG[num_inputs]

    print(f"\n  Instruction : {insn_name}")
    print(f"  Inputs      : {num_inputs} ({cfg['format']})")
    print(f"  Description : {desc}")

    # Find free opcode
    print("\n  Finding free opcode slot...")
    result = find_free_opcode(num_inputs)
    if result is None:
        print(f"  ERROR: No free opcode slots for {num_inputs}-input instructions!")
        print(f"  Use --delete to remove an existing instruction and free up a slot.")
        return None

    base_name, base_opc, f3, f7_or_f2, match_val, mask_val = result
    funct_label = "funct7" if num_inputs <= 2 else "funct2"
    print(f"    Base     : {base_name} (0x{base_opc:02x})")
    print(f"    funct3   : {f3}")
    print(f"    {funct_label:<8} : {f7_or_f2}")
    print(f"    MATCH    : 0x{match_val:08x}")
    print(f"    MASK     : 0x{mask_val:08x}")

    # Modify source files
    print(f"\n  Modifying toolchain source files...")
    modify_riscv_opc_h(insn_name, match_val, mask_val)
    modify_riscv_opc_c(insn_name, match_val, mask_val, num_inputs)
    modify_riscv_ftypes_def(num_inputs)
    modify_riscv_builtins_cc(insn_name, num_inputs)
    modify_riscv_md(insn_name, num_inputs)

    # Save references
    save_reference_copies(insn_name, num_inputs, match_val, mask_val)

    # Generate demo
    print(f"\n  Generating demo...")
    generate_demo_c(insn_name, num_inputs, desc)
    generate_build_script(insn_name)

    return (match_val, mask_val)


# ════════════════════════════════════════════════════════════════════════
#  Main
# ════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Automated Custom RISC-V Instruction Generator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
        Examples:
          %(prog)s --name my_pow --inputs 2 --desc "(a^b)^a"
          %(prog)s --name hw_status --inputs 0 --desc "read status"
          %(prog)s --name my_relu --inputs 1 --desc "max(0,x)"
          %(prog)s --name fused_mac --inputs 3 --desc "a*b+c"
          %(prog)s --batch instructions.txt
          %(prog)s --delete my_pow
          %(prog)s --delete my_pow fused_mac
          %(prog)s --list
          %(prog)s --scan 2
          %(prog)s                                    # Interactive

        Batch file format (one per line, max 4):
          # name    inputs   description
          my_pow      2      (a^b)^a
          fused_mac   3      a*b+c
          hw_status   0      read hardware status
          my_relu     1      max(0, x)
        """)
    )
    parser.add_argument('--name', help='Instruction name (e.g., my_pow)')
    parser.add_argument('--inputs', type=int, choices=[0, 1, 2, 3],
                        help='Number of inputs (0-3)')
    parser.add_argument('--desc', default='Custom instruction',
                        help='Short description')
    parser.add_argument('--scan', type=int, choices=[0, 1, 2, 3], metavar='N',
                        help='Show free opcodes for N inputs, then exit')
    parser.add_argument('--list', action='store_true',
                        help='List all auto-generated custom instructions')
    parser.add_argument('--delete', nargs='+', metavar='NAME',
                        help='Delete one or more custom instructions by name')
    parser.add_argument('--batch', metavar='FILE',
                        help='Batch file with multiple instructions (max 4)')
    parser.add_argument('--no-build', action='store_true',
                        help='Skip rebuilding the toolchain')
    parser.add_argument('--no-compile', action='store_true',
                        help='Skip compiling the demo')
    args = parser.parse_args()

    print("=" * 64)
    print("  RISC-V Custom Instruction Generator (Group 9)")
    print("  Supports: 0/1/2/3 inputs | batch | delete")
    print("=" * 64)

    # ── List mode ──
    if args.list:
        print("\nCustom instructions in toolchain:")
        list_custom_instructions()
        return

    # ── Scan-only mode ──
    if args.scan is not None:
        print(f"\nFree opcode slots for {args.scan}-input instructions:")
        count = show_all_free_opcodes(args.scan, limit=30)
        if count == 0:
            print("  No free slots found!")
        return

    # ── Delete mode ──
    if args.delete:
        print(f"\nDeleting {len(args.delete)} instruction(s)...")
        for name in args.delete:
            name = re.sub(r'[^a-z0-9_]', '_', name.lower())
            delete_instruction(name)

        if not args.no_build:
            print("\n" + "-" * 50)
            print("Rebuilding toolchain after deletion...")
            rebuild_binutils()
            rebuild_gcc()

        print("\n" + "=" * 64)
        print(f"  DONE — Deleted: {', '.join(args.delete)}")
        print("=" * 64)
        return

    # ── Batch mode ──
    if args.batch:
        batch_path = Path(args.batch)
        if not batch_path.exists():
            print(f"ERROR: Batch file not found: {batch_path}")
            sys.exit(1)

        instructions = parse_batch_file(batch_path)
        if not instructions:
            print("ERROR: No valid instructions in batch file")
            sys.exit(1)

        print(f"\nBatch mode: {len(instructions)} instruction(s)")
        print("-" * 50)

        results = []
        for insn_name, num_inputs, desc in instructions:
            result = add_instruction(insn_name, num_inputs, desc)
            if result:
                results.append((insn_name, num_inputs, result[0], result[1]))
            print()

        # Build once for all
        if not args.no_build:
            print("\n" + "-" * 50)
            print("Rebuilding toolchain (once for all instructions)...")
            if not rebuild_binutils():
                print("ERROR: binutils rebuild failed!")
                if input("Continue anyway? [y/N]: ").strip().lower() != 'y':
                    sys.exit(1)
            if not rebuild_gcc():
                print("ERROR: GCC rebuild failed!")
                if input("Continue anyway? [y/N]: ").strip().lower() != 'y':
                    sys.exit(1)

        # Compile all demos
        if not args.no_compile:
            print("\n" + "-" * 50)
            print("Compiling all demos...")
            for insn_name, _, _, _ in results:
                compile_and_dump(insn_name)

        print("\n" + "=" * 64)
        print(f"  BATCH DONE — Added {len(results)} instruction(s):")
        for name, ni, mv, mk in results:
            print(f"    {name:<20} inputs={ni}  MATCH=0x{mv:08x}  MASK=0x{mk:08x}")
        print("=" * 64)
        return

    # ── Single instruction mode (interactive or CLI) ──
    insn_name  = args.name
    num_inputs = args.inputs
    desc       = args.desc

    if not insn_name:
        insn_name = input("\nInstruction name (e.g., my_pow): ").strip()
        if not insn_name:
            print("Error: name cannot be empty")
            sys.exit(1)

    insn_name = re.sub(r'[^a-z0-9_]', '_', insn_name.lower())

    if num_inputs is None:
        inp = input("Number of inputs (0, 1, 2, or 3) [default=2]: ").strip()
        if inp in ('0', '1', '2', '3'):
            num_inputs = int(inp)
        else:
            num_inputs = 2

    if desc == 'Custom instruction':
        d = input(f"Short description [{desc}]: ").strip()
        if d:
            desc = d

    print("\n" + "-" * 50)
    result = add_instruction(insn_name, num_inputs, desc)
    if result is None:
        sys.exit(1)
    match_val, mask_val = result

    # Build
    if not args.no_build:
        print("\n" + "-" * 50)
        print("Rebuilding toolchain (incremental)...")
        if not rebuild_binutils():
            print("ERROR: binutils rebuild failed!")
            if input("Continue anyway? [y/N]: ").strip().lower() != 'y':
                sys.exit(1)
        if not rebuild_gcc():
            print("ERROR: GCC rebuild failed!")
            if input("Continue anyway? [y/N]: ").strip().lower() != 'y':
                sys.exit(1)
    else:
        print("\n[SKIP] Toolchain rebuild (--no-build)")

    # Compile
    if not args.no_compile:
        print("\n" + "-" * 50)
        print("Compiling demo and generating objdump...")
        compile_and_dump(insn_name)
    else:
        print("\n[SKIP] Demo compilation (--no-compile)")

    # Done
    cfg = INPUT_CONFIG[num_inputs]
    print("\n" + "=" * 64)
    print(f"  DONE — Custom instruction '{insn_name}' added!")
    print(f"  Format    : {cfg['format']} ({num_inputs} inputs)")
    print(f"  Demo      : custom_attn/demo/main_{insn_name}.c")
    print(f"  Builtin   : __builtin_riscv_{insn_name}()")
    print(f"  Encoding  : MATCH=0x{match_val:08x}  MASK=0x{mask_val:08x}")
    print("=" * 64)


if __name__ == "__main__":
    main()
