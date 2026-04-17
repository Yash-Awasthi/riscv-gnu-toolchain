#!/usr/bin/env python3
"""
identify_free_opcodes.py — Scan for free RISC-V custom opcode slots
===================================================================
Group 9 | RISC-V GNU Toolchain

Scans riscv-opc.h and riscv-opcodes/extensions/ to find opcode slots
that are NOT used by any existing instruction.

Usage:
  python3 identify_free_opcodes.py                    # Show all free slots (summary)
  python3 identify_free_opcodes.py --detailed         # Show all free funct3/funct7 combos
  python3 identify_free_opcodes.py --inputs 2         # Free slots for 2-input (R-type)
  python3 identify_free_opcodes.py --inputs 3         # Free slots for 3-input (R4-type)
  python3 identify_free_opcodes.py --check 0x0200000b # Check if a specific MATCH is free

Requirements:
  - Run from the custom_attn/ directory (or provide --repo-root)
  - binutils submodule must be initialized (for riscv-opc.h)
"""

import argparse
import re
import sys
from pathlib import Path

# ── Paths ────────────────────────────────────────────────────────────
SCRIPT_DIR = Path(__file__).resolve().parent     # custom_attn/scripts/
REPO_ROOT  = SCRIPT_DIR.parent.parent            # riscv-gnu-toolchain/

CUSTOM_BASES = {
    "custom-0": 0x0B,
    "custom-1": 0x2B,
    "custom-2": 0x5B,
    "custom-3": 0x7B,
}

# bits[6:2] values for riscv-opcodes format
BASE_BITS = {0x0B: 0x02, 0x2B: 0x0A, 0x5B: 0x16, 0x7B: 0x1E}


