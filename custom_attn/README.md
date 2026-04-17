# Custom RISC-V `attn` Instruction

A custom RISC-V instruction implementing the Transformer attention mechanism:

```
Attention(Q, K, V) = softmax(Q × K^T / √d_k) × V
```

Encoded as a single R-type instruction: `attn rd, rs1, rs2`

## Instruction Encoding

| Field   | Value        |
|---------|-------------|
| Opcode  | `0x0b` (custom-0) |
| funct3  | `0x0`       |
| funct7  | `0x01`      |
| MATCH   | `0x0200000b` |
| MASK    | `0xfe00707f` |

**Operands:**
- `rs1`: address of dimensions struct `{int rows, cols, seq_len, d_model}`
- `rs2`: address of Q/K/V pointers struct `{float *Q, float *K, float *V}`
- `rd`: result/status register

## Two Paths to Hardware

### Path A: Explicit Builtin

Call `__builtin_riscv_attn()` directly:

```c
long run_attention(long dims_addr, long qkv_addr) {
    return __builtin_riscv_attn(dims_addr, qkv_addr);
}
```

Verified output:
```
0:   02b5050b    attn    a0,a0,a1
4:   8082        ret
```

### Path B: Automatic Detection (GIMPLE Pass)

Write plain C loops — the compiler detects the 4-stage attention pattern
and replaces all loops with a single `attn` instruction automatically:

```c
void attention(int n, int d, float *Q, float *K, float *V, float *out)
{
    float scores[64*64];
    float scale = 1.0f / __builtin_sqrtf((float)d);
    int i, j, k;

    // Stage 1: Q * K^T (matrix multiply)
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            float sum = 0.0f;
            for (k = 0; k < d; k++)
                sum += Q[i*d + k] * K[j*d + k];
            scores[i*n + j] = sum;
        }

    // Stage 2: element-wise scale by 1/sqrt(d_k)
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

    // Stage 4: scores * V (matrix multiply)
    for (i = 0; i < n; i++)
        for (j = 0; j < d; j++) {
            float sum = 0.0f;
            for (k = 0; k < n; k++)
                sum += scores[i*n + k] * V[k*d + j];
            out[i*d + j] = sum;
        }
}
```

Verified output — all loops replaced by a single instruction:
```
  ...
  30:   02e7800b    attn    zero,a5,a4
  ...
```

The compiler builds the `dims` and `qkv` structs on the stack from the
function parameters, then emits the `attn` instruction. No loop code
remains in the output.

---

## Complete Setup: Clone to Working

Follow these steps on a fresh machine to get both paths working.
Total time: ~45–90 minutes (mostly build time).

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

### Step 2: Apply Files 1–6 (Builtin Encoding) — Automated

The automation script patches all 6 toolchain source files to register
the `attn` instruction encoding and `__builtin_riscv_attn()` builtin.

```bash
cd custom_attn
chmod +x automate_instruction.sh
./automate_instruction.sh add attn 2
cd ..
```

This modifies:

| # | File | What |
|---|------|------|
| 1 | `binutils/include/opcode/riscv-opc.h` | MATCH/MASK encoding macros |
| 2 | `binutils/opcodes/riscv-opc.c` | Opcode table entry |
| 3 | `gcc/gcc/config/riscv/riscv-ftypes.def` | Function type signature |
| 4 | `gcc/gcc/config/riscv/riscv-builtins.cc` | `__builtin_riscv_attn` registration |
| 5 | `gcc/gcc/config/riscv/riscv.md` | Machine description pattern |
| 6 | `riscv-opcodes/extensions/rv_custom` | Official opcode registration |

After this step, **Path A (explicit builtin) will work** once built.
Files 1–6 are also a **prerequisite** for Path B — the GIMPLE pass depends
on the instruction encoding being registered.

### Step 3: Apply GIMPLE Auto-Detection Pass — Manual

The GIMPLE pass requires 4 modifications. Run these from the repo root:

#### 3a. Copy the GIMPLE pass source file

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

**Add `#include "context.h"` near the top includes:**
```bash
sed -i '/#include "tm_p.h"/a #include "context.h"' gcc/gcc/config/riscv/riscv.cc
```

**Add the extern declaration and register_pass() call inside `riscv_option_override()`:**
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

**Verify:**
```bash
grep -n 'context.h' gcc/gcc/config/riscv/riscv.cc
grep -n 'make_pass_riscv_attn_detect' gcc/gcc/config/riscv/riscv.cc
grep -n 'register_pass' gcc/gcc/config/riscv/riscv.cc
```

### Step 4: Configure and Build

```bash
mkdir -p build && cd build
../configure --prefix=$HOME/riscv --with-arch=rv64gc --with-abi=lp64d
make -j$(nproc)
export PATH=$HOME/riscv/bin:$PATH
```

Build takes 30–60+ minutes depending on your machine.

### Step 5: Verify Both Paths

```bash
# Path A: Explicit builtin
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c custom_attn/demo/mainbuiltin.c -o mainbuiltin.o
riscv64-unknown-elf-objdump -d mainbuiltin.o
# Expected:
#   0:  02b5050b    attn    a0,a0,a1
#   4:  8082        ret

# Path B: Auto-detected loops
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c custom_attn/demo/mainloops.c -o mainloops.o
riscv64-unknown-elf-objdump -d mainloops.o
# Expected: 'attn' instruction appears, NO loop assembly

# GIMPLE dump verification
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c custom_attn/demo/mainloops.c -o mainloops.o \
    -fdump-tree-all-details
grep "ATTENTION PATTERN DETECTED" mainloops.c.*riscv_attn_detect
# Expected: *** ATTENTION PATTERN DETECTED ***
```

