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
| funct7 | [31:25] | `0x00` |

```
MATCH_ATTN = 0x0000000b
MASK_ATTN  = 0xfe00707f
```

R-type format. Uses the `custom-0` opcode slot reserved by RISC-V for extensions.

---

## Step-by-Step: How to Build Everything from Scratch

### Step 1 — Find a Free Opcode Slot

Before defining any custom instruction, you must find an opcode that does not conflict with existing instructions. RISC-V reserves four custom slots: custom-0 (`0x0B`), custom-1 (`0x2B`), custom-2 (`0x5B`), custom-3 (`0x7B`). But you still need to verify they are actually free.

**Step 1a** — Clone the riscv-opcodes repository (official opcode definitions):

```
cd ~
git clone https://github.com/riscv/riscv-opcodes
cd riscv-opcodes
```

**Step 1b** — Extract all opcodes already used by extensions:

```
grep -o "6..2=0x[0-9A-Fa-f]*" -R extensions/ | sed 's/.*=0x//' | awk '{printf "0x%02x\n", strtonum("0x"$1)}' | sort -u > used_clean.txt
```

This searches every file in `extensions/` for patterns like `6..2=0x02`, extracts the hex values, and saves the sorted unique list.

**Step 1c** — Generate all possible 5-bit opcode values (0x00 to 0x1f):

```
for i in $(seq 0 31); do printf "0x%02x\n" $i; done > all_clean.txt
```

**Step 1d** — Find opcodes that are free (in all_clean but not in used_clean):

```
comm -23 all_clean.txt used_clean.txt > free_from_extensions.txt
cat free_from_extensions.txt
```

`comm -23` outputs lines that are only in the first file. These are your free opcode slots.

**Step 1e** — Cross-check against binutils (in case binutils has extra opcodes not in riscv-opcodes):

```
grep -oP '"[A-Z_]+",\s*0x\K[0-9a-fA-F]+' ~/dc/riscv-gnu-toolchain/binutils/gas/config/tc-riscv.c | awk '{printf "0x%02x\n", strtonum("0x"$1)}' | sort -u > used_binutils.txt
comm -23 free_from_extensions.txt used_binutils.txt
```

This confirms the slot is truly free in both the spec and the assembler source.

**Result for us:** Opcode slot `0x02` (which maps to `custom-0`, full 7-bit opcode `0x0B`) is confirmed free.

---

### Step 1f — Understanding What `0x02` Actually Is

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

---

### Step 1g — Computing MATCH (The Instruction's Fingerprint)

MATCH is a 32-bit number where every fixed field of the instruction is filled in and every variable field (registers) is zero.

For an R-type instruction, the 32-bit layout is:

```
31       25 24    20 19    15 14  12 11     7 6      0
┌──────────┬────────┬────────┬──────┬────────┬────────┐
│  funct7  │  rs2   │  rs1   │funct3│   rd   │ opcode │
│  7 bits  │ 5 bits │ 5 bits │3 bits│ 5 bits │ 7 bits │
└──────────┴────────┴────────┴──────┴────────┴────────┘
```

We chose: opcode = `0x0B`, funct3 = `0`, funct7 = `0`. The register fields (rd, rs1, rs2) are variable — they change depending on which registers the programmer uses — so they are 0 in MATCH.

```
MATCH = opcode | (funct3 << 12) | (funct7 << 25)

Step 1: opcode = 0x0B
        In binary (32 bits): 0000_0000_0000_0000_0000_0000_0000_1011

Step 2: funct3 << 12 = 0x0 << 12 = 0x00000000
        (funct3 is 0, so shifting it changes nothing)

Step 3: funct7 << 25 = 0x00 << 25 = 0x00000000
        (funct7 is 0, so shifting it changes nothing)

Step 4: OR them together:
        0x0000000B | 0x00000000 | 0x00000000 = 0x0000000B

MATCH_ATTN = 0x0000000B
```

If we had picked funct7 = 1 instead (for a second instruction in the same slot):

```
MATCH = 0x0B | (0 << 12) | (1 << 25)
      = 0x0B | 0          | 0x02000000
      = 0x0200000B
```

---

