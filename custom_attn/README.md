# Custom RISC-V Instruction — `attn` (Attention Mechanism)
### Group 9 | RISC-V GNU Toolchain

---

## What This Does

We added a custom RISC-V instruction called `attn` that represents the Transformer attention operation:

```
softmax(Q × Kᵀ / √dₖ) × V
```

One single instruction does the whole thing. The C program uses `__builtin_riscv_attn()` — no inline assembly.

```
attn  rd, rs1, rs2
```

| Operand | Type | What It Carries |
|---------|------|-----------------|
| rs1 | integer register | Address of dimensions struct |
| rs2 | integer register | Address of Q/K/V pointers struct |
| rd | integer register | Result / status |

---

## Encoding

| Field | Bits | Value |
|-------|------|-------|
| opcode | [6:0] | `0x0b` (custom-0) |
| rd | [11:7] | destination register |
| funct3 | [14:12] | `0x0` |
| rs1 | [19:15] | source register 1 |
| rs2 | [24:20] | source register 2 |
| funct7 | [31:25] | `0x01` |

```
MATCH_ATTN = 0x0200000b
MASK_ATTN  = 0xfe00707f
```

R-type format. Uses the `custom-0` opcode slot reserved by RISC-V for extensions. Note: `funct7=1` (not 0), because `funct7=0` on custom-0 conflicts with the upstream `MATCH_CUSTOM0` definition.

---

## Quick Start: How to Build Everything from Scratch

### Step 1 — Install Build Dependencies

On Ubuntu/Debian:

```
sudo apt-get install -y autoconf automake autotools-dev curl python3 libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev
```

These are the compilers and libraries needed to build GCC and binutils from source.

### Step 2 — Clone the Repo

```
mkdir -p ~/dc && cd ~/dc
git clone https://github.com/Yash-Awasthi/riscv-gnu-toolchain
cd riscv-gnu-toolchain
```

### Step 3 — Initialize Submodules

```
git submodule update --init binutils
git submodule update --init gcc
```

Binutils and GCC are **Git submodules** that pull from upstream sources (sourceware.org, gcc-mirror). After this step they contain clean, unmodified source code.

> **Important:** The submodules are always cloned fresh from upstream — they do NOT contain our custom instruction modifications yet. You must apply modifications in Step 4.

### Step 4 — Apply Source Modifications (Automated)

The automation script patches all 6 toolchain source files automatically. It finds a free opcode slot, inserts the MATCH/MASK defines, registers the builtin, generates the machine description pattern, and creates a demo C file — all in one command.

**Option A — Python (recommended):**
```
cd custom_attn
python3 automate_instruction.py --name attn --inputs 2 --desc "Transformer attention mechanism" --no-build
```

**Option B — Bash:**
```
cd custom_attn
./automate_instruction.sh add attn 2 "Transformer attention mechanism"
```

Both scripts modify these 6 files:
1. `binutils/include/opcode/riscv-opc.h` — MATCH/MASK defines + DECLARE_INSN
2. `binutils/opcodes/riscv-opc.c` — opcode table entry
3. `gcc/gcc/config/riscv/riscv-ftypes.def` — function type signature
4. `gcc/gcc/config/riscv/riscv-builtins.cc` — builtin registration
5. `gcc/gcc/config/riscv/riscv.md` — machine description pattern
6. `riscv-opcodes/extensions/rv_custom` — riscv-opcodes format entry

> **To undo:** `python3 automate_instruction.py --delete attn` or `./automate_instruction.sh delete attn`

> **To see what's already added:** `python3 automate_instruction.py --list` or `./automate_instruction.sh list`

> **To scan for free opcode slots:** `python3 identify_free_opcodes.py` or `./identify_free_opcodes.sh`

