# RISC-V Custom Instruction — Complete Documentation
### Everything You Need to Know (Group 9 — Attention Mechanism)

---

## Table of Contents

1. [What is RISC-V?](#1-what-is-risc-v)
2. [What is a Custom Instruction?](#2-what-is-a-custom-instruction)
3. [What is the GNU Toolchain?](#3-what-is-the-gnu-toolchain)
4. [How a C Program Becomes Machine Code](#4-how-a-c-program-becomes-machine-code)
5. [Understanding Opcodes — The Language of the CPU](#5-understanding-opcodes--the-language-of-the-cpu)
6. [RISC-V Instruction Formats](#6-risc-v-instruction-formats)
7. [The custom-0 Through custom-3 Opcode Slots](#7-the-custom-0-through-custom-3-opcode-slots)
8. [How to Find a Free Opcode — Step by Step](#8-how-to-find-a-free-opcode--step-by-step)
9. [MATCH and MASK — How the CPU Identifies Instructions](#9-match-and-mask--how-the-cpu-identifies-instructions)
10. [Our Instruction: attn](#10-our-instruction-attn)
11. [File 1 — riscv-opc.h (The Encoding Registry)](#11-file-1--riscv-opch-the-encoding-registry)
12. [File 2 — riscv-opc.c (The Assembler's Lookup Table)](#12-file-2--riscv-opcc-the-assemblers-lookup-table)
13. [File 3 — riscv-ftypes.def (Function Type Signature)](#13-file-3--riscv-ftypesdef-function-type-signature)
14. [File 4 — riscv-builtins.cc (Builtin Registration)](#14-file-4--riscv-builtinscc-builtin-registration)
15. [File 5 — riscv.md (Machine Description)](#15-file-5--riscvmd-machine-description)
16. [Building the Toolchain — Every Command Explained](#16-building-the-toolchain--every-command-explained)
17. [The Demo C Program — Line by Line](#17-the-demo-c-program--line-by-line)
18. [Compiling and Verifying — Every Command Explained](#18-compiling-and-verifying--every-command-explained)
19. [Reading the objdump Output](#19-reading-the-objdump-output)
20. [The Automation Script — How It Works](#20-the-automation-script--how-it-works)
21. [R4-Type — 3-Input Instructions](#21-r4-type--3-input-instructions)
22. [Viva Questions and Answers](#22-viva-questions-and-answers)

---

## 1. What is RISC-V?

RISC-V (pronounced "risk-five") is an **open-source instruction set architecture** (ISA). An ISA is the contract between software and hardware — it defines what instructions a CPU understands.

Unlike x86 (owned by Intel/AMD) or ARM (owned by ARM Holdings), RISC-V is **free and open**. Anyone can design a RISC-V CPU. This is why universities and research labs use it — you can modify it without paying licensing fees.

**RISC** stands for **Reduced Instruction Set Computer**. This means:
- Each instruction does one simple thing
- All instructions are the same size (32 bits in RV32/RV64)
- There are few instruction formats (R, I, S, B, U, J)
- The CPU hardware is simpler and faster

**RV64IMAC** — this is the specific RISC-V configuration we target:
- **RV64** = 64-bit base integer ISA (registers are 64 bits wide)
- **I** = Base integer instructions (add, sub, load, store, branch)
- **M** = Multiply/divide extension
- **A** = Atomic operations extension
- **C** = Compressed instructions extension (16-bit short forms of common instructions)

---

## 2. What is a Custom Instruction?

RISC-V was designed from the start to be **extensible**. The ISA specification reserves specific opcode slots where you can add your own instructions without conflicting with any standard instruction.

A custom instruction is an instruction that does not exist in the base RISC-V spec. You define:
- What binary pattern represents it (the encoding)
- What the assembler should call it (the mnemonic, like `attn`)
- What it does (the semantics — in our case, the attention mechanism)

In our project, we added the instruction `attn` which conceptually computes:

```
output = softmax(Q × Kᵀ / √dₖ) × V
```

This is the core operation of the Transformer architecture used in models like GPT and BERT.

In a real chip, this instruction would trigger a hardware accelerator. In our project, we are proving that the **toolchain** (compiler + assembler + disassembler) can handle the instruction. The hardware implementation would be a separate project.

---

## 3. What is the GNU Toolchain?

The GNU Toolchain is a collection of programming tools that turn your C code into machine code. For RISC-V, it includes:

| Tool | What It Does | Binary Name |
|------|-------------|-------------|
| **GCC** | C compiler — turns `.c` files into assembly `.s` files | `riscv64-unknown-elf-gcc` |
| **GAS** (GNU Assembler) | Assembler — turns `.s` assembly into `.o` object files | `riscv64-unknown-elf-as` |
| **LD** (GNU Linker) | Linker — combines `.o` files into executables | `riscv64-unknown-elf-ld` |
| **objdump** | Disassembler — shows you the instructions inside a `.o` file | `riscv64-unknown-elf-objdump` |
| **binutils** | The package that contains the assembler, linker, and objdump | — |

The name `riscv64-unknown-elf` is the **target triplet**:
- `riscv64` = the architecture (64-bit RISC-V)
- `unknown` = the vendor (generic, no specific vendor)
- `elf` = the binary format (ELF = Executable and Linkable Format, the standard on Linux)

This is a **cross-compiler** — it runs on your x86 laptop but produces RISC-V binaries. You cannot run the output directly on your laptop; you would need a RISC-V CPU or emulator (like QEMU).

---

## 4. How a C Program Becomes Machine Code

Here is the complete pipeline:

```
main.c (C source code)
    │
    │  GCC compiler
    │  Recognizes __builtin_riscv_attn()
    │  Looks up the machine description (riscv.md)
    │  Generates: "attn a0,a0,a1" assembly text
    │
    ▼
main.s (assembly source)
    │
    │  GAS assembler (part of binutils)
    │  Reads riscv-opc.c opcode table
    │  Finds "attn" → MATCH_ATTN = 0x0000000b
    │  Encodes registers into the 32-bit instruction
    │  Outputs binary: 0x00b5050b
    │
    ▼
main.o (object file — raw machine code)
    │
    │  objdump (part of binutils)
    │  Reads the binary, uses riscv-opc.c + riscv-opc.h
    │  Recognizes 0x00b5050b as "attn a0,a0,a1"
    │  Displays human-readable disassembly
    │
    ▼
Disassembly output (what we see in the terminal)
```

Every file we modify is part of this pipeline. That is why we need to touch both binutils (for the assembler and disassembler) and GCC (for the compiler).

---

## 5. Understanding Opcodes — The Language of the CPU

An **opcode** (operation code) is the binary number that tells the CPU which instruction to execute.

Every RISC-V instruction is exactly 32 bits wide (4 bytes). Inside those 32 bits, different sections (called "fields") carry different information:

```
31       25 24    20 19    15 14  12 11     7 6      0
┌──────────┬────────┬────────┬──────┬────────┬────────┐
│  funct7  │  rs2   │  rs1   │funct3│   rd   │ opcode │
│ (7 bits) │(5 bits)│(5 bits)│(3 b) │(5 bits)│(7 bits)│
└──────────┴────────┴────────┴──────┴────────┴────────┘
```

- **opcode** (bits [6:0]) — Identifies the *category* of instruction (is it an add? a load? a branch? a custom instruction?)
- **rd** (bits [11:7]) — The destination register (where the result goes)
- **funct3** (bits [14:12]) — Further narrows down which instruction within the category
- **rs1** (bits [19:15]) — First source register
- **rs2** (bits [24:20]) — Second source register
- **funct7** (bits [31:25]) — Even further narrows down the instruction

Think of it like an address: opcode = country, funct3 = city, funct7 = street. Together they uniquely identify one instruction.

### Registers

RISC-V has 32 integer registers, named `x0` through `x31`. They also have human-readable aliases:

| Register | Alias | Purpose |
|----------|-------|---------|
| x0 | zero | Always contains 0, writes are ignored |
| x1 | ra | Return address |
| x2 | sp | Stack pointer |
| x10 | a0 | Function argument 1 / return value |
| x11 | a1 | Function argument 2 |
| x12 | a2 | Function argument 3 |
| ... | ... | ... |

In our instruction `attn a0, a0, a1`:
- `rd` = `a0` (x10) — the result register
- `rs1` = `a0` (x10) — first argument (dims struct address)
- `rs2` = `a1` (x11) — second argument (qkv struct address)

Each register number is 5 bits (because 2⁵ = 32 registers). So `a0` = register 10 = `01010` in binary, and `a1` = register 11 = `01011` in binary.

---

## 6. RISC-V Instruction Formats

RISC-V defines 6 standard instruction formats. We use **R-type** for 2-input instructions and **R4-type** for 3-input instructions.

### R-type (Register-Register) — Used for `attn`

```
31       25 24    20 19    15 14  12 11     7 6      0
┌──────────┬────────┬────────┬──────┬────────┬────────┐
│  funct7  │  rs2   │  rs1   │funct3│   rd   │ opcode │
└──────────┴────────┴────────┴──────┴────────┴────────┘
   7 bits    5 bits   5 bits  3 bits  5 bits   7 bits  = 32 bits
```

R-type is used when an instruction takes two source registers and produces one result. Examples: `add`, `sub`, `and`, `or`, and our `attn`.

### R4-type (Four-Register) — Used for 3-input custom instructions

```
31    27 26 25 24    20 19    15 14  12 11     7 6      0
┌────────┬─────┬──────┬────────┬──────┬────────┬────────┐
│  rs3   │ f2  │ rs2  │  rs1   │funct3│   rd   │ opcode │
└────────┴─────┴──────┴────────┴──────┴────────┴────────┘
  5 bits  2 bits 5 bits 5 bits  3 bits  5 bits   7 bits  = 32 bits
```

R4-type is used when an instruction takes three source registers. The funct7 field is split into rs3 (5 bits) + funct2 (2 bits). Used for fused multiply-add operations like `fmadd`.

---

## 7. The custom-0 Through custom-3 Opcode Slots

RISC-V reserves four opcode values specifically for custom extensions:

| Name | Opcode value (bits [6:0]) | Binary | Free to use |
|------|---------------------------|--------|-------------|
| custom-0 | `0x0B` | `000_1011` | Yes |
| custom-1 | `0x2B` | `010_1011` | Yes |
| custom-2 | `0x5B` | `101_1011` | Yes |
| custom-3 | `0x7B` | `111_1011` | Yes |

These will **never** be used by the official RISC-V specification. They exist precisely so that people like us can add custom instructions.

Within each slot, you can use different `funct3` and `funct7` values to create many different instructions. For R-type:
- 8 possible funct3 values (3 bits: 0-7)
- 128 possible funct7 values (7 bits: 0-127)
- Total: 8 × 128 = **1,024 different instructions per custom slot**
- Across all 4 slots: **4,096 possible custom instructions**

Our `attn` instruction uses: custom-0, funct3=0, funct7=0. That is the very first slot.

---

## 8. How to Find a Free Opcode — Step by Step

Before you can add a custom instruction, you must **prove** that the opcode slot you want to use is not already taken by any standard or vendor extension. Here is exactly how to do it.

### Step 8.1 — Clone the official riscv-opcodes repository

```
cd ~
git clone https://github.com/riscv/riscv-opcodes
cd riscv-opcodes
```

This repository is maintained by the RISC-V foundation. It contains machine-readable definitions of every instruction in every official extension. The `extensions/` folder has one file per extension.

### Step 8.2 — Extract all opcodes used by existing extensions

```
grep -o "6..2=0x[0-9A-Fa-f]*" -R extensions/ | sed 's/.*=0x//' | awk '{printf "0x%02x\n", strtonum("0x"$1)}' | sort -u > used_clean.txt
```

What each part of this command does:

| Part | What it does |
|------|-------------|
| `grep -o "6..2=0x[0-9A-Fa-f]*" -R extensions/` | Searches all files in `extensions/` recursively (`-R`) for the pattern `6..2=0xNN`. In riscv-opcodes format, `6..2=0x02` means "bits 6 down to 2 of the opcode field equal 0x02". The `-o` flag prints only the matching part, not the whole line. |
| `sed 's/.*=0x//'` | Strips everything before and including `=0x`, leaving just the hex value (e.g., `02`). `sed` is a stream editor — `s/pattern/replacement/` means substitute. |
| `awk '{printf "0x%02x\n", strtonum("0x"$1)}'` | Formats each value as a proper hex number like `0x02`. `strtonum` converts the string to a number, `%02x` formats it as 2-digit lowercase hex. |
| `sort -u` | Sorts the list and removes duplicates (`-u` = unique). |
| `> used_clean.txt` | Saves the output to a file. |

The output `used_clean.txt` contains every opcode value (bits [6:2]) that is already claimed by some extension.

### Step 8.3 — Generate all possible opcode values

```
for i in $(seq 0 31); do printf "0x%02x\n" $i; done > all_clean.txt
```

Bits [6:2] are 5 bits, so there are 2^5 = 32 possible values (0 through 31). This command generates all of them. `seq 0 31` produces numbers 0 to 31, and `printf "0x%02x\n"` formats each one as hex.

### Step 8.4 — Find which opcodes are free

```
comm -23 all_clean.txt used_clean.txt > free_from_extensions.txt
cat free_from_extensions.txt
```

`comm` compares two sorted files line by line. The flags `-23` suppress columns 2 and 3, showing only lines unique to the first file (i.e., opcodes that exist in `all_clean.txt` but NOT in `used_clean.txt`). These are the free slots.

### Step 8.5 — Cross-check against binutils

The riscv-opcodes repo might not have every vendor extension. To be extra safe, also check what the assembler already knows about:

```
grep -oP '"[A-Z_]+",\s*0x\K[0-9a-fA-F]+' ~/dc/riscv-gnu-toolchain/binutils/gas/config/tc-riscv.c | awk '{printf "0x%02x\n", strtonum("0x"$1)}' | sort -u > used_binutils.txt
comm -23 free_from_extensions.txt used_binutils.txt
```

| Part | What it does |
|------|-------------|
| `grep -oP '"[A-Z_]+",\s*0x\K[0-9a-fA-F]+'` | Searches `tc-riscv.c` (the assembler's RISC-V handler) for opcode constants. `-P` enables Perl-compatible regex. `\K` resets the match start, so only the hex value after `0x` is captured. |
| `comm -23 free_from_extensions.txt used_binutils.txt` | Shows opcodes that are free in extensions AND not used by binutils. |

### Step 8.6 — Result

For our project, opcode slot `0x02` (bits [6:2]) is confirmed free in both the specification and binutils. This maps to:

```
Full 7-bit opcode = (0x02 << 2) | 0x03 = 0x0B = custom-0
```

The bottom 2 bits of any standard 32-bit RISC-V opcode are always `11` (binary) = `0x3`. So the 5-bit value `0x02` becomes the 7-bit value `0x0B`.

Let us work through the math carefully:

**What does `bits[6:2] = 0x02` mean?**

The notation `[6:2]` means "bit 6 down to bit 2" — that is 5 bits. In a 32-bit instruction, bit 0 is the rightmost bit (least significant) and bit 31 is the leftmost (most significant). So `bits[6:2]` are the 5 bits at positions 6, 5, 4, 3, 2.

The full 7-bit opcode field occupies `bits[6:0]` = positions 6, 5, 4, 3, 2, 1, 0. The riscv-opcodes repo only stores `bits[6:2]` because `bits[1:0]` are always `11` for 32-bit instructions (this is defined by the RISC-V spec — it is how the CPU distinguishes 32-bit instructions from 16-bit compressed instructions).

**Converting 5-bit value to 7-bit opcode:**

```
bits[6:2] = 0x02 = 00010 in binary (5 bits)
bits[1:0] = 0x3  = 11    in binary (2 bits, always fixed)

Combine: bits[6:0] = 00010 ++ 11 = 0001011 (7 bits)

Convert 0001011 to hex:
  0001011 = 0×64 + 0×32 + 0×16 + 1×8 + 0×4 + 1×2 + 1×1
          = 8 + 2 + 1 = 11 decimal = 0x0B

Shortcut formula: full_opcode = (bits_6_2 << 2) | 0x03
                              = (0x02 << 2) | 0x03
                              = (0x02 × 4)  | 0x03
                              = 0x08 | 0x03
                              = 0x0B
```

**What does `<< 2` mean?** It means "shift left by 2 bits" — equivalent to multiplying by 4. This makes room for the 2 fixed bits at positions [1:0].

**What does `|` mean?** It means bitwise OR — it combines the bits. `0x08 | 0x03` = `0000_1000 | 0000_0011` = `0000_1011` = `0x0B`.

For reference, all four custom slots:

| Grep value [6:2] | Binary [6:2] | Append 11 | Binary [6:0] | Hex [6:0] | Name |
|-------------------|-------------|-----------|-------------|-----------|------|
| `0x02` | `00010` | `00010` ++ `11` | `0001011` | `0x0B` | custom-0 |
| `0x0A` | `01010` | `01010` ++ `11` | `0101011` | `0x2B` | custom-1 |
| `0x16` | `10110` | `10110` ++ `11` | `1011011` | `0x5B` | custom-2 |
| `0x1E` | `11110` | `11110` ++ `11` | `1111011` | `0x7B` | custom-3 |

### Step 8.7 — Computing MATCH: The Instruction's Bit Fingerprint

Now we have the opcode (`0x0B`). We also choose funct3 and funct7 — we pick `0` for both (first available slot). Together, these three values uniquely identify our instruction.

MATCH is a 32-bit number where we put the opcode, funct3, and funct7 in their correct bit positions, and set all register fields to zero.

The R-type instruction layout (for reference):

```
Bit:    31       25 24    20 19    15 14  12 11     7 6      0
Field:  funct7      rs2      rs1     funct3   rd      opcode
Ours:   0000000    00000    00000     000    00000   0001011
```

```
MATCH = opcode | (funct3 << 12) | (funct7 << 25)
```

What does `<< 12` mean? It shifts the value left by 12 bit positions. funct3 occupies bits [14:12], so its value needs to be shifted left by 12 to land in the right position.

What does `<< 25` mean? funct7 occupies bits [31:25], so its value needs to be shifted left by 25.

Working it out:

```
opcode = 0x0B = 0000_0000_0000_0000_0000_0000_0000_1011
                                                 ^^^^^^^ bits [6:0]

funct3 << 12 = 0x0 << 12 = 0
  (0 shifted by any amount is still 0)

funct7 << 25 = 0x00 << 25 = 0
  (0 shifted by any amount is still 0)

MATCH = 0x0000000B | 0x00000000 | 0x00000000 = 0x0000000B
```

**Example with non-zero funct7:** If we picked funct7 = 1 for a second instruction:

```
funct7 << 25 = 1 << 25 = 0x02000000

In binary: 0000_0010_0000_0000_0000_0000_0000_0000
                ^^                                   bit 25 is set

MATCH = 0x0000000B | 0x00000000 | 0x02000000 = 0x0200000B
```

### Step 8.8 — Computing MASK: Which Bits Identify This Instruction

MASK is a 32-bit number where every bit that is part of an "identifying field" (opcode, funct3, funct7) is set to `1`, and every bit that is part of a "variable field" (rd, rs1, rs2) is set to `0`.

```
Build it field by field:

opcode [6:0]   = 7 bits  → 1111111  (these identify the instruction)
rd     [11:7]  = 5 bits  → 00000    (these vary per usage)
funct3 [14:12] = 3 bits  → 111      (these identify the instruction)
rs1    [19:15] = 5 bits  → 00000    (these vary per usage)
rs2    [24:20] = 5 bits  → 00000    (these vary per usage)
funct7 [31:25] = 7 bits  → 1111111  (these identify the instruction)

Concatenate from bit 31 down to bit 0:
  1111111 00000 00000 111 00000 1111111

Group into nibbles (4 bits each, from the right):
  1111 1110 0000 0000 0111 0000 0111 1111
  F    E    0    0    7    0    7    F

MASK = 0xFE00707F
```

**Let us verify the nibble conversion step by step:**

```
Binary:  1111  1110  0000  0000  0111  0000  0111  1111
Hex:       F     E     0     0     7     0     7     F

How to convert each nibble:
  1111 = 8+4+2+1 = 15 = F
  1110 = 8+4+2+0 = 14 = E
  0000 = 0
  0111 = 4+2+1   = 7
  1111 = 15 = F
```

**Sanity check:** Count the 1-bits in the MASK:
- opcode contributes 7 ones
- funct3 contributes 3 ones
- funct7 contributes 7 ones
- Total: 17 ones, 15 zeros (from rd + rs1 + rs2 = 5+5+5). Correct.

### Step 8.9 — Verification: The Full Check

Our compiled instruction from objdump is `0x00B5050B` (which decodes to `attn a0, a0, a1`).

The identification check: `(instruction & MASK) == MATCH`

```
instruction = 0x00B5050B
MASK        = 0xFE00707F

Step 1: AND them together (bit by bit, 1&1=1, anything else=0):

  0000_0000_1011_0101_0000_0101_0000_1011   (0x00B5050B)
& 1111_1110_0000_0000_0111_0000_0111_1111   (0xFE00707F)
= 0000_0000_0000_0000_0000_0000_0000_1011   (0x0000000B)

Step 2: Compare with MATCH:
  0x0000000B == 0x0000000B  ✓  This IS the attn instruction!
```

Now extract the register values from the variable fields:

```
Full binary of 0x00B5050B:
  0000_0000_1011_0101_0000_0101_0000_1011

funct7 = bits[31:25] = 0000000 = 0         ← part of instruction identity
rs2    = bits[24:20] = 01011   = 11 = x11 = a1   ← second source register
rs1    = bits[19:15] = 01010   = 10 = x10 = a0   ← first source register
funct3 = bits[14:12] = 000     = 0         ← part of instruction identity
rd     = bits[11:7]  = 01010   = 10 = x10 = a0   ← destination register
opcode = bits[6:0]   = 0001011 = 0x0B      ← part of instruction identity

Register number to name: x10 = a0, x11 = a1  (RISC-V ABI naming)
Result: attn a0, a0, a1  ✓  (matches objdump output exactly)
```

This is exactly what the automation script does — it scans for the first unused funct3/funct7 combination and computes MATCH and MASK automatically.

---

## 9. MATCH and MASK — How the CPU Identifies Instructions

When the CPU (or the disassembler) reads a 32-bit instruction from memory, it needs to figure out which instruction it is. It does this using **MATCH** and **MASK** values.

### MASK — "Which bits do I care about?"

The MASK tells you which bits of the 32-bit instruction are significant for identification. A `1` in the mask means "this bit matters", a `0` means "ignore this bit".

For R-type instructions:

```
MASK = 0xFE00707F

Binary: 1111_1110_0000_0000_0111_0000_0111_1111
         ^^^^^^^^                 ^^^      ^^^^^^^
         funct7                  funct3    opcode
         (7 bits matter)         (3 bits)  (7 bits)

Bits that are 0 in the mask (ignored):
         rs2 (bits 24-20), rs1 (bits 19-15), rd (bits 11-7)
```

This makes sense — the register fields change depending on which registers you use, but the instruction identity stays the same.

### MATCH — "What should those bits be?"

The MATCH value is the actual bit pattern that must appear in the "cared about" positions.

For `attn`:

```
MATCH = 0x0000000B

Binary: 0000_0000_0000_0000_0000_0000_0000_1011
        ^^^^^^^^                 ^^^      ^^^^^^^
        funct7=0x00             funct3=0  opcode=0x0B
```

### How identification works

```
instruction_from_memory & MASK == MATCH
```

If this is true, the instruction is `attn`. Let us verify with the actual binary we saw in objdump:

```
Encoded instruction: 0x00B5050B

0x00B5050B & 0xFE00707F = ?

  0000_0000_1011_0101_0000_0101_0000_1011   (0x00B5050B)
& 1111_1110_0000_0000_0111_0000_0111_1111   (0xFE00707F)
= 0000_0000_0000_0000_0000_0000_0000_1011   (0x0000000B)

0x0000000B == 0x0000000B ✓  →  This is the attn instruction!
```

Now let us extract the registers from that same instruction:

```
Full encoding: 0x00B5050B
Binary: 0000_0000_1011_0101_0000_0101_0000_1011

rd    = bits [11:7]  = 01010  = 10 = a0  ✓
rs1   = bits [19:15] = 01010  = 10 = a0  ✓
rs2   = bits [24:20] = 01011  = 11 = a1  ✓
funct7= bits [31:25] = 0000000 = 0
funct3= bits [14:12] = 000     = 0
opcode= bits [6:0]   = 0001011 = 0x0B = custom-0
```

This decodes to: `attn a0, a0, a1` — exactly what objdump showed us.

---

## 10. Our Instruction: attn

### What it represents

The Transformer attention mechanism:

```
Attention(Q, K, V) = softmax(Q × Kᵀ / √dₖ) × V
```

Where:
- **Q** (Query) — a matrix of shape [seq_len × d_k]
- **K** (Key) — a matrix of shape [seq_len × d_k]
- **V** (Value) — a matrix of shape [seq_len × d_v]
- **dₖ** — the dimension of the key vectors (used for scaling)
- **Kᵀ** — K transposed
- **softmax** — applied row-wise to normalize attention weights

### Why two pointer arguments?

You cannot pass three entire matrices through CPU registers. Registers are 64 bits wide — they can hold one number. So instead, we pass **memory addresses** (pointers) to structs:

- `rs1` points to a struct containing the matrix dimensions (rows, columns, sequence length, model dimension)
- `rs2` points to a struct containing three pointers to the Q, K, and V matrices in memory

A hardware accelerator connected to this CPU would read the struct, DMA the matrices from memory into its own SRAM, and compute the attention.

### The struct definitions

```c
typedef struct {
    int rows;
    int cols;
    int seq_len;
    int d_model;
} attn_dims_t;

typedef struct {
    float *Q;
    float *K;
    float *V;
} attn_qkv_t;
```

---

## 11. File 1 — riscv-opc.h (The Encoding Registry)

**Full path:** `binutils/include/opcode/riscv-opc.h`

**What this file is:** A massive header file that contains the binary encoding of every single RISC-V instruction. It is a registry — a phone book of instruction encodings. Both the assembler and the disassembler include this file.

**What we added:**

```
#define MATCH_ATTN  0x0000000b
#define MASK_ATTN   0xfe00707f
```

These two lines define the binary identity of our instruction (explained in detail in section 8).

```
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)
```

This is a macro that registers the instruction in a table. The macro expands differently depending on context — sometimes it generates code for the assembler, sometimes for the disassembler, sometimes for validation. That is why the file has an `#ifdef DECLARE_INSN` section.

**Why this file:** Without these definitions, neither the assembler nor the disassembler would know that the bit pattern `0x0000000b` corresponds to an instruction called `attn`.

**Where exactly to add:** The `#define` lines go in the main body of the file (after line 23, `/* Instruction opcode macros. */`), inside the `#ifndef RISCV_ENCODING_H` guard. The `DECLARE_INSN` line goes in the `#ifdef DECLARE_INSN` section at the bottom of the file.

**Do this:**

Open `binutils/include/opcode/riscv-opc.h`. Find the line that says `/* Instruction opcode macros.  */` (line 23). Add the two `#define` lines right after it. Then scroll to the very bottom, find the `#ifdef DECLARE_INSN` block, and add the `DECLARE_INSN` line at the end of that block (before `#endif`).

---

## 12. File 2 — riscv-opc.c (The Assembler's Lookup Table)

**Full path:** `binutils/opcodes/riscv-opc.c`

**What this file is:** The assembler's main lookup table. When you write `attn a0, a0, a1` in assembly, the assembler looks up "attn" in this table to find out how to encode it into binary.

**What we added:**

```
{"attn", 0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0},
```

Let us break down every single field in this entry:

| Field | Value | Meaning |
|-------|-------|---------|
| `"attn"` | — | The mnemonic. When the assembler sees this text, it matches this entry. |
| `0` | — | Instruction length override. 0 means "use the default" (32 bits). |
| `INSN_CLASS_I` | — | Instruction class. `I` means it belongs to the base integer ISA. It does NOT require floating-point hardware. If we used float registers, this would be `INSN_CLASS_F`. |
| `"d,s,t"` | — | **Operand format string.** This is critical. Each letter maps to a register field: |
| | `d` | = `rd` (destination register, bits [11:7]), integer register |
| | `s` | = `rs1` (source register 1, bits [19:15]), integer register |
| | `t` | = `rs2` (source register 2, bits [24:20]), integer register |
| | `,` | = literal comma separator in the assembly syntax |
| `MATCH_ATTN` | `0x0000000b` | The MATCH value (from riscv-opc.h) |
| `MASK_ATTN` | `0xfe00707f` | The MASK value (from riscv-opc.h) |
| `match_opcode` | — | A function pointer. This is the matching function the assembler uses to determine if an instruction matches this entry. `match_opcode` simply does `(insn & MASK) == MATCH`. |
| `0` | — | Flags. 0 means no special flags. |

**The operand format characters:**

| Char | Register Field | Type | Bits |
|------|---------------|------|------|
| `d` | rd | Integer | [11:7] |
| `s` | rs1 | Integer | [19:15] |
| `t` | rs2 | Integer | [24:20] |
| `r` | rs3 | Integer | [31:27] (R4-type only) |
| `D` | rd | Float | [11:7] |
| `S` | rs1 | Float | [19:15] |
| `T` | rs2 | Float | [24:20] |
| `R` | rs3 | Float | [31:27] |

So `"d,s,t"` means: the assembler expects `attn <integer_rd>, <integer_rs1>, <integer_rs2>`.

For a 3-input instruction, you would use `"d,s,t,r"` to add integer rs3.

**Why this file:** This is the bridge between the human-readable assembly (`attn a0, a0, a1`) and the binary encoding. Without this entry, writing `attn` in assembly would give you `Error: unrecognized opcode 'attn'`.

**Do this:**

Open `binutils/opcodes/riscv-opc.c`. Find the line `const struct riscv_opcode riscv_opcodes[] =` and the opening `{` right after it. Add the entry as the **first line** inside that array.

---

## 13. File 3 — riscv-ftypes.def (Function Type Signature)

**Full path:** `gcc/gcc/config/riscv/riscv-ftypes.def`

**What this file is:** A definition file that creates C function type signatures that GCC builtins can use. GCC needs to know the types of arguments and return values for every builtin function.

**What we added:**

```
DEF_RISCV_FTYPE (2, (DI, DI, DI))
```

Let us break this down:

| Part | Meaning |
|------|---------|
| `DEF_RISCV_FTYPE` | Macro that creates a function type |
| `2` | Number of arguments |
| `(DI, DI, DI)` | Type list: (return_type, arg1_type, arg2_type) |

**DI** stands for **Double Integer** — a 64-bit integer. In RISC-V 64-bit, this is the size of a pointer (an `unsigned long`).

Other type codes you might see:
- `SI` = Single Integer (32 bits, like `int`)
- `SF` = Single Float (32 bits, like `float`)
- `DF` = Double Float (64 bits, like `double`)
- `DI` = Double Integer (64 bits, like `unsigned long` or a pointer)

This macro creates a type named `RISCV_DI_FTYPE_DI_DI`, which means: "a function that returns DI and takes two DI arguments". The naming convention is:

```
RISCV_<return_type>_FTYPE_<arg1_type>_<arg2_type>
```

For a 3-argument function:

```
DEF_RISCV_FTYPE (3, (DI, DI, DI, DI))
```

This creates `RISCV_DI_FTYPE_DI_DI_DI`.

**Why this file:** GCC is a strongly typed compiler. Before it can create a builtin function, it needs to know the exact type signature. Without this, the builtin registration in the next file would fail because the type `RISCV_DI_FTYPE_DI_DI` would not exist.

**Do this:**

Open `gcc/gcc/config/riscv/riscv-ftypes.def`. Scroll to the very end of the file and add the `DEF_RISCV_FTYPE` line there.

---

## 14. File 4 — riscv-builtins.cc (Builtin Registration)

**Full path:** `gcc/gcc/config/riscv/riscv-builtins.cc`

**What this file is:** A C++ source file in GCC that registers all RISC-V builtin functions. A builtin is a function that the compiler recognizes specially — instead of generating a function call, it generates specific machine instructions directly.

When you write `__builtin_riscv_attn(x, y)` in C, GCC does not look for a library function called `attn`. Instead, it checks its builtin table, finds the entry we added, and emits the `attn` assembly instruction directly.

**What we added — three pieces:**

### Piece 1: Type mapping

```
#define RISCV_ATYPE_DI long_integer_type_node
```

This creates a mapping between our type code `DI` and GCC's internal type representation. `long_integer_type_node` is GCC's internal object representing the `long` type (64 bits on RV64).

Other mappings already exist:
- `RISCV_ATYPE_SI` → `intSI_type_node` (32-bit int)
- `RISCV_ATYPE_USI` → `unsigned_intSI_type_node` (unsigned 32-bit int)

We need `RISCV_ATYPE_DI` because our instruction uses 64-bit pointer-sized arguments. If this was already defined by someone else, the script skips it.

### Piece 2: Availability predicate

```
AVAIL (always_enabled, (!0))
```

This defines an availability predicate called `always_enabled`. It determines whether the builtin is available to the user.

The `AVAIL` macro creates a function `riscv_builtin_avail_always_enabled` that returns `(!0)`, which is C for `true`. This means our builtin is **always available**, regardless of which extensions are enabled.

Other builtins might have conditions like "only available when the F (float) extension is enabled". Ours has no such restriction because we use integer registers.

### Piece 3: Builtin registration

```
DIRECT_BUILTIN (attn, RISCV_DI_FTYPE_DI_DI, always_enabled),
```

This is the actual registration. Let us break it down:

| Part | Meaning |
|------|---------|
| `DIRECT_BUILTIN` | Macro that registers a builtin that maps directly to one instruction |
| `attn` | The instruction name. GCC will create `__builtin_riscv_attn()` from this |
| `RISCV_DI_FTYPE_DI_DI` | The function type (from file 3): returns DI, takes two DI args |
| `always_enabled` | The availability predicate (from piece 2): always available |

The `DIRECT_BUILTIN` macro does several things behind the scenes:
1. Creates a function called `__builtin_riscv_attn`
2. Sets its return type and argument types to match `RISCV_DI_FTYPE_DI_DI`
3. Maps it to the RTL pattern `"riscv_attn"` (which we define in file 5)
4. Uses the availability predicate to decide if the builtin is visible

**Why `__builtin_riscv_` prefix:** GCC naming convention. All RISC-V builtins get the prefix `__builtin_riscv_`. When you register `attn`, the user-facing name becomes `__builtin_riscv_attn`.

**Why this file:** This is where GCC learns that `__builtin_riscv_attn()` exists. Without this, calling `__builtin_riscv_attn()` in your C code would give: `error: implicit declaration of function '__builtin_riscv_attn'`.

**Do this:**

Open `gcc/gcc/config/riscv/riscv-builtins.cc`. You need to make three additions:
1. Find `#define RISCV_ATYPE_SI intSI_type_node` and add the `RISCV_ATYPE_DI` line right after it.
2. Find `AVAIL (hint_pause, (!0))` and add the `AVAIL (always_enabled, (!0))` line right after it.
3. Find `#include "corev.def"` and add the `DIRECT_BUILTIN` line right after it.

---

## 15. File 5 — riscv.md (Machine Description)

**Full path:** `gcc/gcc/config/riscv/riscv.md`

**What this file is:** GCC's Machine Description file for RISC-V. Written in a special pattern language called **RTL** (Register Transfer Language). This file tells GCC how to convert high-level operations into actual assembly instructions.

Every instruction the compiler can emit has a pattern in this file. When GCC decides it needs to emit the `attn` instruction, it looks for a pattern named `"riscv_attn"` in this file.

**What we added — two pieces:**

### Piece 1: UNSPEC constant

```
UNSPEC_ATTN
```

This is added inside the `(define_c_enum "unspec" [...])` block.

**What is UNSPEC?** GCC's RTL (internal representation) has standard operations like `plus`, `mult`, `and`, etc. But our `attn` instruction does not correspond to any standard operation. It is a completely custom operation. `UNSPEC` is GCC's way of saying "this is a special operation that GCC should not try to optimize or simplify — just emit it as-is."

Without `UNSPEC`, GCC might:
- Think the operation has no side effects and remove it ("dead code elimination")
- Try to combine it with other operations
- Reorder it in ways that break correctness

`UNSPEC_ATTN` is just a named constant (like an enum value) that uniquely identifies our custom operation.

### Piece 2: Instruction pattern

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

This is dense. Let us go through every single part:

**`(define_insn "riscv_attn"` ...**

This defines a new instruction pattern named `"riscv_attn"`. The name must match what `DIRECT_BUILTIN` generates — for builtin name `attn`, GCC looks for pattern `"riscv_attn"`.

**`[(set (match_operand:DI 0 "register_operand" "=r") ...)]`**

This is the RTL pattern. It says: "This instruction SETS operand 0."

| Part | Meaning |
|------|---------|
| `set` | This instruction writes a result |
| `match_operand:DI` | Operand type is DI (64-bit integer) |
| `0` | This is operand number 0 (the output/destination) |
| `"register_operand"` | It must be a register (not memory, not immediate) |
| `"=r"` | Constraint: `=` means "write-only output", `r` means "general-purpose register" |

**`(unspec:DI [(match_operand:DI 1 "register_operand" "r") (match_operand:DI 2 "register_operand" "r")] UNSPEC_ATTN)`**

| Part | Meaning |
|------|---------|
| `unspec:DI` | An unspecified (custom) operation producing a DI result |
| `match_operand:DI 1 "register_operand" "r"` | Input operand 1: a DI register, constraint `r` (read) |
| `match_operand:DI 2 "register_operand" "r"` | Input operand 2: a DI register, constraint `r` (read) |
| `UNSPEC_ATTN` | The UNSPEC identifier we defined in piece 1 |

**`""`**

The condition string. An empty string `""` means "this pattern is always valid, no special conditions needed." If we wanted it only available with a certain extension, we would put a C expression here.

**`"attn\t%0,%1,%2"`**

The **output template** — this is the actual assembly text GCC will emit. It is like printf:

| Part | Meaning |
|------|---------|
| `attn` | The instruction mnemonic |
| `\t` | A tab character (standard formatting in assembly) |
| `%0` | Replaced with operand 0 (the rd register, e.g., `a0`) |
| `%1` | Replaced with operand 1 (the rs1 register, e.g., `a0`) |
| `%2` | Replaced with operand 2 (the rs2 register, e.g., `a1`) |

So if GCC chooses registers a0, a0, a1, the output is: `attn	a0,a0,a1`

**`[(set_attr "type" "arith") (set_attr "mode" "DI")]`**

Attributes used by GCC's instruction scheduler:
- `type "arith"` — classifies this as an arithmetic instruction (helps the scheduler estimate latency)
- `mode "DI"` — the operation mode is 64-bit integer

**Why this file:** This is the final link in the chain. Without this pattern, GCC would know that `__builtin_riscv_attn()` exists (from file 4), but would not know what assembly to emit for it. You would get an internal compiler error: `unrecognizable insn`.

**Do this:**

Open `gcc/gcc/config/riscv/riscv.md`. Find `(define_c_enum "unspec" [` near the top and add `UNSPEC_ATTN` inside the list. Then scroll to the very end of the file and add the entire `(define_insn "riscv_attn" ...)` block there.

---

## 16. Building the Toolchain — Every Command Explained

### Installing build dependencies

```
sudo apt-get install -y autoconf automake autotools-dev curl python3 libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev
```

| Package | Why we need it |
|---------|---------------|
| `autoconf`, `automake`, `autotools-dev` | GNU build system tools — generates `configure` scripts and `Makefile`s |
| `curl` | Downloads files (used by GCC's `download_prerequisites` script) |
| `python3` | GCC's build system uses Python scripts |
| `libmpc-dev`, `libmpfr-dev`, `libgmp-dev` | Math libraries GCC depends on for constant folding and floating-point arithmetic during compilation |
| `gawk` | GNU Awk — a text processing tool required by GCC's build scripts |
| `build-essential` | Meta-package: installs `gcc`, `g++`, `make`, `libc-dev` — the host compiler |
| `bison`, `flex` | Parser and lexer generators — GCC uses these to build its C/C++ parser |
| `texinfo` | Documentation formatting system (required even if you do not build docs) |
| `gperf` | Perfect hash function generator — used by GCC for keyword lookup |
| `libtool` | Shared library management tool |
| `patchutils`, `bc` | Utility tools used by build scripts |
| `zlib1g-dev` | Compression library — GCC uses it for compressed debug info |
| `libexpat-dev` | XML parsing library — used by GDB (the debugger, built alongside) |

**`sudo`** — runs the command as root (administrator). `apt-get install` requires root because it modifies system files.

**`-y`** — automatically answers "yes" to all prompts.

### Cloning the repository

```
git clone https://github.com/camelttheoot-png/riscv-gnu-toolchain
```

**`git clone`** — downloads the entire repository from GitHub to your local machine. This creates a folder called `riscv-gnu-toolchain` with all the files.

### Initializing submodules

```
git submodule update --init binutils
git submodule update --init gcc
```

**What are submodules?** The riscv-gnu-toolchain repository does not contain the actual source code of binutils and GCC. Instead, it contains **pointers** (like bookmarks) to specific versions of those projects. Submodules are Git's way of embedding one repository inside another.

**`git submodule update --init`** — downloads the actual source code for the specified submodule. Without this, the `binutils/` and `gcc/` directories would be empty.

### Setting up paths

```
export PREFIX=$HOME/riscv_custom
export PATH=$PREFIX/bin:$PATH
```

**`export`** — sets an environment variable that child processes can see.

**`PREFIX`** — the directory where the built tools will be installed. `$HOME` expands to your home directory (e.g., `/home/username`).

**`PATH`** — the system's list of directories where it looks for executable programs. By prepending `$PREFIX/bin:`, we ensure that when you type `riscv64-unknown-elf-gcc`, the system finds our custom-built version first.

### Building binutils

```
cd ~/dc/riscv-gnu-toolchain
mkdir -p build_binutils && cd build_binutils
```

**`mkdir -p`** — creates a directory. `-p` means "no error if it already exists, and create parent directories if needed."

We build in a **separate directory** (not inside the source tree). This is called an "out-of-tree build." It keeps the source code clean and lets you rebuild without cluttering source files.

```
../binutils/configure --target=riscv64-unknown-elf --prefix=$PREFIX --disable-werror
```

**`configure`** — a script that checks your system and generates `Makefile`s tailored to it. It is part of the GNU Autotools build system.

| Flag | Meaning |
|------|---------|
| `--target=riscv64-unknown-elf` | We are building tools that produce RISC-V 64-bit code (cross-compilation) |
| `--prefix=$PREFIX` | Install the built tools into `$PREFIX` (e.g., `~/riscv_custom`) |
| `--disable-werror` | Do not treat compiler warnings as errors (prevents build failures from harmless warnings) |

```
make -j$(nproc)
```

**`make`** — reads the `Makefile` generated by `configure` and compiles the source code.

**`-j$(nproc)`** — run this many parallel compile jobs. `$(nproc)` is a command that returns the number of CPU cores. If you have 8 cores, this runs 8 compilations simultaneously, making the build much faster.

```
make install
```

This copies the compiled binaries into `$PREFIX/bin/`. After this, you have:
- `$PREFIX/bin/riscv64-unknown-elf-as` (assembler)
- `$PREFIX/bin/riscv64-unknown-elf-objdump` (disassembler)
- `$PREFIX/bin/riscv64-unknown-elf-ld` (linker)
- and more

### Building GCC

```
mkdir -p build_gcc && cd build_gcc
../gcc/configure --target=riscv64-unknown-elf --prefix=$PREFIX --disable-shared --disable-threads --disable-multilib --disable-libatomic --disable-libmudflap --disable-libssp --disable-libquadmath --disable-libgomp --disable-nls --disable-bootstrap --enable-languages=c --with-arch=rv64imac --with-abi=lp64 --with-newlib
```

| Flag | Meaning |
|------|---------|
| `--target=riscv64-unknown-elf` | Cross-compiler targeting RISC-V 64-bit |
| `--prefix=$PREFIX` | Install location |
| `--disable-shared` | Do not build shared libraries (simpler for embedded/bare-metal) |
| `--disable-threads` | No threading support (bare-metal target has no OS) |
| `--disable-multilib` | Only build for one architecture variant |
| `--disable-libatomic` | Skip the atomic operations library |
| `--disable-libmudflap` | Skip the pointer debugging library |
| `--disable-libssp` | Skip stack smashing protector library |
| `--disable-libquadmath` | Skip 128-bit floating point library |
| `--disable-libgomp` | Skip OpenMP library |
| `--disable-nls` | No native language support (English only, saves build time) |
| `--disable-bootstrap` | Do not do a 3-stage bootstrap (much faster, fine for cross-compilers) |
| `--enable-languages=c` | Only build the C compiler (not C++, Fortran, etc.) |
| `--with-arch=rv64imac` | Target architecture: RV64 with Integer, Multiply, Atomic, Compressed |
| `--with-abi=lp64` | ABI: Long and Pointer are 64-bit (standard for RV64 without float) |
| `--with-newlib` | Use the Newlib C library (a minimal C library for embedded systems) |

```
make all-gcc -j$(nproc)
make install-gcc
```

**`all-gcc`** — this Make target builds only the compiler (`gcc`) itself, not the full runtime library. This is "stage 1" of the build. It is sufficient for our purposes because we only need to compile C to assembly/object files (`-c` flag).

A full `make` would also build `libgcc`, `libstdc++`, etc., which requires a working C library (`newlib`). We skip that for speed.

---

## 17. The Demo C Program — Line by Line

```c
__attribute__((noinline))
unsigned long run_attention(unsigned long dims_addr,
                            unsigned long qkv_addr)
{
    return __builtin_riscv_attn(dims_addr, qkv_addr);
}
```

**`__attribute__((noinline))`**

This is a GCC extension that tells the compiler: "Do NOT inline this function." Normally, if the compiler sees a small function, it might copy its body into the caller (inlining) to avoid the overhead of a function call. We prevent this because we want `run_attention` to appear as a **separate function** in the objdump output, making the `attn` instruction easy to find.

**`unsigned long`**

On RV64, `unsigned long` is 64 bits — the same size as a pointer. We use it to carry memory addresses. This maps to the GCC type code `DI` (Double Integer).

**`__builtin_riscv_attn(dims_addr, qkv_addr)`**

This is the compiler builtin we registered. When GCC encounters this:
1. It looks up `attn` in its builtin table (file 4: riscv-builtins.cc)
2. It finds the RTL pattern `"riscv_attn"` (file 5: riscv.md)
3. It emits: `attn <rd>, <rs1>, <rs2>` where the registers are chosen by GCC's register allocator

The compiler does all this at compile time. There is no function call at runtime. The builtin compiles to a single machine instruction.

```c
int main(void)
{
    float Q[4] = {0.10f, 0.20f, 0.30f, 0.40f};
    float K[4] = {0.50f, 0.60f, 0.70f, 0.80f};
    float V[4] = {0.90f, 1.00f, 1.10f, 1.20f};
```

These create small matrices on the stack. In a real application, these would be much larger.

```c
    typedef struct { int rows; int cols; int seq_len; int d_model; } attn_dims_t;
    typedef struct { float *Q; float *K; float *V; } attn_qkv_t;

    attn_dims_t dims = {2, 2, 4, 8};
    attn_qkv_t  qkv  = {Q, K, V};
```

We define and fill the two structs. The hardware accelerator would read these from memory.

```c
    volatile unsigned long result = run_attention(
        (unsigned long)&dims,
        (unsigned long)&qkv
    );
```

**`(unsigned long)&dims`** — takes the address of `dims` (`&dims`) and casts it to `unsigned long`. This is how we pass a pointer as an integer register value.

**`volatile`** — tells the compiler: "Do not optimize away this variable." Without `volatile`, the compiler might see that `result` is never used and remove the entire function call, which would remove our `attn` instruction from the output.

```c
    (void)result;
    return 0;
}
```

**`(void)result`** — suppresses the "unused variable" compiler warning.

---

## 18. Compiling and Verifying — Every Command Explained

### Compiling

```
riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 -ffreestanding -nostdinc -c main.c -o main.o
```

| Flag | Meaning |
|------|---------|
| `riscv64-unknown-elf-gcc` | Our custom-built cross-compiler |
| `-O2` | Optimization level 2. Makes the output cleaner and more realistic. Without optimization, GCC would insert many unnecessary loads and stores. |
| `-march=rv64imac` | Target the RV64IMAC ISA (Integer + Multiply + Atomic + Compressed) |
| `-mabi=lp64` | Use the LP64 ABI (Long and Pointer are 64-bit, no float in registers) |
| `-ffreestanding` | This is a freestanding environment — no standard library is assumed. No `printf`, no `malloc`. We use this because we did not build the full C runtime. |
| `-nostdinc` | Do not search standard include directories. We did not install headers for the target. |
| `-c` | Compile only — produce an object file (`.o`), do not link into an executable. Linking would fail because we have no runtime library. |
| `main.c` | Input: our C source file |
| `-o main.o` | Output: the object file |

### Disassembling

```
riscv64-unknown-elf-objdump -d main.o
```

| Flag | Meaning |
|------|---------|
| `riscv64-unknown-elf-objdump` | Our custom-built disassembler |
| `-d` | Disassemble — convert machine code back to assembly text |
| `main.o` | The object file to disassemble |

---

## 19. Reading the objdump Output

```
0000000000000000 <run_attention>:
   0:   00b5050b    attn    a0,a0,a1
   4:   8082        ret
```

Let us read every part:

**`0000000000000000 <run_attention>:`**

This is the function label. `0000000000000000` is the address (offset within the `.text` section). In an object file (not yet linked), functions start at offset 0. `<run_attention>` is the symbol name.

**`0:   00b5050b    attn    a0,a0,a1`**

| Part | Meaning |
|------|---------|
| `0:` | Offset 0 (this is the first instruction in the function) |
| `00b5050b` | The raw 32-bit machine code in hexadecimal. This is what the CPU actually reads. |
| `attn` | The mnemonic — our custom instruction name! The disassembler recognized it. |
| `a0,a0,a1` | The operands: rd=a0, rs1=a0, rs2=a1 |

Let us verify `00b5050b` manually:

```
Hex:    0   0   b   5   0   5   0   b
Binary: 0000 0000 1011 0101 0000 0101 0000 1011

Bit fields:
[31:25] funct7 = 0000000 = 0    ✓ (matches our encoding)
[24:20] rs2    = 01011   = 11   = a1  ✓
[19:15] rs1    = 01010   = 10   = a0  ✓
[14:12] funct3 = 000     = 0    ✓ (matches our encoding)
[11:7]  rd     = 01010   = 10   = a0  ✓
[6:0]   opcode = 0001011 = 0x0B = custom-0  ✓
```

Every bit matches our design.

**`4:   8082        ret`**

Offset 4 (4 bytes after the first instruction). `8082` is only 16 bits — this is a **compressed instruction** (from the C extension). `ret` is a pseudo-instruction that means "return from function" (it expands to `jalr x0, x1, 0`).

---

## 20. The Automation Script — How It Works

The file `custom_attn/automate_instruction.py` automates the entire process of adding a new custom instruction.

### What it does, step by step:

**Step 1 — Find a free opcode slot**

The script reads `riscv-opc.h` and extracts every `#define MATCH_*` value. It then iterates through all possible (base_opcode, funct3, funct7) combinations for custom-0 through custom-3, and finds the first one that is not already used.

For R-type (2 inputs): iterates funct3 (0-7) × funct7 (0-127) = 1,024 slots per base.
For R4-type (3 inputs): iterates funct3 (0-7) × funct2 (0-3) = 32 slots per base.

**Step 2 — Modify all 5 source files**

For each file, the script:
1. Reads the current file contents
2. Checks if the instruction is already present (idempotency)
3. Finds the correct anchor point (using regex to handle whitespace variations)
4. Inserts the new code at the right position
5. Writes the file back

The script is idempotent — running it twice with the same instruction name does not create duplicates.

**Step 3 — Generate demo C code**

Creates `demo/main_<name>.c` with the wrapper pattern:

```
wrapper_<name>() → calls __builtin_riscv_<name>() → emits the instruction
```

The wrapper is kept `noinline` so it shows up clearly in objdump.

**Step 4 — Rebuild toolchain**

Runs `make` in the existing build directories (incremental rebuild). Since only a few files changed, this is much faster than a full build — usually under a minute.

**Step 5 — Compile demo and run objdump**

Compiles the generated C file and checks if the instruction mnemonic appears in the disassembly output.

### Key functions in the script:

| Function | What it does |
|----------|-------------|
| `parse_existing_match_values()` | Reads riscv-opc.h, extracts all MATCH_* hex values into a set |
| `compute_match_mask()` | Calculates MATCH and MASK from (base_opcode, funct3, funct7/funct2) |
| `find_free_opcode()` | Scans all 4 custom slots to find the first unused combination |
| `modify_riscv_opc_h()` | Inserts #define MATCH/MASK and DECLARE_INSN into riscv-opc.h |
| `modify_riscv_opc_c()` | Adds the opcode table entry to riscv-opc.c |
| `modify_riscv_ftypes_def()` | Adds the function type to riscv-ftypes.def |
| `modify_riscv_builtins_cc()` | Adds RISCV_ATYPE_DI, AVAIL, and DIRECT_BUILTIN to riscv-builtins.cc |
| `modify_riscv_md()` | Adds UNSPEC and define_insn pattern to riscv.md |
| `generate_demo_c()` | Creates the demo C file with wrapper function |
| `rebuild_binutils()` | Runs make in the binutils build directory |
| `rebuild_gcc()` | Runs make all-gcc in the GCC build directory |
| `compile_and_dump()` | Compiles demo.c and runs objdump |

---

## 21. R4-Type — 3-Input Instructions

If your custom instruction needs 3 source registers (for example, a fused multiply-add), you use the **R4-type** format:

```
31    27 26 25 24    20 19    15 14  12 11     7 6      0
┌────────┬─────┬──────┬────────┬──────┬────────┬────────┐
│  rs3   │ f2  │ rs2  │  rs1   │funct3│   rd   │ opcode │
└────────┴─────┴──────┴────────┴──────┴────────┴────────┘
```

The differences from R-type:
- funct7 (7 bits) is split into rs3 (5 bits) + funct2 (2 bits)
- MASK changes to `0x0600707F` (only checks opcode + funct3 + funct2, not the rs3 field)
- Operand format string becomes `"d,s,t,r"` where `r` = integer rs3

In the automation script, use `--inputs 3`:

```
python3 automate_instruction.py --name fused_mac --inputs 3
```

This generates:
- Operand format: `"d,s,t,r"` (four integer registers)
- Function type: `DEF_RISCV_FTYPE (3, (DI, DI, DI, DI))` → `RISCV_DI_FTYPE_DI_DI_DI`
- Machine description with 3 input match_operands
- Assembly output: `fused_mac %0,%1,%2,%3`

---

## 22. Viva Questions and Answers

### Basic RISC-V Questions

**Q: What does RISC-V stand for?**
A: RISC-V is the fifth generation of processors built on the Reduced Instruction Set Computer (RISC) principles, developed at UC Berkeley. The "V" is the Roman numeral 5.

**Q: How wide is a RISC-V instruction?**
A: Standard instructions are 32 bits (4 bytes). With the C (Compressed) extension, some common instructions can be 16 bits. Our custom instruction is 32 bits.

**Q: How many registers does RISC-V have?**
A: 32 integer registers (x0-x31) and optionally 32 floating-point registers (f0-f31). x0 is hardwired to zero.

**Q: What is the difference between RV32 and RV64?**
A: RV32 has 32-bit registers and addresses. RV64 has 64-bit registers and addresses. We use RV64 because pointers need to be 64 bits to address large memory spaces.

### Custom Instruction Questions

**Q: Where does the custom instruction's opcode come from?**
A: RISC-V reserves four opcode slots for custom instructions: custom-0 (0x0B), custom-1 (0x2B), custom-2 (0x5B), custom-3 (0x7B). We used custom-0. These slots are guaranteed to never be used by the official specification.

**Q: How did you verify the opcode slot was free?**
A: We cloned the official `riscv-opcodes` repository and extracted all opcodes used by existing extensions using `grep`. Then we generated all 32 possible 5-bit opcode values and used `comm -23` to find values present in the full list but absent from the used list. We cross-checked against binutils source (`tc-riscv.c`) to make sure no vendor extension was using it either. Slot `0x02` (= custom-0 = `0x0B`) was confirmed free in both sources.

**Q: How do you compute MATCH and MASK from the opcode?**
A: MATCH is the exact bit pattern: `opcode | (funct3 << 12) | (funct7 << 25)`. For custom-0 with funct3=0 and funct7=0, that is `0x0B | 0 | 0 = 0x0000000B`. MASK is `0xFE00707F` for all R-type instructions — it has 1s in the opcode, funct3, and funct7 fields (the bits that identify the instruction) and 0s in the register fields (which change per usage).

**Q: What format is your instruction?**
A: R-type (Register-Register). It has three register operands: rd (destination), rs1 (source 1), rs2 (source 2), plus funct3 and funct7 fields to identify the specific instruction.

**Q: What do MATCH and MASK mean?**
A: MASK tells us which bits of the 32-bit instruction identify it (opcode + funct3 + funct7 = 0xFE00707F). MATCH is what those bits should equal (0x0000000B). The check is: `(instruction & MASK) == MATCH`.

**Q: Why did you use integer registers instead of floating-point registers?**
A: Because our operands are memory addresses (pointers to structs), not floating-point values. Addresses are integers. The actual matrix data lives in memory, accessed through these pointers.

**Q: Why R-type and not R4-type?**
A: We have only 2 inputs (address of dimensions struct + address of Q/K/V struct). R-type supports 2 source registers. R4-type is for 3 source registers.

**Q: What is INSN_CLASS_I?**
A: It classifies the instruction as belonging to the base Integer ISA. This means it does not require any extensions (like F for floating-point) to be available. Since we use integer registers, INSN_CLASS_I is correct.

**Q: What does "d,s,t" mean in the opcode table?**
A: These are operand format characters. `d` = integer rd (destination register, bits [11:7]), `s` = integer rs1 (source 1, bits [19:15]), `t` = integer rs2 (source 2, bits [24:20]).

### Compiler Questions

**Q: What is a compiler builtin?**
A: A function recognized specially by the compiler. Instead of generating a function call, the compiler emits specific machine instructions directly. `__builtin_riscv_attn()` compiles to a single `attn` instruction.

**Q: Why not use inline assembly?**
A: The assignment specifically prohibits inline assembly. Using a compiler builtin is cleaner — the compiler handles register allocation, instruction scheduling, and optimization. Inline assembly bypasses all of that.

**Q: What is UNSPEC in GCC?**
A: A way to tell GCC "this operation is special, do not try to optimize or simplify it." Without UNSPEC, GCC might eliminate our instruction because it cannot understand what it does.

**Q: What does the machine description (.md) file do?**
A: It maps high-level operations (builtins) to assembly output. When GCC encounters `__builtin_riscv_attn()`, it looks up the pattern `"riscv_attn"` in riscv.md to know what assembly text to emit.

**Q: What does DEF_RISCV_FTYPE do?**
A: It defines a function type signature that builtins can use. `DEF_RISCV_FTYPE(2, (DI, DI, DI))` creates the type "returns DI, takes two DI arguments" named `RISCV_DI_FTYPE_DI_DI`.

**Q: What does AVAIL(always_enabled, (!0)) mean?**
A: It defines an availability predicate. `(!0)` evaluates to `true`, meaning the builtin is always available regardless of which ISA extensions are enabled.

### Toolchain Questions

**Q: What files did you modify and why?**
A: Six files across three components:
1. `riscv-opc.h` — Register the binary encoding (MATCH/MASK)
2. `riscv-opc.c` — Teach the assembler/disassembler the mnemonic
3. `riscv-ftypes.def` — Define the C function type signature
4. `riscv-builtins.cc` — Register the `__builtin_riscv_attn()` function
5. `riscv.md` — Tell GCC how to emit the assembly instruction
6. `riscv-opcodes/extensions/rv_custom` — Register the opcode in riscv-opcodes format

Files 1-2 are in binutils (assembler/disassembler). Files 3-5 are in GCC (compiler). File 6 is in the riscv-opcodes registry so the automation script tracks which opcode slots are in use.

**Q: Why do you need both binutils AND GCC changes?**
A: Binutils handles assembly → binary (assembler) and binary → assembly (disassembler). GCC handles C → assembly (compiler). Without binutils changes, the assembler would not recognize `attn`. Without GCC changes, the compiler would not know about `__builtin_riscv_attn()`.

**Q: What is a cross-compiler?**
A: A compiler that runs on one architecture (x86, your laptop) but produces code for a different architecture (RISC-V). We cannot run the output on our laptop — we would need a RISC-V processor or emulator.

**Q: What does `make all-gcc` do vs `make`?**
A: `make all-gcc` builds only the compiler binary itself. `make` would build the compiler plus all runtime libraries (libgcc, libstdc++, etc.). We only need the compiler for our `-c` (compile-only) workflow.

**Q: What does `-ffreestanding` mean?**
A: It tells the compiler this is a freestanding environment — no operating system, no standard library. Only language features that do not require OS support are available. We use this because we built only stage 1 GCC without a C library.

### Attention Mechanism Questions

**Q: What is the attention mechanism?**
A: The core computation in Transformer models: `Attention(Q,K,V) = softmax(Q × Kᵀ / √dₖ) × V`. It computes how much each token in a sequence should "pay attention to" every other token.

**Q: What are Q, K, and V?**
A: Query, Key, and Value matrices. They are linear projections of the input. Q asks "what am I looking for?", K answers "what do I contain?", V provides "what information do I give?". The dot product Q×Kᵀ measures similarity, softmax normalizes it to probabilities, and multiplying by V produces the weighted combination.

**Q: Why divide by √dₖ?**
A: Without scaling, the dot products grow large as the dimension increases (because you are summing more terms). Large values push the softmax into regions where gradients are very small. Dividing by √dₖ keeps the variance stable. This is called "scaled dot-product attention."

**Q: Can your instruction actually compute attention?**
A: The instruction encoding and toolchain support are real. The actual computation would require a hardware accelerator that reads the struct pointers, fetches the matrices from memory, and performs the matrix operations. Our project proves the toolchain can handle the instruction; the hardware is a separate implementation.

---

## Quick Reference Card

```
Instruction:     attn rd, rs1, rs2
Opcode slot:     custom-0 (0x0B)
Format:          R-type
MATCH:           0x0000000B
MASK:            0xFE00707F
funct3:          0x0
funct7:          0x00
GCC builtin:     __builtin_riscv_attn(unsigned long, unsigned long)
Operand format:  "d,s,t"
Insn class:      INSN_CLASS_I
GCC type:        RISCV_DI_FTYPE_DI_DI
UNSPEC:          UNSPEC_ATTN
RTL pattern:     "riscv_attn"
Target:          riscv64-unknown-elf
Architecture:    rv64imac / lp64
```