---

## Rebuilding After Source Changes

If you modify `riscv-attn-detect.cc` and need to rebuild without
redoing the entire toolchain:

```bash
# Copy updated source
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc
touch gcc/gcc/config/riscv/riscv-attn-detect.cc

# Delete both stage .o files to force recompilation
rm -f build/build-gcc-newlib-stage1/gcc/riscv-attn-detect.o
rm -f build/build-gcc-newlib-stage2/gcc/riscv-attn-detect.o

# Rebuild directly in the GCC build directory
cd build/build-gcc-newlib-stage2/gcc && make riscv-attn-detect.o && make install

# Finish top-level build
cd ../../.. && cd build && rm -f stamps/build-gcc-newlib-stage* && make -j$(nproc)
```

---

## How the GIMPLE Pass Works

The pass (`riscv_attn_detect`) is registered after GCC's loop optimization
phase. It runs on every function and performs two phases:

### Detection Phase

1. Collects all top-level loops from the function's loop tree
2. Reverses loop order (GCC lists children in reverse program order)
3. Slides a window of 4 consecutive loops, checking:

| Stage | Loop Pattern | What It Matches |
|-------|-------------|-----------------|
| 1 | `is_matmul_pattern()` | Triple-nested loop with MULT+PLUS reduction (Q × K^T) |
| 2 | `is_elementwise_div()` | Double-nested loop with MULT by 1/sqrt(d) |
| 3 | `is_softmax_pattern()` | Outer loop with 2–3 child loops (exp+sum, normalize) |
| 4 | `is_matmul_pattern()` | Triple-nested loop (scores × V) |

### Replacement Phase

When all 4 stages match:

1. Extracts function parameters (`n`, `d`, `Q`, `K`, `V`) via `DECL_ARGUMENTS`
2. Builds `dims` struct on stack: `{rows=n, cols=n, seq_len=n, d_model=d}`
3. Builds `qkv` struct on stack: `{Q, K, V}` pointers
4. Splits the preheader → loop1 edge to create a fresh basic block
5. Emits the `attn` instruction via `.insn r 0x0b, 0, 0x01, x0, rs1, rs2`
   (volatile inline assembly — immune to dead code elimination)
6. Redirects control flow from the new block to the function exit,
   bypassing all 4 original loops (which become dead code)

### Key GCC 15.2.0 Compatibility Notes

- Uses `TARGET_MEM_REF` (tree code 166) alongside `MEM_REF` (167) for
  pointer-based memory accesses
- Handles `PLUS_EXPR` operand ordering (MULT result can be in rhs1 or rhs2)
- Reverses `top_loops` vector (GCC lists children in reverse program order)
- Accepts softmax with 2 or 3 child loops (with/without max-reduction)
- Scans middle loop (not innermost) for matmul store targets

---

## Toolchain Modifications Summary

### Files 1–6: Instruction Encoding & Builtin

Automated by `automate_instruction.sh add attn 2`.

| # | File | What |
|---|------|------|
| 1 | `binutils/include/opcode/riscv-opc.h` | `MATCH_ATTN` / `MASK_ATTN` macros |
| 2 | `binutils/opcodes/riscv-opc.c` | Opcode table entry for assembler/disassembler |
| 3 | `gcc/gcc/config/riscv/riscv-ftypes.def` | Function type: `RISCV_ATYPE_UL(UL, UL)` |
| 4 | `gcc/gcc/config/riscv/riscv-builtins.cc` | `__builtin_riscv_attn(unsigned long, unsigned long)` |
| 5 | `gcc/gcc/config/riscv/riscv.md` | `(define_insn "riscv_attn" ...)` pattern |
| 6 | `riscv-opcodes/extensions/rv_custom` | Official opcode database entry |

### GIMPLE Auto-Detection Pass

Applied manually (Step 3 above).

| File | Action | What |
|------|--------|------|
| `gcc/gcc/config/riscv/riscv-attn-detect.cc` | **NEW** | GIMPLE pass (~1300 lines) |
| `gcc/gcc/config/riscv/t-riscv` | **MODIFIED** | `EXTRA_OBJS += riscv-attn-detect.o` |
| `gcc/gcc/Makefile.in` | **MODIFIED** | Build rule for the `.o` |
| `gcc/gcc/config/riscv/riscv.cc` | **MODIFIED** | `#include "context.h"` + `register_pass()` |

---

## Demo Files

| File | Description |
|------|-------------|
| `demo/mainbuiltin.c` | Explicit `__builtin_riscv_attn()` call |
| `demo/mainloops.c` | Plain C loops (auto-detected by GIMPLE pass) |

## Project Structure

```
custom_attn/
├── README.md                    # This file
├── implementation_log.txt       # Detailed technical log with all fixes
├── DOCUMENTATION.md             # Full documentation
├── GENERIC_TEMPLATE.md          # Template for adding custom instructions
├── OPCODE_FIELDS.md             # Opcode field specifications
├── automate_instruction.py      # Automation tool (Python)
├── automate_instruction.sh      # Automation tool (Bash)
├── identify_free_opcodes.py     # Opcode scanner (Python)
├── identify_free_opcodes.sh     # Opcode scanner (Bash)
├── demo/
│   ├── mainbuiltin.c            # Explicit builtin demo
│   └── mainloops.c              # Auto-detected loops demo
└── src/
    └── riscv-attn-detect.cc     # GIMPLE pass source (~1300 lines)
```
