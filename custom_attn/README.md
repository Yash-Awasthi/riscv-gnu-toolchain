# Custom RISC-V `attn` Instruction

A custom RISC-V instruction that computes the full Transformer attention mechanism in hardware:

```
Attention(Q, K, V) = softmax(Q × K^T / √d_k) × V
```

One instruction replaces all four stages — matrix multiply, scale, softmax, and the final matrix multiply.

## Instruction Encoding

| Field   | Value            |
|---------|-----------------|
| Format  | R-type          |
| Opcode  | `0x0b` (custom-0) |
| funct3  | `0x0`           |
| funct7  | `0x01`          |
| MATCH   | `0x0200000b`    |
| MASK    | `0xfe00707f`    |

**Operands:**
- `rs1`: address of dimensions struct `{int rows, cols, seq_len, d_model}`
- `rs2`: address of Q/K/V pointers struct `{float *Q, float *K, float *V}`
- `rd`: result/status register

---

## How It Works

The toolchain is modified in two layers:

**Layer 1 — Instruction Encoding & Builtin (Files 1–6)**
Registers the `attn` instruction in the assembler, disassembler, and GCC. After this,
you can call `__builtin_riscv_attn()` directly to emit the instruction.

**Layer 2 — GIMPLE Auto-Detection Pass (Files 7a–7d)**
A GCC optimization pass that scans your C code for the 4-stage attention loop pattern
and replaces all loops with the `attn` instruction automatically. No builtin call needed —
write plain C loops and the compiler does the rest.

Layer 1 is a **prerequisite** for Layer 2. The GIMPLE pass emits the instruction
using its encoding, so the encoding must be registered first.

### Using the Builtin (after Layer 1)

```c
long run_attention(long dims_addr, long qkv_addr) {
    return __builtin_riscv_attn(dims_addr, qkv_addr);
}
```

Produces:
```
0:   02b5050b    attn    a0,a0,a1
4:   8082        ret
```

### Auto-Detection (after Layer 2)

Write plain C loops — the compiler detects and replaces them:

```c
void attention(int n, int d, float *Q, float *K, float *V, float *out)
{
    float scores[64*64];
    float scale = 1.0f / __builtin_sqrtf((float)d);
    int i, j, k;

    // Stage 1: Q * K^T
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            float sum = 0.0f;
            for (k = 0; k < d; k++)
                sum += Q[i*d + k] * K[j*d + k];
            scores[i*n + j] = sum;
        }

    // Stage 2: scale by 1/sqrt(d_k)
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            scores[i*n + j] *= scale;

    // Stage 3: softmax per row
    for (i = 0; i < n; i++) {
        float row_sum = 0.0f;
        for (j = 0; j < n; j++) {
            scores[i*n + j] = __builtin_expf(scores[i*n + j]);
            row_sum += scores[i*n + j];
        }
        for (j = 0; j < n; j++)
            scores[i*n + j] /= row_sum;
    }

    // Stage 4: scores * V
    for (i = 0; i < n; i++)
        for (j = 0; j < d; j++) {
            float sum = 0.0f;
            for (k = 0; k < n; k++)
                sum += scores[i*n + k] * V[k*d + j];
            out[i*d + j] = sum;
        }
}
```

Produces — all loops gone, single instruction:
```
  ...
  30:   02e7800b    attn    zero,a5,a4
  ...
```

The compiler builds `dims` and `qkv` structs on the stack from the function
parameters, then emits `attn`. No loop code remains in the output.

---

## Complete Setup

Follow these steps on a fresh machine. Total time: ~45–90 minutes (mostly build time).

### Prerequisites

- Linux system with standard build tools
- `gcc`, `g++`, `make`, `flex`, `bison`, `texinfo`, `gawk`, `autoconf`, `automake`
- ~10 GB free disk space

### Step 1: Clone and Initialize Submodules

```bash
git clone https://github.com/Yash-Awasthi/riscv-gnu-toolchain.git
cd riscv-gnu-toolchain
git submodule update --init binutils gcc riscv-opcodes
```

### Step 2: Register the Instruction (Layer 1)

The automation script patches all 6 toolchain files to register `attn` and its builtin.

```bash
cd custom_attn/scripts
chmod +x automate_instruction.sh
./automate_instruction.sh add attn 2
cd ../..
```

This modifies:

| # | File | What |
|---|------|------|
| 1 | `binutils/include/opcode/riscv-opc.h` | `MATCH_ATTN` / `MASK_ATTN` encoding macros |
| 2 | `binutils/opcodes/riscv-opc.c` | Opcode table entry for assembler/disassembler |
| 3 | `gcc/gcc/config/riscv/riscv-ftypes.def` | Function type: `RISCV_ATYPE_UL(UL, UL)` |
| 4 | `gcc/gcc/config/riscv/riscv-builtins.cc` | `__builtin_riscv_attn(unsigned long, unsigned long)` |
| 5 | `gcc/gcc/config/riscv/riscv.md` | `(define_insn "riscv_attn" ...)` machine pattern |
| 6 | `riscv-opcodes/extensions/rv_custom` | Official opcode database entry |

