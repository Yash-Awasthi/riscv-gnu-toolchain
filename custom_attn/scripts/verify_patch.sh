#!/usr/bin/env bash
# ================================================================
# verify_patch.sh — Verify a custom instruction is in all 6 files
# ================================================================
# Group 9 | RISC-V GNU Toolchain
#
# Usage:
#   ./verify_patch.sh <name> [name2 ...]
#   ./verify_patch.sh attn
#   ./verify_patch.sh attn my_relu fused_mac
#
# Exit code:
#   0  — all instructions pass all checks
#   1  — one or more instructions are incomplete
#
# What it checks (5 required + 1 optional):
#   1. binutils/include/opcode/riscv-opc.h    — MATCH_*, MASK_*, DECLARE_INSN
#   2. binutils/opcodes/riscv-opc.c           — riscv_opcodes[] table entry
#   3. gcc/gcc/config/riscv/riscv-ftypes.def  — DEF_RISCV_FTYPE definition
#   4. gcc/gcc/config/riscv/riscv-builtins.cc — DIRECT_BUILTIN registration
#   5. gcc/gcc/config/riscv/riscv.md          — UNSPEC constant + define_insn
#   6. riscv-opcodes/extensions/rv_custom      — riscv-opcodes format (optional)
# ================================================================

set -euo pipefail

# ── Colors ────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

pass_icon="${GREEN}✓${NC}"
fail_icon="${RED}✗${NC}"
warn_icon="${YELLOW}?${NC}"

# ── Paths ─────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

OPC_H="$REPO_ROOT/binutils/include/opcode/riscv-opc.h"
OPC_C="$REPO_ROOT/binutils/opcodes/riscv-opc.c"
FTYPES_DEF="$REPO_ROOT/gcc/gcc/config/riscv/riscv-ftypes.def"
BUILTINS_CC="$REPO_ROOT/gcc/gcc/config/riscv/riscv-builtins.cc"
RISCV_MD="$REPO_ROOT/gcc/gcc/config/riscv/riscv.md"
RV_CUSTOM="$REPO_ROOT/riscv-opcodes/extensions/rv_custom"

to_upper() { echo "$1" | tr '[:lower:]' '[:upper:]'; }