If you prefer to apply modifications manually, see [Manual Modifications](#manual-modifications) below.

### Step 5 — Set Install Prefix

```
export PREFIX=$HOME/riscv_custom
export PATH=$PREFIX/bin:$PATH
```

All compiled tools will go into `~/riscv_custom/bin/`.

### Step 6 — Build Binutils

```
cd ~/dc/riscv-gnu-toolchain
mkdir -p build_binutils && cd build_binutils
../binutils/configure --target=riscv64-unknown-elf --prefix=$PREFIX --disable-werror
make -j$(nproc)
make install
```

This builds the assembler (`riscv64-unknown-elf-as`) and disassembler (`riscv64-unknown-elf-objdump`) with our custom `attn` instruction support.

### Step 7 — Build GCC (Stage 1)

```
cd ~/dc/riscv-gnu-toolchain
mkdir -p build_gcc && cd build_gcc
../gcc/configure --target=riscv64-unknown-elf --prefix=$PREFIX --disable-shared --disable-threads --disable-multilib --disable-libatomic --disable-libmudflap --disable-libssp --disable-libquadmath --disable-libgomp --disable-nls --disable-bootstrap --enable-languages=c --with-arch=rv64imac --with-abi=lp64 --with-newlib
make all-gcc -j$(nproc)
make install-gcc
```

This builds the cross-compiler (`riscv64-unknown-elf-gcc`) that knows about `__builtin_riscv_attn()`.

### Step 8 — Compile the Demo and Verify

```
cd ~/dc/riscv-gnu-toolchain/custom_attn/demo
riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 -ffreestanding -nostdinc -c main.c -o main.o
riscv64-unknown-elf-objdump -d main.o
```

You should see:

```
0000000000000000 <run_attention>:
   0:   02b5050b    attn    a0,a0,a1
   4:   8082        ret
```

The custom instruction `attn` appears by name in the disassembly. That confirms the full pipeline works: C code → GCC builtin → assembler → binary → disassembler recognizes it.

---

## The Demo C Program

```c
__attribute__((noinline))
unsigned long run_attention(unsigned long dims_addr,
                            unsigned long qkv_addr)
{
    return __builtin_riscv_attn(dims_addr, qkv_addr);
}

int main(void)
{
    float Q[4] = {0.10f, 0.20f, 0.30f, 0.40f};
    float K[4] = {0.50f, 0.60f, 0.70f, 0.80f};
    float V[4] = {0.90f, 1.00f, 1.10f, 1.20f};

    typedef struct { int rows; int cols; int seq_len; int d_model; } attn_dims_t;
    typedef struct { float *Q; float *K; float *V; } attn_qkv_t;

    attn_dims_t dims = {2, 2, 4, 8};
    attn_qkv_t  qkv  = {Q, K, V};

    volatile unsigned long result = run_attention(
        (unsigned long)&dims,
        (unsigned long)&qkv
    );

    (void)result;
    return 0;
}
```

No inline assembly anywhere. The compiler emits the `attn` instruction directly from the builtin call.

---

## Automation Scripts

### Adding a new instruction

**Python:**
```
python3 automate_instruction.py --name my_pow --inputs 2 --desc "(a^b)^a"
```

**Bash:**
```
./automate_instruction.sh add my_pow 2 "(a^b)^a"
```

This will:
1. Find the next free opcode slot
2. Modify all 6 toolchain source files
3. Generate `demo/main_my_pow.c` with `wrapper_my_pow()` calling `__builtin_riscv_my_pow()`
4. Rebuild binutils and GCC (unless `--no-build` is passed to the Python version)
5. Compile and run objdump to verify

Supports 0-input (no args), 1-input, 2-input (R-type), and 3-input (R4-type) instructions.

### Scan for free opcode slots

Standalone scripts to inspect the opcode space without modifying anything:

```
python3 identify_free_opcodes.py                    # Summary view
python3 identify_free_opcodes.py --detailed         # Show first 30 free slots
python3 identify_free_opcodes.py --check 0x0200000b # Check specific MATCH
./identify_free_opcodes.sh                          # Summary view (bash)
./identify_free_opcodes.sh --detailed               # Show first 20 free slots
./identify_free_opcodes.sh --check 0x0200000b       # Check specific MATCH
```

Or via the automation scripts:

```
python3 automate_instruction.py --scan 2   # Free 2-input slots
./automate_instruction.sh scan 2           # Same, bash version
```

### Batch add

```
python3 automate_instruction.py --batch instructions_example.txt
./automate_instruction.sh batch instructions_example.txt
```

### Delete

```
python3 automate_instruction.py --delete my_pow
./automate_instruction.sh delete my_pow
```

### List existing custom instructions

```
python3 automate_instruction.py --list
./automate_instruction.sh list
```

---

## File 7 — GIMPLE Idiom Detection Pass (Automatic Pattern Recognition)

Beyond the builtin-based approach (files 1-6), we implemented a **GIMPLE optimization pass** that automatically detects the attention mechanism pattern in plain C code and replaces it with the `attn` instruction. No builtins, no inline assembly, no annotations required.

### What is a GIMPLE pass?

GCC compiles C code through multiple intermediate representations. GIMPLE is GCC's high-level IR where loops, assignments, and function calls are still visible (unlike low-level RTL where everything is register transfers). A GIMPLE pass is a plugin that runs during compilation, inspects the IR, and can transform it.

Our pass (`riscv_attn_detect`) runs after GCC's loop distribution pass. At that point:
- Loops are in canonical form (single entry, single exit)
- Scalar evolution (SCEV) is available for analyzing loop trip counts
- Array accesses are still recognizable as `ARRAY_REF` trees

The pass scans for the 4-stage attention pattern, and when found, replaces all 4 loop nests with a single call to `__builtin_riscv_attn()` — which the existing files 1-6 pipeline lowers to the `attn` instruction.

### How the user writes code (no builtins needed)

```c
#include <math.h>

void attention(int seq_len, int d_model,
               float Q[][d_model], float K[][d_model],
               float V[][d_model], float output[][d_model])
{
    float score[seq_len][seq_len];

    // Stage 1: Q * K^T → score
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < seq_len; j++) {
            score[i][j] = 0;
            for (int k = 0; k < d_model; k++)
                score[i][j] += Q[i][k] * K[j][k];  // K[j][k] not K[k][j] = transposed
        }

    // Stage 2: scale by 1/sqrt(d_model)
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < seq_len; j++)
            score[i][j] /= sqrtf((float)d_model);

    // Stage 3: softmax per row
    for (int i = 0; i < seq_len; i++) {
        float max_val = score[i][0];
        for (int j = 1; j < seq_len; j++)
            if (score[i][j] > max_val) max_val = score[i][j];

        float sum = 0;
        for (int j = 0; j < seq_len; j++)
            sum += expf(score[i][j] - max_val);

        for (int j = 0; j < seq_len; j++)
            score[i][j] = expf(score[i][j] - max_val) / sum;
    }

    // Stage 4: score * V → output
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < d_model; j++) {
            output[i][j] = 0;
            for (int k = 0; k < seq_len; k++)
                output[i][j] += score[i][k] * V[k][j];
        }
}
```

This is **plain C** — standard loops, standard math. The compiler's GIMPLE pass recognizes the 4-stage pattern and silently replaces it with the `attn` instruction. The programmer does not need to know the instruction exists.

### Detection algorithm (4 stages)

The pass collects all top-level loops in the function and looks for 4 consecutive ones matching:

1. **Stage 1 (matmul Q*K^T)**: Triple-nested loop with reduction `score[i][j] += Q[i][k] * K[j][k]`. K transposition is detected because both Q and K use the innermost induction variable `k` as the fast-varying index (K is `K[j][k]` not `K[k][j]`).

2. **Stage 2 (scale)**: Double-nested loop doing `score[i][j] /= sqrt(d_model)`. Pass checks for `RDIV_EXPR` on the same array base as stage 1's output.

3. **Stage 3 (softmax per row)**: Outer loop (over rows) containing exactly 3 sequential inner loops:
   - **Max-reduction**: `GIMPLE_COND` with `GT_EXPR`, PHI node for running max
   - **Sum-of-exp**: `PLUS_EXPR` reduction with `__builtin_expf` call
   - **Normalize**: `RDIV_EXPR` with `__builtin_expf` and the sum from previous loop

4. **Stage 4 (matmul score*V)**: Triple-nested loop `output[i][j] += score[i][k] * V[k][j]`. First operand base must match the `score` array from stages 1-3.

If all 4 stages match and their array bases chain correctly, the pass:
1. Builds `attn_dims_t` and `attn_qkv_t` structs on the stack
2. Stores dimensions and Q/K/V pointers into the structs
3. Inserts a `__builtin_riscv_attn(dims_addr, qkv_addr)` call
4. Deletes all 4 loop nests

### Files involved

| Source file (in `custom_attn/src/`) | Target (in GCC submodule) | What it does |
|------|---------------------------|---------|
| `riscv-attn-detect.cc` | Copy to `gcc/gcc/config/riscv/` | The GIMPLE pass itself (~700 lines) |
| `t-riscv.addition` | Modify `gcc/gcc/config/riscv/t-riscv` | Build rule for the new .cc file |
| `riscv.cc.addition` | Modify `gcc/gcc/config/riscv/riscv.cc` | Register pass in optimizer pipeline |
| `tree-pass.h.addition` | Modify `gcc/gcc/tree-pass.h` | Declare factory function |

### Step-by-step: How to apply File 7

> **Prerequisite:** Files 1-6 must already be applied (the GIMPLE pass generates a `__builtin_riscv_attn()` call, which needs files 1-6 to work). If you followed Steps 1-4 in the Quick Start, files 1-6 are already done.

**Step 7a — Copy the pass source file into GCC:**

```bash
cd ~/dc/riscv-gnu-toolchain
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc
```

This is the main file — ~700 lines of C++ that implements the pattern detection and replacement.

**Step 7b — Add the build rule to `t-riscv`:**

Open `gcc/gcc/config/riscv/t-riscv` and find the existing build rules (search for `riscv-builtins.o`). Add this block alongside them:

```makefile
riscv-attn-detect.o : $(srcdir)/config/riscv/riscv-attn-detect.cc \
  $(CONFIG_H) $(SYSTEM_H) coretypes.h $(TM_H) $(TREE_H) \
  $(GIMPLE_H) tree-pass.h cfgloop.h
	$(COMPILER) -c $(ALL_COMPILERFLAGS) $(ALL_CPPFLAGS) $(INCLUDES) \
	  $(srcdir)/config/riscv/riscv-attn-detect.cc
```

Then find the line that starts with `EXTRA_OBJS` and add `riscv-attn-detect.o` to it:

```
EXTRA_OBJS += riscv-attn-detect.o
```

> **Important:** The indentation in the `$(COMPILER)` line must be a **TAB character**, not spaces. Makefiles require tabs for recipe lines.

**Step 7c — Declare the factory function in `tree-pass.h`:**

Open `gcc/gcc/tree-pass.h` and search for `make_pass_loop_distribution`. Near that area (with the other `make_pass_*` declarations), add:

```cpp
extern gimple_opt_pass *make_pass_riscv_attn_detect (gcc::context *);
```

**Step 7d — Register the pass in `riscv.cc`:**

Open `gcc/gcc/config/riscv/riscv.cc`.

First, make sure these headers are included near the top (they may already be there):
```cpp
#include "tree-pass.h"
#include "context.h"
```

Then find the function `riscv_option_override()` and add this block at the end of it (before the closing `}`):

```cpp
  /* Register the attention idiom detection pass.
     Runs after loop distribution, where loops are in canonical form
     and SCEV is available for trip count analysis.  */
  {
    struct register_pass_info attn_pass_info;
    attn_pass_info.pass = make_pass_riscv_attn_detect (g);
    attn_pass_info.reference_pass_name = "ldist";
    attn_pass_info.ref_pass_instance_number = 1;
    attn_pass_info.pos_op = PASS_POS_INSERT_AFTER;
    register_pass (&attn_pass_info);
  }
```

This tells GCC: "insert our pass right after the loop distribution pass (`ldist`) in the optimization pipeline."

**Step 7e — Rebuild GCC:**

```bash
cd ~/dc/riscv-gnu-toolchain/build_gcc
make all-gcc -j$(nproc)
make install-gcc
```

This is an incremental rebuild — only the new/changed files get compiled.

### Step-by-step: Verify File 7 works

**Verify 1 — Check the pass is registered:**

```bash
riscv64-unknown-elf-gcc -O2 -fdump-passes demo/main_idiom.c 2>&1 | grep attn
```

Should show `riscv_attn_detect` in the pass list.

**Verify 2 — Compile the plain C demo with tree dump:**

```bash
cd ~/dc/riscv-gnu-toolchain/custom_attn
riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \
    -fdump-tree-riscv_attn_detect-details \
    -ffreestanding -nostdinc \
    -c demo/main_idiom.c -o demo/main_idiom.o
```

**Verify 3 — Check the dump shows detection:**

```bash
cat demo/main_idiom.c.*t.riscv_attn_detect
```

Should show "Attention pattern detected" in the dump output.

**Verify 4 — Check objdump shows `attn` instruction:**

```bash
riscv64-unknown-elf-objdump -d demo/main_idiom.o | grep attn
```

Should show the `attn` instruction — same as `demo/main.o` (explicit builtin), proving the GIMPLE pass found the pattern and replaced it automatically.

**Verify 5 — Negative test (should NOT trigger):**

Write a file with just a matmul (no softmax):
```c
void just_matmul(int n, int m, float A[][m], float B[][m], float C[][m]) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++) {
            C[i][j] = 0;
            for (int k = 0; k < m; k++)
                C[i][j] += A[i][k] * B[k][j];
        }
}
```

Compile it — the pass should NOT trigger (only matmul, no scale/softmax/second matmul). No `attn` in objdump.

### Quick reference: copy-paste all commands

```bash
# === Apply File 7 modifications ===
cd ~/dc/riscv-gnu-toolchain

# 7a: Copy the pass source
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc

# 7b: Add build rule to t-riscv (do this manually — see instructions above)
#     Edit: gcc/gcc/config/riscv/t-riscv

# 7c: Add declaration to tree-pass.h (do this manually — see instructions above)
#     Edit: gcc/gcc/tree-pass.h

# 7d: Register pass in riscv.cc (do this manually — see instructions above)
#     Edit: gcc/gcc/config/riscv/riscv.cc

# 7e: Rebuild GCC
cd build_gcc
make all-gcc -j$(nproc)
make install-gcc

# === Verify ===
cd ~/dc/riscv-gnu-toolchain/custom_attn

# Check pass is registered
riscv64-unknown-elf-gcc -O2 -fdump-passes demo/main_idiom.c 2>&1 | grep attn

# Compile plain C demo
riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \
    -fdump-tree-riscv_attn_detect-details \
    -ffreestanding -nostdinc \
    -c demo/main_idiom.c -o demo/main_idiom.o

# Check detection
cat demo/main_idiom.c.*t.riscv_attn_detect

# Check objdump
riscv64-unknown-elf-objdump -d demo/main_idiom.o | grep attn
```

### Demo file

`demo/main_idiom.c` — Contains a plain C `attention()` function with the full 4-stage loop pattern. Compiles to `attn` automatically with the GIMPLE pass. Compare with `demo/main.c` (explicit builtin) — both produce the same instruction.

### How File 7 differs from Files 1-6

| | Files 1-6 (Builtin) | File 7 (GIMPLE Pass) |
|---|---|---|
| **C code** | Uses `__builtin_riscv_attn()` | Plain loops, standard C |
| **Programmer** | Must know the builtin exists | Writes normal attention code |
| **Compiler** | Directly lowers builtin to `attn` | Detects pattern, inserts builtin, then lowers |
| **When it triggers** | Always (explicit call) | Only when 4-stage pattern matches exactly |
| **Demo file** | `demo/main.c` | `demo/main_idiom.c` |
| **Requires** | Files 1-6 applied | Files 1-6 applied + File 7 applied |

---

## Manual Modifications

If you prefer to apply modifications by hand instead of using the automation script, here are all 6 files:

#### File 1 — `binutils/include/opcode/riscv-opc.h`

This file stores the binary encoding of every RISC-V instruction.

Open the file, find the line `/* Instruction opcode macros.  */` (near line 23), and add this right after it:

```
#define MATCH_ATTN  0x0200000b
#define MASK_ATTN   0xfe00707f
```

Then scroll to the bottom of the file, find the `DECLARE_INSN` block (starts with `#ifdef DECLARE_INSN`), and add this line at the end of that block:

```
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)
```

---

#### File 2 — `binutils/opcodes/riscv-opc.c`

This file is the opcode table — it maps mnemonics to encodings.

Find the array `const struct riscv_opcode riscv_opcodes[] =` and add this as the **first entry** inside the array (right after the opening `{`):

```
{"attn", 0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0},
```

---

#### File 3 — `gcc/gcc/config/riscv/riscv-ftypes.def`

This file defines C function type signatures that GCC builtins can use.

Add this at the end of the file:

```
DEF_RISCV_FTYPE (2, (DI, DI, DI))
```

This creates the type `RISCV_DI_FTYPE_DI_DI` meaning: returns DI (64-bit int), takes two DI arguments.

---

#### File 4 — `gcc/gcc/config/riscv/riscv-builtins.cc`

This file registers compiler builtins. Three additions needed.

**Addition 1** — Find `#define RISCV_ATYPE_SI intSI_type_node` and add after it:

```
#define RISCV_ATYPE_DI long_integer_type_node
```

**Addition 2** — Find `AVAIL (hint_pause, (!0))` and add after it:

```
AVAIL (always_enabled, (!0))
```

**Addition 3** — Find `#include "corev.def"` and add after it:

```
DIRECT_BUILTIN (attn, RISCV_DI_FTYPE_DI_DI, always_enabled),
```

---

#### File 5 — `gcc/gcc/config/riscv/riscv.md`

This is the GCC machine description — it maps builtins to assembly output.

**Addition 1** — Find `(define_c_enum "unspec" [` and add inside the list:

```
UNSPEC_ATTN
```

**Addition 2** — Add at the very end of the file:

```
(define_insn "riscv_attn"
  [(set (match_operand:DI 0 "register_operand" "=r")
        (unspec:DI [(match_operand:DI 1 "register_operand" "r")
                    (match_operand:DI 2 "register_operand" "r")]
                   UNSPEC_ATTN))]
  ""
  "attn\t%0,%1,%2"
  [(set_attr "type" "arith")
   (set_attr "mode" "DI")])
```

---

#### File 6 — `riscv-opcodes/extensions/rv_custom`

This file registers your custom instructions in the official riscv-opcodes format, so the opcode slot is tracked and the automation script knows which encodings are in use.

Add a line at the end of the file:

```
attn rd rs1 rs2 31..25=1 14..12=0 6..2=0x02 1..0=3
```

This follows the same format as `rv_i`, `rv64_m`, etc. — instruction name, operand fields, then bit-field constraints. `6..2=0x02` means custom-0 base opcode. `31..25=1` means funct7=1.

---

## Appendix: Understanding Opcode Identification

This section explains how opcode slots work in RISC-V. The automation script handles this automatically — this is for educational reference.

### How the custom opcode slots work

RISC-V reserves four custom slots: custom-0 (`0x0B`), custom-1 (`0x2B`), custom-2 (`0x5B`), custom-3 (`0x7B`). Within each slot, you pick a `funct3` (3 bits, 0-7) and `funct7` (7 bits, 0-127) to uniquely identify your instruction. That gives 8 × 128 = 1024 possible R-type encodings per slot, 4096 total.

### Finding free slots manually

**Step 1** — Clone the riscv-opcodes repository (official opcode definitions):

```
cd ~
git clone https://github.com/riscv/riscv-opcodes
cd riscv-opcodes
```

**Step 2** — Extract all opcodes already used by extensions:

```
grep -o "6..2=0x[0-9A-Fa-f]*" -R extensions/ | sed 's/.*=0x//' | awk '{printf "0x%02x\n", strtonum("0x"$1)}' | sort -u > used_clean.txt
```

**Step 3** — Generate all possible 5-bit opcode values (0x00 to 0x1f):

```
for i in $(seq 0 31); do printf "0x%02x\n" $i; done > all_clean.txt
```

**Step 4** — Find opcodes that are free:

```
comm -23 all_clean.txt used_clean.txt > free_from_extensions.txt
cat free_from_extensions.txt
```

**Step 5** — Cross-check against binutils:

```
grep -oP '"[A-Z_]+",\s*0x\K[0-9a-fA-F]+' ~/dc/riscv-gnu-toolchain/binutils/gas/config/tc-riscv.c | awk '{printf "0x%02x\n", strtonum("0x"$1)}' | sort -u > used_binutils.txt
comm -23 free_from_extensions.txt used_binutils.txt
```

**Result for us:** Opcode slot `0x02` (which maps to `custom-0`, full 7-bit opcode `0x0B`) is confirmed free.

Or just use the standalone scripts: `python3 identify_free_opcodes.py` / `./identify_free_opcodes.sh`

### Understanding what `0x02` actually is

The grep output gives us `0x02`. This is the value of bits [6:2] of the opcode field — only 5 bits. But the full opcode field in a RISC-V instruction is 7 bits [6:0]. Here is how they relate:

A RISC-V 32-bit instruction always has `bits[1:0] = 11` (binary) = `0x3`. This is how the CPU knows it is a 32-bit instruction (16-bit compressed instructions have different patterns in these bits). So the opcode field is:

```
bits [6:0] = { bits[6:2] , bits[1:0] }
            = { 5-bit value,    11    }
```

Our free slot value is `0x02` (5 bits). Converting to the full 7-bit opcode:

```
Step 1: Write 0x02 in binary (5 bits)
        0x02 = 00010

Step 2: Append the fixed bits [1:0] = 11
        00010 | 11 = 0001011

Step 3: Convert back to hex (7 bits)
        0001011 = 0x0B

Formula: full_opcode = (0x02 << 2) | 0x03
                     = (0x02 × 4)  + 0x03
                     = 0x08 + 0x03
                     = 0x0B
```

So `0x02` from the grep → full opcode `0x0B` → this is the `custom-0` slot.

For reference, all four custom slots follow the same pattern:

| Grep value (bits[6:2]) | Shift left by 2 | OR with 0x3 | Full opcode [6:0] | Name |
|------------------------|-----------------|-------------|-------------------|------|
| `0x02` | `0x08` | `0x08 \| 0x03` | `0x0B` | custom-0 |
| `0x0A` | `0x28` | `0x28 \| 0x03` | `0x2B` | custom-1 |
| `0x16` | `0x58` | `0x58 \| 0x03` | `0x5B` | custom-2 |
| `0x1E` | `0x78` | `0x78 \| 0x03` | `0x7B` | custom-3 |

### Computing MATCH (The Instruction's Fingerprint)

MATCH is a 32-bit number where every fixed field of the instruction is filled in and every variable field (registers) is zero.

For an R-type instruction, the 32-bit layout is:

```
31       25 24    20 19    15 14  12 11     7 6      0
┌──────────┬────────┬────────┬──────┬────────┬────────┐
│  funct7  │  rs2   │  rs1   │funct3│   rd   │ opcode │
│  7 bits  │ 5 bits │ 5 bits │3 bits│ 5 bits │ 7 bits │
└──────────┴────────┴────────┴──────┴────────┴────────┘
```

We chose: opcode = `0x0B`, funct3 = `0`, funct7 = `1`. The register fields (rd, rs1, rs2) are variable — they change depending on which registers the programmer uses — so they are 0 in MATCH.

```
MATCH = opcode | (funct3 << 12) | (funct7 << 25)

Step 1: opcode = 0x0B
        In binary (32 bits): 0000_0000_0000_0000_0000_0000_0000_1011

Step 2: funct3 << 12 = 0x0 << 12 = 0x00000000
        (funct3 is 0, so shifting it changes nothing)

Step 3: funct7 << 25 = 0x01 << 25 = 0x02000000
        In binary: 0000_0010_0000_0000_0000_0000_0000_0000

Step 4: OR them together:
        0x0000000B | 0x00000000 | 0x02000000 = 0x0200000B

MATCH_ATTN = 0x0200000B
```

Why funct7 = 1 (not 0)?  The upstream riscv-opc.h already defines `MATCH_CUSTOM0 = 0x0000000B` with funct7=0. Using funct7=0 would conflict. funct7=1 gives us a unique encoding within the same custom-0 slot.

### Computing MASK (Which Bits Identify the Instruction)

MASK tells the CPU: "look at THESE bits to identify which instruction this is." Every bit that is `1` in the MASK is a bit that matters for identification. Bits that are `0` in the MASK are "don't care" (they hold register numbers, which vary).

For R-type, the identifying fields are: opcode [6:0], funct3 [14:12], funct7 [31:25].

```
Build the MASK bit by bit:

Bits [6:0]   = opcode  → must check → set to 1111111
Bits [11:7]  = rd      → varies     → set to 00000
Bits [14:12] = funct3  → must check → set to 111
Bits [19:15] = rs1     → varies     → set to 00000
Bits [24:20] = rs2     → varies     → set to 00000
Bits [31:25] = funct7  → must check → set to 1111111

Full 32-bit MASK:
  1111111_00000_00000_111_00000_1111111

Grouped into nibbles (4-bit groups) from the right:
  1111_1110_0000_0000_0111_0000_0111_1111

Convert each nibble to hex:
  F    E    0    0    7    0    7    F

MASK_ATTN = 0xFE00707F
```

Verification — count the 1-bits:
- opcode: 7 bits
- funct3: 3 bits
- funct7: 7 bits
- Total: 17 bits are 1 in the MASK, the other 15 bits (rd + rs1 + rs2 = 5+5+5) are 0. Correct.

### Verification: Does (instruction & MASK) == MATCH?

Our compiled instruction from objdump is `0x02B5050B` (which is `attn a0, a0, a1`). Let us verify:

```
instruction = 0x02B5050B
MASK        = 0xFE00707F
MATCH       = 0x0200000B

instruction & MASK:
  0000_0010_1011_0101_0000_0101_0000_1011   (0x02B5050B)
& 1111_1110_0000_0000_0111_0000_0111_1111   (0xFE00707F)
= 0000_0010_0000_0000_0000_0000_0000_1011   (0x0200000B)

0x0200000B == 0x0200000B  ✓  This IS the attn instruction!
```

Now extract the registers:

```
Full binary: 0000_0010_1011_0101_0000_0101_0000_1011

funct7 = bits[31:25] = 0000001 = 1        ← identifies instruction
rs2    = bits[24:20] = 01011   = 11 = a1  ← second argument
rs1    = bits[19:15] = 01010   = 10 = a0  ← first argument
funct3 = bits[14:12] = 000     = 0        ← identifies instruction
rd     = bits[11:7]  = 01010   = 10 = a0  ← destination
opcode = bits[6:0]   = 0001011 = 0x0B     ← identifies instruction

Result: attn a0, a0, a1  ✓
```

---

## File 7 — GIMPLE Idiom Detection Pass (Automatic Pattern Recognition)

Beyond the builtin-based approach (files 1-6), we implemented a **GIMPLE optimization pass** that automatically detects the attention mechanism pattern in plain C code and replaces it with the `attn` instruction. No builtins, no inline assembly, no annotations required.

### What is a GIMPLE pass?

GCC compiles C code through multiple intermediate representations. GIMPLE is GCC's high-level IR where loops, assignments, and function calls are still visible (unlike low-level RTL where everything is register transfers). A GIMPLE pass is a plugin that runs during compilation, inspects the IR, and can transform it.

Our pass (`riscv_attn_detect`) runs after GCC's loop pass. At that point:
- Loops are in canonical form (single entry, single exit)
- Scalar evolution (SCEV) is available for analyzing loop trip counts
- Array accesses are still recognizable as `ARRAY_REF` trees

The pass scans for the 4-stage attention pattern, and when found, replaces all 4 loop nests with a single call to `__builtin_riscv_attn()` — which the existing files 1-6 pipeline lowers to the `attn` instruction.

### How the user writes code (no builtins needed)

```c
#include <math.h>

void attention(int seq_len, int d_model,
               float Q[][d_model], float K[][d_model],
               float V[][d_model], float output[][d_model])
{
    float score[seq_len][seq_len];

    // Stage 1: Q * K^T → score
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < seq_len; j++) {
            score[i][j] = 0;
            for (int k = 0; k < d_model; k++)
                score[i][j] += Q[i][k] * K[j][k];  // K[j][k] not K[k][j] = transposed
        }

    // Stage 2: scale by 1/sqrt(d_model)
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < seq_len; j++)
            score[i][j] /= sqrtf((float)d_model);

    // Stage 3: softmax per row
    for (int i = 0; i < seq_len; i++) {
        float max_val = score[i][0];
        for (int j = 1; j < seq_len; j++)
            if (score[i][j] > max_val) max_val = score[i][j];

        float sum = 0;
        for (int j = 0; j < seq_len; j++)
            sum += expf(score[i][j] - max_val);

        for (int j = 0; j < seq_len; j++)
            score[i][j] = expf(score[i][j] - max_val) / sum;
    }

    // Stage 4: score * V → output
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < d_model; j++) {
            output[i][j] = 0;
            for (int k = 0; k < seq_len; k++)
                output[i][j] += score[i][k] * V[k][j];
        }
}
```

This is **plain C** — standard loops, standard math. The compiler's GIMPLE pass recognizes the 4-stage pattern and silently replaces it with the `attn` instruction. The programmer does not need to know the instruction exists.

### Detection algorithm (4 stages)

The pass collects all top-level loops in the function and looks for 4 consecutive ones matching:

1. **Stage 1 (matmul Q*K^T)**: Triple-nested loop with reduction `score[i][j] += Q[i][k] * K[j][k]`. K transposition is detected because both Q and K use the innermost induction variable `k` as the fast-varying index.

2. **Stage 2 (scale)**: Double-nested loop doing `score[i][j] /= sqrt(d_model)`. Pass checks for `RDIV_EXPR` on the same array base as stage 1's output.

3. **Stage 3 (softmax per row)**: Outer loop (over rows) containing exactly 3 sequential inner loops:
   - **Max-reduction**: `GIMPLE_COND` with `GT_EXPR`, PHI node for running max
   - **Sum-of-exp**: `PLUS_EXPR` reduction with `__builtin_expf` call
   - **Normalize**: `RDIV_EXPR` with `__builtin_expf` and the sum from previous loop

4. **Stage 4 (matmul score*V)**: Triple-nested loop `output[i][j] += score[i][k] * V[k][j]`. First operand base must match the `score` array from stages 1-3.

If all 4 stages match and their array bases chain correctly, the pass:
1. Builds `attn_dims_t` and `attn_qkv_t` structs on the stack
2. Stores dimensions and Q/K/V pointers into the structs
3. Inserts a `__builtin_riscv_attn(dims_addr, qkv_addr)` call
4. Redirects control flow to bypass all 4 loop nests (dead code eliminated by later DCE passes)

### Files involved

| Action | File | What it does |
|--------|------|-------------|
| **New file** | `gcc/gcc/config/riscv/riscv-attn-detect.cc` | The GIMPLE pass itself (~1169 lines) |
| **Modified** | `gcc/gcc/config/riscv/t-riscv` | Added `EXTRA_OBJS += riscv-attn-detect.o` so GCC links it |
| **Modified** | `gcc/gcc/Makefile.in` | Added compilation rule for `.cc` → `.o` |
| **Modified** | `gcc/gcc/config/riscv/riscv.cc` | Added `#include "context.h"`, extern declaration, and `register_pass()` inside `riscv_option_override()` |

### Step-by-step: How to apply File 7

> **Prerequisite:** Files 1-6 must already be applied (the GIMPLE pass generates a `__builtin_riscv_attn()` call, which needs files 1-6 to work).

**Step 7a — Copy the pass source file into GCC:**

```bash
cd ~/dc/riscv-gnu-toolchain
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc
```

**Step 7b — Add the object to the build system:**

Append to `gcc/gcc/config/riscv/t-riscv`:

```makefile
EXTRA_OBJS += riscv-attn-detect.o
```

Append the compilation rule to `gcc/gcc/Makefile.in`:

```makefile
riscv-attn-detect.o : $(srcdir)/config/riscv/riscv-attn-detect.cc \
  $(CONFIG_H) $(SYSTEM_H) coretypes.h $(TM_H) $(TREE_H) \
  $(GIMPLE_H) tree-pass.h cfgloop.h
	$(COMPILER) -c $(ALL_COMPILERFLAGS) $(ALL_CPPFLAGS) $(INCLUDES) \
	  $(srcdir)/config/riscv/riscv-attn-detect.cc
```

> **Important:** The `$(COMPILER)` line must be indented with a **TAB character**, not spaces.

**Step 7c — Register the pass in `riscv.cc`:**

Open `gcc/gcc/config/riscv/riscv.cc` and make three additions:

**Addition 1** — Add header near the top (after other `#include` lines):
```cpp
#include "context.h"
```

**Addition 2** — Add extern declaration (before the `riscv_option_override` function):
```cpp
extern gimple_opt_pass *make_pass_riscv_attn_detect (gcc::context *);
```

**Addition 3** — Add pass registration at the end of `riscv_option_override()`, before its closing `}`:
```cpp
  /* Register attention detection pass */
  struct register_pass_info attn_info;
  attn_info.pass = make_pass_riscv_attn_detect (g);
  attn_info.reference_pass_name = "loop";
  attn_info.ref_pass_instance_number = 1;
  attn_info.pos_op = PASS_POS_INSERT_AFTER;
  register_pass (&attn_info);
```

The variable `g` is the global `gcc::context *` declared in `context.h`.

**Step 7d — Fix header ordering in `riscv-attn-detect.cc`:**

The source file requires two header fixes for GCC 15.x compatibility:

1. **Move `fold-const.h` before `tree-scalar-evolution.h`**: The header `tree-scalar-evolution.h` transitively includes `tree-data-ref.h`, which uses macros from `fold-const.h`. If `fold-const.h` comes after, compilation fails.

2. **Add `cfghooks.h`** after `cfganal.h`: Needed for `redirect_edge_and_branch()`.

Optionally remove `#include "tree-data-ref.h"` — the pass does not use it directly.

**Step 7e — Rebuild the toolchain:**

```bash
cd ~/dc/riscv-gnu-toolchain
mkdir -p build && cd build
../configure --prefix=$HOME/riscv --with-arch=rv64gc --with-abi=lp64d
make -j$(nproc)
```

### Copy-paste: All commands in one block

```bash
cd ~/dc/riscv-gnu-toolchain

# 7a: Copy pass source
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc

# 7b: Add to build system
echo '' >> gcc/gcc/config/riscv/t-riscv
echo 'EXTRA_OBJS += riscv-attn-detect.o' >> gcc/gcc/config/riscv/t-riscv

cat >> gcc/gcc/Makefile.in << 'RULE'

riscv-attn-detect.o : $(srcdir)/config/riscv/riscv-attn-detect.cc \
  $(CONFIG_H) $(SYSTEM_H) coretypes.h $(TM_H) $(TREE_H) \
  $(GIMPLE_H) tree-pass.h cfgloop.h
	$(COMPILER) -c $(ALL_COMPILERFLAGS) $(ALL_CPPFLAGS) $(INCLUDES) \
	  $(srcdir)/config/riscv/riscv-attn-detect.cc
RULE

# 7c: Register pass in riscv.cc
sed -i '/#include "tm.h"/a #include "context.h"' gcc/gcc/config/riscv/riscv.cc

sed -i '/^riscv_option_override/,/^}/ {
  /^}/ i\
extern gimple_opt_pass *make_pass_riscv_attn_detect (gcc::context *);\
\  /* Register attention detection pass */\
\  struct register_pass_info attn_info;\
\  attn_info.pass = make_pass_riscv_attn_detect (g);\
\  attn_info.reference_pass_name = "loop";\
\  attn_info.ref_pass_instance_number = 1;\
\  attn_info.pos_op = PASS_POS_INSERT_AFTER;\
\  register_pass (\&attn_info);
}' gcc/gcc/config/riscv/riscv.cc

# 7d: Fix header ordering in the pass source
sed -i '/#include "fold-const.h"/d' gcc/gcc/config/riscv/riscv-attn-detect.cc
sed -i '/#include "tree-scalar-evolution.h"/i #include "fold-const.h"' gcc/gcc/config/riscv/riscv-attn-detect.cc
sed -i '/#include "cfganal.h"/a #include "cfghooks.h"' gcc/gcc/config/riscv/riscv-attn-detect.cc
sed -i '/#include "tree-data-ref.h"/d' gcc/gcc/config/riscv/riscv-attn-detect.cc

# 7e: Build
mkdir -p build && cd build
../configure --prefix=$HOME/riscv --with-arch=rv64gc --with-abi=lp64d
make -j$(nproc)
```

### Build fixes applied (GCC 15.x compatibility)

During the build, three compilation issues were encountered and resolved:

| Error | Cause | Fix |
|-------|-------|-----|
| `redirect_edge_and_branch not declared` | Missing header | Added `#include "cfghooks.h"` |
| `operand_equal_p / fold_unary not declared` | `tree-data-ref.h` needs `fold-const.h` first | Moved `#include "fold-const.h"` before `tree-scalar-evolution.h` |
| `'g' was not declared` / `incomplete type gcc::context` | `riscv.cc` missing context header | Added `#include "context.h"` to `riscv.cc` |

### How File 7 differs from Files 1-6

| | Files 1-6 (Builtin) | File 7 (GIMPLE Pass) |
|---|---|---|
| **C code** | Uses `__builtin_riscv_attn()` | Plain loops, standard C |
| **Programmer** | Must know the builtin exists | Writes normal attention code |
| **Compiler** | Directly lowers builtin to `attn` | Detects pattern, inserts builtin, then lowers |
| **When it triggers** | Always (explicit call) | Only when 4-stage pattern matches exactly |
| **Demo file** | `demo/main.c` | `demo/main_idiom.c` |
| **Requires** | Files 1-6 applied | Files 1-6 applied + File 7 applied |

---

## Repository Structure

```
custom_attn/
├── README.md                    ← This file
├── DOCUMENTATION.md             ← Detailed teaching document (viva prep)
├── GENERIC_TEMPLATE.md          ← Template for adding any new instruction
├── OPCODE_FIELDS.md             ← Opcode/operand field reference
├── automate_instruction.py      ← Python automation script
├── automate_instruction.sh      ← Bash automation script
├── identify_free_opcodes.py     ← Standalone opcode slot scanner (Python)
├── identify_free_opcodes.sh     ← Standalone opcode slot scanner (Bash)
├── instructions_example.txt     ← Sample batch file for automation
├── implementation_log.txt       ← Build log with timestamps
├── demo/
│   ├── main.c                   ← Demo: explicit builtin (with noinline wrapper)
│   ├── mainclean.c              ← Demo: no wrapper (documents the reordering bug)
│   ├── main_idiom.c             ← Demo: plain C attention (GIMPLE pass detects it)
│   ├── main.o                   ← Compiled object file
│   ├── main_objdump.txt         ← Disassembly output
│   ├── main_hexdump.txt         ← Hex dump of .text section
│   └── build_and_dump.sh        ← Build script
└── src/
    ├── riscv-opc.h.additions    ← File 1: MATCH/MASK defines
    ├── riscv-opc.c.addition     ← File 2: opcode table entry
    ├── riscv-ftypes.def.addition← File 3: function type signature
    ├── riscv-builtins.cc.additions ← File 4: builtin registration
    ├── riscv.md.additions       ← File 5: machine description pattern
    ├── rv_custom                ← File 6: riscv-opcodes format entry
    └── riscv-attn-detect.cc     ← File 7: GIMPLE idiom detection pass
```

---

## Summary

| Item | Value |
|------|-------|
| Instruction | `attn rd, rs1, rs2` |
| Opcode slot | custom-0 (`0x0b`) |
| Format | R-type |
| MATCH | `0x0200000b` |
| MASK | `0xfe00707f` |
| GCC builtin | `__builtin_riscv_attn()` |
| GIMPLE pass | `riscv_attn_detect` (auto-detects attention loops) |
| Toolchain target | `riscv64-unknown-elf` |
| Architecture | `rv64imac` / `lp64` |
| Files modified | 6 (across binutils + GCC) + GIMPLE pass |
| Inline assembly | None |