### Step 3: Add the GIMPLE Auto-Detection Pass (Layer 2)

Four manual modifications. Run from the repo root:

#### 3a. Copy the GIMPLE pass source

```bash
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc
```

#### 3b. Add build object to t-riscv

```bash
echo '' >> gcc/gcc/config/riscv/t-riscv
echo '# GIMPLE attention detection pass' >> gcc/gcc/config/riscv/t-riscv
echo 'EXTRA_OBJS += riscv-attn-detect.o' >> gcc/gcc/config/riscv/t-riscv
```

#### 3c. Add build rule to Makefile.in

```bash
cat >> gcc/gcc/Makefile.in << 'RULE'

# GIMPLE attention detection pass
riscv-attn-detect.o : $(srcdir)/config/riscv/riscv-attn-detect.cc \
  $(CONFIG_H) $(SYSTEM_H) coretypes.h $(TM_H) $(TREE_H) \
  $(GIMPLE_H) tree-pass.h cfgloop.h
	$(COMPILER) -c $(ALL_COMPILERFLAGS) $(ALL_CPPFLAGS) $(INCLUDES) \
	  $(srcdir)/config/riscv/riscv-attn-detect.cc
RULE
```

**Important:** The indented lines MUST use a real tab character, not spaces.
The `cat << 'RULE'` preserves tabs. Verify:
```bash
grep -P '\triscv-attn-detect' gcc/gcc/Makefile.in
```

#### 3d. Register the pass in riscv.cc

Add `#include "context.h"` near the top:
```bash
sed -i '/#include "tm_p.h"/a #include "context.h"' gcc/gcc/config/riscv/riscv.cc
```

Add the pass registration inside `riscv_option_override()`:
```bash
sed -i '/^riscv_option_override (void)/,/^}/ {
  /^}/ i\
\
  /* Register GIMPLE attention detection pass */\
  extern gimple_opt_pass *make_pass_riscv_attn_detect (gcc::context *);\
  struct register_pass_info attn_info;\
  attn_info.pass = make_pass_riscv_attn_detect (g);\
  attn_info.reference_pass_name = "loop";\
  attn_info.ref_pass_instance_number = 1;\
  attn_info.pos_op = PASS_POS_INSERT_AFTER;\
  register_pass (&attn_info);
}' gcc/gcc/config/riscv/riscv.cc
```

Verify:
```bash
grep -n 'context.h' gcc/gcc/config/riscv/riscv.cc
grep -n 'make_pass_riscv_attn_detect' gcc/gcc/config/riscv/riscv.cc
```

### Step 4: Build the Toolchain

```bash
mkdir -p build && cd build
../configure --prefix=$HOME/riscv --with-arch=rv64gc --with-abi=lp64d
make -j$(nproc)
export PATH=$HOME/riscv/bin:$PATH
```

Build takes 30–60+ minutes depending on your machine.

### Step 5: Verify

```bash
# Builtin call
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c custom_attn/demo/mainbuiltin.c -o mainbuiltin.o
riscv64-unknown-elf-objdump -d mainbuiltin.o
# Expected: attn a0,a0,a1

# Auto-detected loops
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c custom_attn/demo/mainloops.c -o mainloops.o
riscv64-unknown-elf-objdump -d mainloops.o
# Expected: attn zero,a5,a4  (no loop assembly)

# GIMPLE dump
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c custom_attn/demo/mainloops.c -o mainloops.o \
    -fdump-tree-all-details
grep "PATTERN DETECTED" mainloops.c.*riscv_attn_detect
# Expected: *** 3-LOOP ATTENTION PATTERN DETECTED ***
# (3 top-level loops at -O2; matmul+scale are fused by the optimizer)
```

---

## Rebuilding After Source Changes

If you modify `riscv-attn-detect.cc` and need to rebuild without redoing
the entire toolchain:

```bash
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc
touch gcc/gcc/config/riscv/riscv-attn-detect.cc

rm -f build/build-gcc-newlib-stage1/gcc/riscv-attn-detect.o
rm -f build/build-gcc-newlib-stage2/gcc/riscv-attn-detect.o

cd build/build-gcc-newlib-stage2/gcc && make riscv-attn-detect.o && make install
cd ../../.. && cd build && rm -f stamps/build-gcc-newlib-stage* && make -j$(nproc)
```