### Step 1h — Computing MASK (Which Bits Identify the Instruction)

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

---

### Step 1i — Verification: Does (instruction & MASK) == MATCH?

Our compiled instruction from objdump is `0x00B5050B` (which is `attn a0, a0, a1`). Let us verify:

```
instruction = 0x00B5050B
MASK        = 0xFE00707F
MATCH       = 0x0000000B

instruction & MASK:
  0000_0000_1011_0101_0000_0101_0000_1011   (0x00B5050B)
& 1111_1110_0000_0000_0111_0000_0111_1111   (0xFE00707F)
= 0000_0000_0000_0000_0000_0000_0000_1011   (0x0000000B)

0x0000000B == 0x0000000B  ✓  This IS the attn instruction!
```

Now extract the registers:

```
Full binary: 0000_0000_1011_0101_0000_0101_0000_1011

funct7 = bits[31:25] = 0000000 = 0        ← identifies instruction
rs2    = bits[24:20] = 01011   = 11 = a1  ← second argument
rs1    = bits[19:15] = 01010   = 10 = a0  ← first argument
funct3 = bits[14:12] = 000     = 0        ← identifies instruction
rd     = bits[11:7]  = 01010   = 10 = a0  ← destination
opcode = bits[6:0]   = 0001011 = 0x0B     ← identifies instruction

Result: attn a0, a0, a1  ✓
```

---

### Step 2 — Install Build Dependencies

On Ubuntu/Debian:

```
sudo apt-get install -y autoconf automake autotools-dev curl python3 libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev
```

These are the compilers and libraries needed to build GCC and binutils from source.

### Step 3 — Clone the Repo

```
mkdir -p ~/dc && cd ~/dc
git clone https://github.com/Yash-Awasthi/riscv-gnu-toolchain
cd riscv-gnu-toolchain
```

### Step 4 — Initialize Submodules

```
git submodule update --init binutils
git submodule update --init gcc
```

Binutils and GCC are **Git submodules** that pull from upstream sources (sourceware.org, gcc-mirror). After this step they contain clean, unmodified source code.

> **Important:** The submodules are always cloned fresh from upstream — they do NOT contain our custom instruction modifications yet. You must apply modifications in Step 5 (manually) or use the automation script (Step 5 — Automated Method) to patch them.

### Step 5 — Apply Source Modifications

There are 6 files to modify. You have two options:

**Option A — Automated (recommended):**
```
cd custom_attn
python3 automate_instruction.py --name attn --inputs 2 --desc "Transformer attention mechanism" --no-build
```
Or using bash:
```
cd custom_attn
./automate_instruction.sh add attn 2 "Transformer attention mechanism"
```
This patches all 6 files automatically. Skip to Step 6.

**Option B — Manual:**
Apply each modification by hand as described below.

---

#### File 1 — `binutils/include/opcode/riscv-opc.h`

This file stores the binary encoding of every RISC-V instruction.

Open the file, find the line `/* Instruction opcode macros.  */` (near line 23), and add this right after it:

```
#define MATCH_ATTN  0x0000000b
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
attn rd rs1 rs2 31..25=0 14..12=0 6..2=0x02 1..0=3
```

This follows the same format as `rv_i`, `rv64_m`, etc. — instruction name, operand fields, then bit-field constraints. `6..2=0x02` means custom-0 base opcode.

---

### Step 6 — Set Install Prefix

```
export PREFIX=$HOME/riscv_custom
export PATH=$PREFIX/bin:$PATH
```

All compiled tools will go into `~/riscv_custom/bin/`.

### Step 7 — Build Binutils

```
cd ~/dc/riscv-gnu-toolchain
mkdir -p build_binutils && cd build_binutils
../binutils/configure --target=riscv64-unknown-elf --prefix=$PREFIX --disable-werror
make -j$(nproc)
make install
```

This builds the assembler (`riscv64-unknown-elf-as`) and disassembler (`riscv64-unknown-elf-objdump`) with our custom `attn` instruction support.

### Step 8 — Build GCC (Stage 1)