def find_opc_h():
    """Locate riscv-opc.h in the repo."""
    candidates = [
        REPO_ROOT / "binutils" / "include" / "opcode" / "riscv-opc.h",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def find_riscv_opcodes_dir():
    """Locate riscv-opcodes/extensions/ directory."""
    candidates = [
        REPO_ROOT / "riscv-opcodes" / "extensions",
        Path.home() / "riscv-opcodes" / "extensions",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def parse_match_values_from_opc_h(opc_h_path):
    """Extract all MATCH_* hex values from riscv-opc.h."""
    matches = {}
    pattern = re.compile(r'#define\s+(MATCH_\w+)\s+(0x[0-9a-fA-F]+)')
    with open(opc_h_path, 'r') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                name = m.group(1)
                val = int(m.group(2), 16)
                matches[val] = name
    return matches


def parse_used_bases_from_extensions(ext_dir):
    """Extract used opcode base values (bits[6:2]) from riscv-opcodes/extensions/."""
    used = set()
    pattern = re.compile(r'6\.\.2=0x([0-9a-fA-F]+)')
    for f in ext_dir.iterdir():
        if f.is_file() and not f.name.startswith('.'):
            for line in f.read_text().splitlines():
                if line.strip().startswith('#'):
                    continue
                m = pattern.search(line)
                if m:
                    used.add(int(m.group(1), 16))
    return used


def compute_match_mask(base_opcode, funct3, funct7=0, funct2=0, r4type=False):
    """Compute MATCH and MASK for an R-type or R4-type encoding."""
    if not r4type:
        match_val = base_opcode | (funct3 << 12) | (funct7 << 25)
        mask_val  = 0xFE00707F
    else:
        match_val = base_opcode | (funct3 << 12) | (funct2 << 25)
        mask_val  = 0x0600707F
    return match_val, mask_val


def find_free_slots(existing_matches, num_inputs=2, limit=None):
    """Find free opcode slots across all custom bases."""
    r4type = (num_inputs == 3)
    free = []

    for base_name, base_opc in CUSTOM_BASES.items():
        if not r4type:
            for f3 in range(8):
                for f7 in range(128):
                    match_val, mask_val = compute_match_mask(base_opc, f3, funct7=f7)
                    if match_val not in existing_matches:
                        free.append({
                            "base": base_name,
                            "base_opc": base_opc,
                            "funct3": f3,
                            "funct7": f7,
                            "match": match_val,
                            "mask": mask_val,
                        })
                        if limit and len(free) >= limit:
                            return free
        else:
            for f3 in range(8):
                for f2 in range(4):
                    match_val, mask_val = compute_match_mask(base_opc, f3, funct2=f2, r4type=True)
                    if match_val not in existing_matches:
                        free.append({
                            "base": base_name,
                            "base_opc": base_opc,
                            "funct3": f3,
                            "funct2": f2,
                            "match": match_val,
                            "mask": mask_val,
                        })
                        if limit and len(free) >= limit:
                            return free

    return free


def print_summary(existing_matches):
    """Print a summary of free slots per base per input type."""
    print("\n╔══════════════════════════════════════════════════════════════╗")
    print("║         Free RISC-V Custom Opcode Slots — Summary          ║")
    print("╚══════════════════════════════════════════════════════════════╝\n")

    for label, r4 in [("R-type (0/1/2 inputs)", False), ("R4-type (3 inputs)", True)]:
        print(f"  {label}:")
        print(f"  {'Base':<14} {'Total Possible':<18} {'Used':<8} {'Free':<8}")
        print(f"  {'-'*48}")

        total_free = 0
        for base_name, base_opc in CUSTOM_BASES.items():
            if not r4:
                total = 8 * 128  # funct3 × funct7
                used = 0
                for f3 in range(8):
                    for f7 in range(128):
                        m, _ = compute_match_mask(base_opc, f3, funct7=f7)
                        if m in existing_matches:
                            used += 1
            else:
                total = 8 * 4  # funct3 × funct2
                used = 0
                for f3 in range(8):
                    for f2 in range(4):
                        m, _ = compute_match_mask(base_opc, f3, funct2=f2, r4type=True)
                        if m in existing_matches:
                            used += 1

            free = total - used
            total_free += free
            print(f"  {base_name:<14} {total:<18} {used:<8} {free:<8}")

        print(f"  {'TOTAL':<14} {'':<18} {'':<8} {total_free:<8}")
        print()


def print_detailed(existing_matches, num_inputs, limit=30):
    """Print detailed list of free slots."""
    r4type = (num_inputs == 3)
    funct_label = "funct2" if r4type else "funct7"
    input_label = f"{num_inputs}-input ({'R4-type' if r4type else 'R-type'})"

    print(f"\n  Free opcode slots for {input_label} instructions")
    print(f"  (showing first {limit}):\n")
    print(f"  {'Base':<14} {'funct3':<8} {funct_label:<10} {'MATCH':<14} {'MASK':<14}")
    print(f"  {'-'*60}")

    free = find_free_slots(existing_matches, num_inputs, limit=limit)
    for slot in free:
        fv = slot.get("funct2", slot.get("funct7", 0))
        print(f"  {slot['base']:<14} {slot['funct3']:<8} {fv:<10} "
              f"0x{slot['match']:08x}    0x{slot['mask']:08x}")

    if len(free) >= limit:
        print(f"  ... (showing first {limit} of many)")

    print(f"\n  Total shown: {len(free)}")


def check_match_value(existing_matches, value):
    """Check if a specific MATCH value is free."""
    if value in existing_matches:
        print(f"\n  0x{value:08x} is TAKEN by: {existing_matches[value]}")
        return False
    else:
        print(f"\n  0x{value:08x} is FREE")

        # Decode the encoding
        opcode = value & 0x7F
        funct3 = (value >> 12) & 0x7
        funct7 = (value >> 25) & 0x7F

        base_name = "unknown"
        for name, opc in CUSTOM_BASES.items():
            if opc == opcode:
                base_name = name
                break

        print(f"  Decoding: opcode=0x{opcode:02x} ({base_name}) funct3={funct3} funct7={funct7}")
        return True


def main():
    parser = argparse.ArgumentParser(
        description="Scan for free RISC-V custom opcode slots",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  %(prog)s                        Show summary of all free slots
  %(prog)s --detailed             Show first 30 free R-type slots
  %(prog)s --inputs 3 --detailed  Show first 30 free R4-type slots
  %(prog)s --check 0x0200000b     Check if a specific MATCH is free
  %(prog)s --limit 50             Show more slots in detailed mode
"""
    )
    parser.add_argument('--inputs', type=int, choices=[0, 1, 2, 3], default=2,
                        help='Number of inputs (0-2 = R-type, 3 = R4-type)')
    parser.add_argument('--detailed', action='store_true',
                        help='Show detailed list of free slots')
    parser.add_argument('--limit', type=int, default=30,
                        help='Max slots to show in detailed mode (default: 30)')
    parser.add_argument('--check', type=str, metavar='0xHEX',
                        help='Check if a specific MATCH value is free')
    parser.add_argument('--repo-root', type=str,
                        help='Path to riscv-gnu-toolchain root (default: auto-detect)')
    args = parser.parse_args()

    if args.repo_root:
        global REPO_ROOT
        REPO_ROOT = Path(args.repo_root)

    # Find riscv-opc.h
    opc_h = find_opc_h()
    if not opc_h:
        print("ERROR: riscv-opc.h not found!")
        print("  Make sure binutils submodule is initialized:")
        print("    git submodule update --init binutils")
        sys.exit(1)

    print(f"  Source: {opc_h}")

    # Parse existing MATCH values
    existing = parse_match_values_from_opc_h(opc_h)
    print(f"  Found {len(existing)} existing MATCH values in riscv-opc.h")

    # Also check riscv-opcodes/extensions/ if available
    ext_dir = find_riscv_opcodes_dir()
    if ext_dir:
        used_bases = parse_used_bases_from_extensions(ext_dir)
        print(f"  Found {len(used_bases)} used base opcodes in riscv-opcodes/extensions/")

    # Check mode
    if args.check:
        try:
            val = int(args.check, 16)
        except ValueError:
            print(f"ERROR: Invalid hex value: {args.check}")
            sys.exit(1)
        check_match_value(existing, val)
        return

    # Summary or detailed
    if args.detailed:
        print_detailed(existing, args.inputs, limit=args.limit)
    else:
        print_summary(existing)

    # Show the first free slot recommendation
    free = find_free_slots(existing, args.inputs, limit=1)
    if free:
        slot = free[0]
        fv = slot.get("funct2", slot.get("funct7", 0))
        fl = "funct2" if args.inputs == 3 else "funct7"
        print(f"  Recommended next slot:")
        print(f"    Base: {slot['base']} (0x{slot['base_opc']:02x})")
        print(f"    funct3={slot['funct3']}, {fl}={fv}")
        print(f"    MATCH=0x{slot['match']:08x}  MASK=0x{slot['mask']:08x}")
    else:
        print("  WARNING: No free slots found!")


if __name__ == "__main__":
    main()