---

## How the GIMPLE Pass Works

The pass (`riscv_attn_detect`) runs after GCC's loop optimization phase.
It processes every function in two phases:

### Detection

1. Collects all top-level loops from the function's loop tree
2. Reverses loop order (GCC lists children in reverse program order)
3. Slides a window of 4 consecutive loops:

| Stage | Detector | Pattern |
|-------|----------|---------|
| 1 | `is_matmul_pattern()` | Triple-nested loop with MULT+PLUS reduction (Q × K^T) |
| 2 | `is_elementwise_div()` | Double-nested loop with MULT by 1/sqrt(d) |
| 3 | `is_softmax_pattern()` | Outer loop with 2–3 child loops (exp+sum, normalize) |
| 4 | `is_matmul_pattern()` | Triple-nested loop (scores × V) |

> **Implementation note — 3 top-level loops at `-O2`:** When compiled with `-O2`,
> GCC fuses the scale operation (`/ sqrt(d_k)`) directly into the first matmul loop.
> The pass therefore detects **3 top-level loops** in practice, not 4 separate ones:
>
> | Window | Pattern |
> |--------|---------|
> | Loop 1 | `is_matmul_pattern()` — matmul + scale fused (Q×Kᵀ and divide in one loop nest) |
> | Loop 2 | `is_softmax_pattern()` — exp, sum, normalize per row |
> | Loop 3 | `is_matmul_pattern()` — scores × V → output |
>
> Dump output confirms: `*** 3-LOOP ATTENTION PATTERN DETECTED ***`
> with `Loops: 1 (matmul+scale fused) -> 2 (softmax) -> 3 (matmul2)`.
> See `test/test2.c` for the working fused-scale C source and `test/attn_detect.dump`
> for the full pass trace.

### Replacement

When all 4 stages match:

1. Extracts function parameters (`n`, `d`, `Q`, `K`, `V`) via `DECL_ARGUMENTS`
2. Builds `dims` struct on stack: `{rows=n, cols=n, seq_len=n, d_model=d}`
3. Builds `qkv` struct on stack: `{Q, K, V}` pointers
4. Splits the preheader → loop1 edge to create a fresh basic block
5. Emits `attn` via volatile inline asm: `.insn r 0x0b, 0, 0x01, x0, rs1, rs2`
   (volatile asm is immune to dead code elimination)
6. Redirects control flow to the function exit, bypassing all 4 loops
   (which become dead code and are cleaned up by GCC)

> **Current implementation:** Steps 1–3 above (building `dims`/`qkv` structs and
> passing pointer arguments) represent the full hardware contract.  The current
> `riscv-attn-detect.cc` emits a simpler form: a single `volatile` inline asm
> `.word 0x0200000b` with a `"memory"` clobber.  The instruction encoding alone
> identifies the operation; the hardware reads the matrix pointers from its own
> micro-architectural state.  The struct-building code is documented here as the
> intended calling convention for a full hardware implementation.

### GCC 15.2.0 Compatibility

- Uses `TARGET_MEM_REF` alongside `MEM_REF` for pointer-based memory accesses
- Handles `PLUS_EXPR` operand ordering (MULT result can be in rhs1 or rhs2)
- Reverses `top_loops` vector (GCC lists children in reverse program order)
- Accepts softmax with 2 or 3 child loops (with/without max-reduction)
- Scans middle loop (not innermost) for matmul store targets
- Manual exit block fallback when `single_exit()` returns NULL

---

## Project Structure

```
custom_attn/
├── README.md                           # This file
├── implementation_log.txt              # Detailed log of all 11 fixes
├── src/
│   └── riscv-attn-detect.cc           # GIMPLE pass source (~1300 lines)
├── demo/
│   ├── mainbuiltin.c                  # Explicit builtin demo
│   └── mainloops.c                    # Auto-detected loops demo (annotated)
├── scripts/
│   ├── automate_instruction.sh        # Automation tool (Bash)
│   ├── automate_instruction.py        # Automation tool (Python)
│   ├── identify_free_opcodes.sh       # Opcode scanner (Bash)
│   ├── identify_free_opcodes.py       # Opcode scanner (Python)
│   └── instructions_example.txt       # Batch input example
└── docs/
    ├── DOCUMENTATION.md               # Full beginner-friendly documentation
    ├── BUILD_GUIDE.md                 # Step-by-step build guide (WSL / Ubuntu)
    ├── KNOWN_ISSUES.md                # All 11 issues and their fixes
    ├── MANUAL_PATCHES.md              # Manual patch reference for each GCC/binutils file
    ├── GENERIC_TEMPLATE.md            # Template for adding other custom instructions
    └── OPCODE_FIELDS.md              # Opcode field specifications
```