```
cd ~/dc/riscv-gnu-toolchain
mkdir -p build_gcc && cd build_gcc
../gcc/configure --target=riscv64-unknown-elf --prefix=$PREFIX --disable-shared --disable-threads --disable-multilib --disable-libatomic --disable-libmudflap --disable-libssp --disable-libquadmath --disable-libgomp --disable-nls --disable-bootstrap --enable-languages=c --with-arch=rv64imac --with-abi=lp64 --with-newlib
make all-gcc -j$(nproc)
make install-gcc
```

This builds the cross-compiler (`riscv64-unknown-elf-gcc`) that knows about `__builtin_riscv_attn()`.

### Step 9 — Compile the Demo

```
cd ~/dc/riscv-gnu-toolchain/custom_attn/demo
riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 -ffreestanding -nostdinc -c main.c -o main.o
```

### Step 10 — Verify with objdump

```
riscv64-unknown-elf-objdump -d main.o
```

You should see:

```
0000000000000000 <run_attention>:
   0:   00b5050b    attn    a0,a0,a1
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

## Automation Script

An automation script is included at `custom_attn/automate_instruction.py`. It does the entire process automatically for any new instruction.

### Scan for free opcodes

```
python3 custom_attn/automate_instruction.py --scan 2
```

Shows all available opcode slots for 2-input instructions.

```
python3 custom_attn/automate_instruction.py --scan 3
```

Shows all available opcode slots for 3-input (R4-type) instructions.

### Add a new instruction automatically

```
python3 custom_attn/automate_instruction.py --name my_pow --inputs 2 --desc "(a^b)^a"
```

This will:
1. Find the next free opcode slot
2. Modify all 6 toolchain source files
3. Rebuild binutils and GCC
4. Generate `demo/main_my_pow.c` with `wrapper_my_pow()` calling `__builtin_riscv_my_pow()`
5. Compile and run objdump to verify

For 3-input instructions (R4-type encoding):

```
python3 custom_attn/automate_instruction.py --name fused_mac --inputs 3 --desc "a*b+c"
```

### What the generated C code looks like

For an instruction called `my_pow` with 2 inputs:

```c
__attribute__((noinline))
unsigned long wrapper_my_pow(unsigned long arg0_addr, unsigned long arg1_addr)
{
    return __builtin_riscv_my_pow(arg0_addr, arg1_addr);
}

int main(void)
{
    float A = 2.0f;
    float B = 4.0f;

    volatile unsigned long result = wrapper_my_pow(
        (unsigned long)&A,
        (unsigned long)&B
    );

    (void)result;
    return 0;
}
```

---

## Repository Structure

```
custom_attn/
├── README.md                    ← This file
├── DOCUMENTATION.md             ← Detailed teaching document (viva prep)
├── GENERIC_TEMPLATE.md          ← Template for adding any new instruction
├── OPCODE_FIELDS.md             ← Opcode/operand field reference
├── automate_instruction.py      ← Automation script
├── implementation_log.txt       ← Build log with timestamps
├── demo/
│   ├── main.c                   ← Demo C program (attn instruction)
│   ├── main.o                   ← Compiled object file
│   ├── main_objdump.txt         ← Disassembly output
│   ├── main_hexdump.txt         ← Hex dump of .text section
│   └── build_and_dump.sh        ← Build script
└── src/
    ├── riscv-opc.h.additions    ← Reference: what was added to riscv-opc.h
    ├── riscv-opc.c.addition     ← Reference: what was added to riscv-opc.c
    ├── riscv-ftypes.def.addition← Reference: what was added to ftypes.def
    ├── riscv-builtins.cc.additions ← Reference: what was added to builtins.cc
    ├── riscv.md.additions       ← Reference: what was added to riscv.md
    └── rv_custom                ← Reference: riscv-opcodes format entry
```

---

## Summary

| Item | Value |
|------|-------|
| Instruction | `attn rd, rs1, rs2` |
| Opcode slot | custom-0 (`0x0b`) |
| Format | R-type |
| MATCH | `0x0000000b` |
| MASK | `0xfe00707f` |
| GCC builtin | `__builtin_riscv_attn()` |
| Toolchain target | `riscv64-unknown-elf` |
| Architecture | `rv64imac` / `lp64` |
| Files modified | 5 (across binutils + GCC) |
| Inline assembly | None |
