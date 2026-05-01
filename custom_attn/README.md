# custom_attn — RISC-V Custom Attention Instruction

**Author:** Yash Awasthi, Group 9
**Instruction:** `attn rd, rs1, rs2`
**Encoding:** R-type, opcode `0x0b` (custom-0), funct3 `0x0`, funct7 `0x01`
**Hex pattern:** `0x0200000b` (for `attn a0, a0, a1` → `0x02b5050b`)

## What this does

Adds a single hardware instruction that computes the full transformer
attention kernel:

```
Attention(Q, K, V) = softmax(Q × Kᵀ / √dk) × V
```

The instruction is exposed as:
- A GCC built-in: `__builtin_riscv_attn(dims_addr, qkv_addr)`
- A GIMPLE tree pass (`riscv_attn_detect`) that **auto-detects** the
  4-stage attention loop pattern in plain C code and replaces it with
  the built-in call.

## Status

| Component | Status |
|-----------|--------|
| Assembler / disassembler (`riscv-opc.{h,c}`) | Working |
| GCC built-in (`__builtin_riscv_attn`) | Working |
| Instruction pattern in `riscv.md` | Working |
| GIMPLE pattern-detection pass | Working (detection confirmed) |
| GIMPLE CFG replacement (loop removal) | Disabled — see Issue 6 |

### Confirmed working

```
$ echo '__builtin_riscv_attn(a,b);' | riscv64-unknown-elf-gcc -O2 -x c -
$ riscv64-unknown-elf-objdump -d a.out | grep attn
   ...  0200000b    attn  a0,a0,a1
```

GIMPLE detection:
```
$ riscv64-unknown-elf-gcc -O2 -fdump-tree-riscv_attn_detect attention.c
# dump file shows: "Attention pattern detected in 'attention'"
```

## Repository layout

```
custom_attn/
├── README.md              ← this file
├── src/
│   ├── riscv-attn-detect.cc   ← GIMPLE pass (copy from gcc/gcc/config/riscv/)
│   └── automate.sh            ← original automation script (historical)
└── docs/
    ├── BUILD_GUIDE.md         ← step-by-step WSL build instructions
    ├── KNOWN_ISSUES.md        ← all 9 issues encountered + fixes
    └── MANUAL_PATCHES.md      ← exact diffs for every modified source file
```

## Quick build (summary)

See `docs/BUILD_GUIDE.md` for the full guide.  The short version:

```bash
cd ~/riscv-gnu-toolchain
git submodule update --init gcc binutils-gdb

# Apply manual patches (see docs/MANUAL_PATCHES.md)

mkdir -p build && cd build
../configure --prefix=/opt/riscv --with-arch=rv64gc --with-abi=lp64d
make -j$(nproc) 2>&1 | tee build.log
```

## Key encoding reference

```
attn rd, rs1, rs2
 31      25 24  20 19  15 14  12 11   7 6      0
[ 0000001 | rs2  | rs1  | 000  |  rd  | 0001011 ]
  funct7    rs2    rs1   funct3   rd    opcode(custom-0)

MATCH_ATTN = 0x0200000b
MASK_ATTN  = 0xfe00707f
```