# ── Arg check ─────────────────────────────────────────────────────
if [[ $# -lt 1 ]] || [[ "$1" == "-h" ]] || [[ "$1" == "--help" ]]; then
    echo "Usage: $0 <instruction_name> [name2 ...]"
    echo ""
    echo "Examples:"
    echo "  $0 attn"
    echo "  $0 attn my_relu fused_mac"
    exit 0
fi

# ── Header ────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}================================================================${NC}"
echo -e "${BOLD}  RISC-V Custom Instruction Patch Verifier (Group 9)${NC}"
echo -e "${BOLD}================================================================${NC}"
echo ""
echo -e "  Repo root : ${CYAN}${REPO_ROOT}${NC}"
echo ""

# ── Check required files exist ────────────────────────────────────
for f in "$OPC_H" "$OPC_C" "$FTYPES_DEF" "$BUILTINS_CC" "$RISCV_MD"; do
    if [[ ! -f "$f" ]]; then
        echo -e "  ${RED}ERROR:${NC} Required file not found:"
        echo "    $f"
        echo ""
        echo "  Make sure submodules are initialized:"
        echo "    git submodule update --init binutils gcc"
        exit 1
    fi
done

# ── Verify each instruction ───────────────────────────────────────
overall_result=0

for name in "$@"; do
    # Sanitize: lowercase, alphanumeric + underscore
    name=$(echo "$name" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9_]/_/g')
    upper=$(to_upper "$name")

    echo -e "  ${BOLD}Instruction: ${CYAN}${name}${NC}  (MATCH_${upper})"
    echo "  ──────────────────────────────────────────────────────────"

    inst_result=0

    # ── File 1: riscv-opc.h ──────────────────────────────────────
    f1_match=false; f1_mask=false; f1_decl=false
    grep -q "MATCH_${upper}" "$OPC_H" 2>/dev/null && f1_match=true
    grep -q "MASK_${upper}"  "$OPC_H" 2>/dev/null && f1_mask=true
    grep -q "DECLARE_INSN(${name}," "$OPC_H" 2>/dev/null && f1_decl=true

    # Extract MATCH value if present
    match_val=$(grep -oP "#define MATCH_${upper}\s+\K0x[0-9a-fA-F]+" "$OPC_H" 2>/dev/null || echo "not found")

    if $f1_match && $f1_mask && $f1_decl; then
        echo -e "  ${pass_icon} riscv-opc.h     MATCH_${upper}=${match_val}, MASK_${upper}, DECLARE_INSN"
    else
        echo -e "  ${fail_icon} riscv-opc.h     INCOMPLETE:"
        $f1_match || echo "       ✗ MATCH_${upper} missing"
        $f1_mask  || echo "       ✗ MASK_${upper} missing"
        $f1_decl  || echo "       ✗ DECLARE_INSN(${name}, ...) missing"
        inst_result=1
    fi

    # ── File 2: riscv-opc.c ──────────────────────────────────────
    if grep -q "\"${name}\"" "$OPC_C" 2>/dev/null; then
        # Extract format string
        fmt=$(grep "\"${name}\"" "$OPC_C" | grep -oP '"\K[dstr,]+(?=",\s*MATCH)' | head -1 || echo "?")
        echo -e "  ${pass_icon} riscv-opc.c     {\"${name}\", ..., operands=\"${fmt}\", MATCH_${upper}}"
    else
        echo -e "  ${fail_icon} riscv-opc.c     Entry for \"${name}\" not found in riscv_opcodes[]"
        inst_result=1
    fi

    # ── File 3: riscv-ftypes.def ─────────────────────────────────
    # Check for any MARKER-tagged ftype or at minimum a DI ftype with 2+ args
    ftype_found=false
    if grep -q "Custom instruction ftype" "$FTYPES_DEF" 2>/dev/null; then
        ftype_found=true
        ftype_line=$(grep -A1 "Custom instruction ftype" "$FTYPES_DEF" | tail -1)
        echo -e "  ${pass_icon} riscv-ftypes.def  ${ftype_line}"
    elif grep -q "DEF_RISCV_FTYPE (2," "$FTYPES_DEF" 2>/dev/null; then
        ftype_found=true
        echo -e "  ${pass_icon} riscv-ftypes.def  DEF_RISCV_FTYPE(2,...) present"
    fi
    if ! $ftype_found; then
        echo -e "  ${fail_icon} riscv-ftypes.def  No custom DEF_RISCV_FTYPE found"
        inst_result=1
    fi

    # ── File 4: riscv-builtins.cc ────────────────────────────────
    if grep -q "DIRECT_BUILTIN (${name}," "$BUILTINS_CC" 2>/dev/null; then
        ftype_used=$(grep "DIRECT_BUILTIN (${name}," "$BUILTINS_CC" | grep -oP 'RISCV_\w+' | tail -1 || echo "?")
        echo -e "  ${pass_icon} riscv-builtins.cc DIRECT_BUILTIN(${name}, ${ftype_used})"
    else
        echo -e "  ${fail_icon} riscv-builtins.cc DIRECT_BUILTIN(${name}, ...) not found"
        echo "       → __builtin_riscv_${name}() will not be available in C code"
        inst_result=1
    fi

    # ── File 5: riscv.md ─────────────────────────────────────────
    md_unspec=false; md_insn=false
    grep -q "UNSPEC_${upper}" "$RISCV_MD" 2>/dev/null && md_unspec=true
    grep -q "riscv_${name}"  "$RISCV_MD" 2>/dev/null && md_insn=true

    if $md_unspec && $md_insn; then
        echo -e "  ${pass_icon} riscv.md         UNSPEC_${upper} + define_insn \"riscv_${name}\""
    else
        echo -e "  ${fail_icon} riscv.md         INCOMPLETE:"
        $md_unspec || echo "       ✗ UNSPEC_${upper} missing from (define_c_enum \"unspec\" [...])"
        $md_insn   || echo "       ✗ (define_insn \"riscv_${name}\" ...) missing"
        inst_result=1
    fi

    # ── File 6: rv_custom (optional) ─────────────────────────────
    if [[ -f "$RV_CUSTOM" ]] && grep -q "^${name} " "$RV_CUSTOM" 2>/dev/null; then
        rv_line=$(grep "^${name} " "$RV_CUSTOM" | head -1)
        echo -e "  ${pass_icon} rv_custom        ${rv_line}"
    else
        echo -e "  ${warn_icon} rv_custom        Not found (optional — won't break the build)"
    fi

    # ── Per-instruction summary ───────────────────────────────────
    echo "  ──────────────────────────────────────────────────────────"
    if [[ $inst_result -eq 0 ]]; then
        echo -e "  ${GREEN}${BOLD}PASS${NC}  ${name} — all 5 required files patched correctly"
    else
        echo -e "  ${RED}${BOLD}FAIL${NC}  ${name} — incomplete (see above)"
        echo "        Fix with: ./automate_instruction.sh add ${name} <inputs>"
        overall_result=1
    fi
    echo ""
done

# ── Overall summary ───────────────────────────────────────────────
if [[ $overall_result -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}All instructions verified successfully.${NC}"
else
    echo -e "  ${RED}${BOLD}Some instructions are incomplete — see details above.${NC}"
fi
echo ""
exit $overall_result
