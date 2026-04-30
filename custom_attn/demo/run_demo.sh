#!/usr/bin/env bash
# run_demo.sh — One-shot build, run, and verify script
#
# Usage:
#   chmod +x run_demo.sh
#   ./run_demo.sh
#
# What it does:
#   1. Compiles demo_attn.c with the patched toolchain
#   2. Runs the binary under QEMU
#   3. Dumps the assembly and highlights the `attn` instruction
#   4. Generates the .s listing for documentation screenshots
#
set -e

CC="riscv64-unknown-elf-gcc"
OBJDUMP="riscv64-unknown-elf-objdump"
CFLAGS="-O2 -march=rv64gc -mabi=lp64d"

# ── 1. Sanity check ──────────────────────────────────────────────────────────
if ! command -v "$CC" &>/dev/null; then
    echo "ERROR: $CC not found."
    echo "Build the patched riscv-gnu-toolchain first:"
    echo "  cd /path/to/riscv-gnu-toolchain"
    echo "  ./configure --prefix=\$RISCV --enable-multilib"
    echo "  make -j\$(nproc)"
    exit 1
fi

echo "════════════════════════════════════════════════════"
echo " RISC-V Custom attn Instruction — Demo Runner"
echo "════════════════════════════════════════════════════"
echo ""

# ── 2. Compile binary ────────────────────────────────────────────────────────
echo "[ 1/4 ] Compiling demo_attn.c ..."
$CC $CFLAGS demo_attn.c -o demo_attn -lm
echo "        → demo_attn built"
echo ""

# ── 3. Generate assembly listing ────────────────────────────────────────────
echo "[ 2/4 ] Generating assembly listing (demo_attn.s) ..."
$CC $CFLAGS -S demo_attn.c -o demo_attn.s
echo "        → demo_attn.s written"
echo ""

# ── 4. Verify attn instruction is present ───────────────────────────────────
echo "[ 3/4 ] Checking for custom instruction in object ..."
ATTN_HITS=$($OBJDUMP -d demo_attn | grep -c "attn" || true)
if [ "$ATTN_HITS" -gt 0 ]; then
    echo "        ✓ Found $ATTN_HITS occurrence(s) of 'attn' in disassembly"
    echo ""
    echo "        Disassembly of attention():"
    $OBJDUMP -d demo_attn | awk '/^[0-9a-f]+ <attention>:/,/^$/' \
        | grep --color=always -E "attn|$"
else
    echo "        ✗ WARNING: 'attn' instruction NOT found."
    echo "          Possible causes:"
    echo "            - Compiler patch not applied correctly"
    echo "            - Wrong GCC in PATH (check: which riscv64-unknown-elf-gcc)"
    echo "            - Optimization level not -O2 (GIMPLE pass requires >= -O2)"
fi
echo ""

# ── 5. Run under QEMU ────────────────────────────────────────────────────────
echo "[ 4/4 ] Running under qemu-riscv64 ..."
echo "────────────────────────────────────────────────────"
if command -v qemu-riscv64 &>/dev/null; then
    qemu-riscv64 ./demo_attn
else
    echo "  qemu-riscv64 not found — skipping execution."
    echo "  Install with: sudo apt install qemu-user   (Debian/Ubuntu)"
    echo "                brew install qemu            (macOS)"
fi
echo "────────────────────────────────────────────────────"
echo ""
echo "Done. Files produced:"
echo "  demo_attn    — RISC-V ELF binary"
echo "  demo_attn.s  — Annotated assembly listing"
