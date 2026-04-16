#!/usr/bin/env bash
# =============================================================================
# build_and_dump.sh
#
# Compile custom_attn/demo/main.c with the custom RISC-V toolchain,
# then disassemble with objdump to verify the 'attn' instruction
# appears in the binary as a proper mnemonic (not raw hex).
#
# The C program uses __builtin_riscv_attn() — NO inline assembly.
#
# Usage:
#   export RISCV_PREFIX=$HOME/riscv_custom
#   bash custom_attn/demo/build_and_dump.sh
# =============================================================================

set -euo pipefail

# ---------- configuration ----------------------------------------------------
RISCV_PREFIX="${RISCV_PREFIX:-$HOME/riscv_custom}"
GCC="${RISCV_PREFIX}/bin/riscv64-unknown-elf-gcc"
OBJDUMP="${RISCV_PREFIX}/bin/riscv64-unknown-elf-objdump"
OBJCOPY="${RISCV_PREFIX}/bin/riscv64-unknown-elf-objcopy"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/main.c"
OBJ="${SCRIPT_DIR}/main.o"
BIN="${SCRIPT_DIR}/main.bin"
HEX="${SCRIPT_DIR}/main_hexdump.txt"
DUMP="${SCRIPT_DIR}/main_objdump.txt"

# ---------- sanity checks ----------------------------------------------------
if [[ ! -x "$GCC" ]]; then
    echo "[ERROR] Custom RISC-V GCC not found at: $GCC"
    echo "        Build the toolchain first (see README.md)."
    echo ""
    echo "        Quick build:"
    echo "          cd riscv-gnu-toolchain"
    echo "          mkdir build && cd build"
    echo "          ../configure --prefix=\$HOME/riscv_custom --with-arch=rv64imac --with-abi=lp64"
    echo "          make -j\$(nproc) && make install"
    exit 1
fi

echo "============================================================"
echo "  RISC-V Custom Instruction — attn (Attention Mechanism)"
echo "  Group 9 — Build & Dump"
echo "============================================================"
echo ""

# ---------- compile ----------------------------------------------------------
echo "==> [STEP 1] Compiling main.c (uses __builtin_riscv_attn, NO inline asm)"
echo "    ${GCC} -O2 -march=rv64imac -mabi=lp64 -c ${SRC} -o ${OBJ}"
"$GCC" -O2 -march=rv64imac -mabi=lp64 -c "$SRC" -o "$OBJ"
echo "    [OK] main.o generated ($(wc -c < "$OBJ") bytes)"
echo ""

# ---------- disassemble ------------------------------------------------------
echo "==> [STEP 2] Disassembly (objdump -d):"
echo "    ${OBJDUMP} -d ${OBJ}"
echo "------------------------------------------------------------"
"$OBJDUMP" -d "$OBJ" | tee "$DUMP"
echo "------------------------------------------------------------"
echo "    [OK] Disassembly saved to ${DUMP}"
echo ""

# ---------- binary extract + hex dump ----------------------------------------
echo "==> [STEP 3] Extracting .text section and generating hex dump..."
if [[ -x "$OBJCOPY" ]]; then
    "$OBJCOPY" -O binary -j .text "$OBJ" "$BIN" 2>/dev/null || true
    if [[ -f "$BIN" ]]; then
        od -An -tx4 -w4 -v "$BIN" > "$HEX" 2>/dev/null || true
        echo "    [OK] Hex dump saved to ${HEX}"
    fi
else
    echo "    [SKIP] objcopy not available"
fi
echo ""

# ---------- verify attn mnemonic appears -------------------------------------
echo "==> [STEP 4] Verifying 'attn' mnemonic in disassembly..."
echo ""
if grep -q "attn" "$DUMP" 2>/dev/null; then
    echo "    ┌─────────────────────────────────────────────────────┐"
    echo "    │  [PASS] 'attn' instruction found in disassembly!   │"
    echo "    └─────────────────────────────────────────────────────┘"
    echo ""
    echo "    Matching lines:"
    grep "attn" "$DUMP" | while read -r line; do
        echo "      $line"
    done
    echo ""
    echo "    Encoding: 0x00B5050B = attn a0, a0, a1"
    echo "      funct7=0000000  rs2=a1(01011)  rs1=a0(01010)  funct3=000  rd=a0(01010)  opcode=0001011"
else
    echo "    [FAIL] 'attn' mnemonic NOT found in disassembly."
    echo "    Make sure you built with the modified binutils."
    exit 1
fi

echo ""
echo "============================================================"
echo "  Build & dump complete. Custom instruction verified."
echo "============================================================"
