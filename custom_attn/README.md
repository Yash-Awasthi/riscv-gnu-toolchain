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

### How it works

The user writes standard C with loops:

```c
void attention(int seq_len, int d_model,
               float Q[][d_model], float K[][d_model],
               float V[][d_model], float output[][d_model])
{
    float score[seq_len][seq_len];

    // Stage 1: Q * K^T → score
    for (i...) for (j...) for (k...)
        score[i][j] += Q[i][k] * K[j][k];

    // Stage 2: scale
    for (i...) for (j...)
        score[i][j] /= sqrtf(d_model);

    // Stage 3: softmax per row
    for (i...) {
        max_val = max(score[i][...]);
        sum = Σ exp(score[i][j] - max_val);
        score[i][j] = exp(score[i][j] - max_val) / sum;
    }

    // Stage 4: score * V → output
    for (i...) for (j...) for (k...)
        output[i][j] += score[i][k] * V[k][j];
}
```

The compiler's GIMPLE pass scans for these 4 consecutive loop nests, verifies the array bases chain correctly (all operating on the same `score` array), and replaces them with a single `__builtin_riscv_attn()` call — which the existing pipeline (files 1-6) lowers to the `attn` instruction.

### Files involved

| File | Location (in GCC submodule) | Purpose |
|------|---------------------------|---------|
| `riscv-attn-detect.cc` | `gcc/gcc/config/riscv/` | The GIMPLE pass (~700 lines) |
| `t-riscv` modification | `gcc/gcc/config/riscv/` | Add .o to build |
| `riscv.cc` modification | `gcc/gcc/config/riscv/` | Register pass dynamically |
| `tree-pass.h` modification | `gcc/gcc/` | Declare factory function |

Reference copies of all modifications are in `custom_attn/src/`:
- `riscv-attn-detect.cc` — the complete pass
- `t-riscv.addition` — build rule to add
- `riscv.cc.addition` — registration code to add
- `tree-pass.h.addition` — declaration to add

### Detection algorithm

1. **Stage 1 (matmul Q×K^T)**: Triple-nested loop with reduction `score[i][j] += Q[i][k] * K[j][k]`. K transposition detected by both arrays sharing the inner IV.
2. **Stage 2 (scale)**: Double-nested loop `score[i][j] /= sqrt(d)` on the same array base.
3. **Stage 3 (softmax)**: Outer loop with 3 child loops: max-reduction, sum-of-exp, normalize.
4. **Stage 4 (matmul score×V)**: Triple-nested loop `output[i][j] += score[i][k] * V[k][j]` with score as first operand.

### Compile and verify

```bash
# Compile with tree dump to see the pass in action
riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \
    -fdump-tree-riscv_attn_detect-details \
    -c demo/main_idiom.c -o main_idiom.o

# Check the dump for detection confirmation
cat main_idiom.c.*t.riscv_attn_detect

# Verify attn instruction in disassembly
riscv64-unknown-elf-objdump -d main_idiom.o | grep attn
```

### Demo file

`demo/main_idiom.c` — Contains a plain C `attention()` function with the 4-stage loop pattern. Compiles to `attn` automatically. Compare with `demo/main.c` (explicit builtin) — both produce the same instruction.

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
    ├── riscv-attn-detect.cc     ← File 7: GIMPLE idiom detection pass
    ├── tree-pass.h.addition     ← File 7: tree-pass.h declaration
    ├── riscv.cc.addition        ← File 7: dynamic pass registration
    └── t-riscv.addition         ← File 7: build system integration
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
