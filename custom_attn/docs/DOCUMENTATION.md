# RISC-V Custom Instruction — Complete Documentation
### Everything You Need to Know (Group 9 — Attention Mechanism)

This document is a sequential guide. Read it top to bottom. Every concept is explained when it first appears, so you should never need to scroll back.

---

## Table of Contents

**Part 1 — Foundations**
1. [What is RISC-V?](#1-what-is-risc-v)
2. [What is a Custom Instruction?](#2-what-is-a-custom-instruction)
3. [What is the GNU Toolchain?](#3-what-is-the-gnu-toolchain)
4. [How a C Program Becomes Machine Code](#4-how-a-c-program-becomes-machine-code)
5. [Understanding Opcodes — The Language of the CPU](#5-understanding-opcodes--the-language-of-the-cpu)
6. [RISC-V Instruction Formats](#6-risc-v-instruction-formats)

**Part 2 — Designing Our Instruction**
7. [The custom-0 Through custom-3 Opcode Slots](#7-the-custom-0-through-custom-3-opcode-slots)
8. [How to Find a Free Opcode — Step by Step](#8-how-to-find-a-free-opcode--step-by-step)
9. [Our Instruction: attn](#9-our-instruction-attn)

**Part 3 — Layer 1: Teaching the Toolchain (Files 1–6)**
10. [File 1 — riscv-opc.h (The Encoding Registry)](#10-file-1--riscv-opch-the-encoding-registry)
11. [File 2 — riscv-opc.c (The Assembler's Lookup Table)](#11-file-2--riscv-opcc-the-assemblers-lookup-table)
12. [File 3 — riscv-ftypes.def (Function Type Signature)](#12-file-3--riscv-ftypesdef-function-type-signature)
13. [File 4 — riscv-builtins.cc (Builtin Registration)](#13-file-4--riscv-builtinscc-builtin-registration)
14. [File 5 — riscv.md (Machine Description)](#14-file-5--riscvmd-machine-description)
15. [File 6 — rv_custom (Opcode Registry)](#15-file-6--rv_custom-opcode-registry)

**Part 4 — Building and Verifying the Builtin**
16. [Building the Toolchain — Every Command Explained](#16-building-the-toolchain--every-command-explained)
17. [The Demo C Program — Line by Line](#17-the-demo-c-program--line-by-line)
18. [Compiling and Verifying — Every Command Explained](#18-compiling-and-verifying--every-command-explained)
19. [Reading the objdump Output](#19-reading-the-objdump-output)

**Part 5 — Layer 2: Automatic Detection (GIMPLE Pass)**
20. [What is GIMPLE?](#20-what-is-gimple)
21. [GCC's Optimization Pipeline](#21-gccs-optimization-pipeline)
22. [What is a Compiler Pass?](#22-what-is-a-compiler-pass)
23. [Our GIMPLE Pass — The Big Picture](#23-our-gimple-pass--the-big-picture)
24. [Detection Phase — How the Pass Reads Loops](#24-detection-phase--how-the-pass-reads-loops)
25. [Replacement Phase — How the Pass Emits the Instruction](#25-replacement-phase--how-the-pass-emits-the-instruction)
26. [Integrating the Pass into GCC](#26-integrating-the-pass-into-gcc)
27. [Verifying Auto-Detection](#27-verifying-auto-detection)
28. [The 11 Fixes — What Went Wrong and Why](#28-the-11-fixes--what-went-wrong-and-why)

**Part 6 — Extras**
29. [R4-Type — 3-Input Instructions](#29-r4-type--3-input-instructions)
30. [The Automation Script — How It Works](#30-the-automation-script--how-it-works)
31. [Quick Reference Card](#31-quick-reference-card)

---

# Part 1 — Foundations

## 1. What is RISC-V?

RISC-V (pronounced "risk-five") is an **open-source instruction set architecture** (ISA). An ISA is the contract between software and hardware — it defines what instructions a CPU understands.

Unlike x86 (owned by Intel/AMD) or ARM (owned by ARM Holdings), RISC-V is **free and open**. Anyone can design a RISC-V CPU. This is why universities and research labs use it — you can modify it without paying licensing fees.

**RISC** stands for **Reduced Instruction Set Computer**. This means:
- Each instruction does one simple thing
- All instructions are the same size (32 bits in RV32/RV64)
- There are few instruction formats (R, I, S, B, U, J)
- The CPU hardware is simpler and faster

**RV64GC** — this is the specific RISC-V configuration we target:
- **RV64** = 64-bit base integer ISA (registers are 64 bits wide)
- **G** = General-purpose (shorthand for IMAFD — Integer, Multiply, Atomic, Float, Double)
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

In a real chip, this instruction would trigger a hardware accelerator. In our project, we are proving that the **toolchain** (compiler + assembler + disassembler) can handle the instruction — and that the compiler can **automatically detect** the attention pattern in plain C code and replace it with the hardware instruction. The hardware implementation would be a separate project.

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

Here is the complete pipeline. Understanding this is critical because every file we modify is part of this chain:

```
main.c (C source code)
    │
    │  GCC compiler (what we modify in Part 3, files 3-5)
    │  1. Preprocesses: expands #includes and macros
    │  2. Parses: builds an Abstract Syntax Tree (AST)
    │  3. Converts to GIMPLE: GCC's internal representation (Part 5)
    │  4. Optimizes: runs optimization passes (our GIMPLE pass lives here)
    │  5. Converts to RTL: Register Transfer Language
    │  6. Emits assembly: "attn a0,a0,a1"
    │
    ▼
main.s (assembly source)
    │
    │  GAS assembler (what we modify in Part 3, files 1-2)
    │  Reads the opcode table (riscv-opc.c)
    │  Finds "attn" → MATCH_ATTN = 0x0200000b
    │  Encodes registers into the 32-bit instruction
    │
    ▼
main.o (object file — raw machine code)
    │
    │  objdump (uses our modifications to files 1-2)
    │  Reads the binary, matches against MATCH/MASK
    │  Recognizes 0x02b5050b as "attn a0,a0,a1"
    │
    ▼
Disassembly output (what we see in the terminal)
```

Notice the two separate flows:
- **Encoding path** (C → assembly → binary): needs GCC (files 3-5) + GAS (files 1-2)
- **Decoding path** (binary → readable text): needs objdump (files 1-2)

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

RISC-V defines 6 standard instruction formats. We use **R-type** for our 2-input instruction.

### R-type (Register-Register) — Used for `attn`

```
31       25 24    20 19    15 14  12 11     7 6      0
┌──────────┬────────┬────────┬──────┬────────┬────────┐
│  funct7  │  rs2   │  rs1   │funct3│   rd   │ opcode │
└──────────┴────────┴────────┴──────┴────────┴────────┘
   7 bits    5 bits   5 bits  3 bits  5 bits   7 bits  = 32 bits
```

R-type is used when an instruction takes two source registers and produces one result. Examples: `add`, `sub`, `and`, `or`, and our `attn`.

---

# Part 2 — Designing Our Instruction

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

Our `attn` instruction uses: custom-0, funct3=0, funct7=1 (0x01). We use funct7=1 (not 0) because funct7=0 was already taken by an earlier test instruction.

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

What each part does:

| Part | What it does |
|------|-------------|
| `grep -o "6..2=0x[0-9A-Fa-f]*" -R extensions/` | Searches all files for the pattern `6..2=0xNN` (the riscv-opcodes format for opcode bits). `-R` = recursive, `-o` = only print matching part. |
| `sed 's/.*=0x//'` | Strips everything before the hex value. `sed` is a stream editor. |
| `awk '{printf "0x%02x\n", strtonum("0x"$1)}'` | Formats each value as proper hex like `0x02`. |
| `sort -u` | Sorts and removes duplicates. |
| `> used_clean.txt` | Saves to file. |

### Step 8.3 — Generate all possible opcode values

```
for i in $(seq 0 31); do printf "0x%02x\n" $i; done > all_clean.txt
```

Bits [6:2] are 5 bits, so there are 2⁵ = 32 possible values. This generates all of them.

### Step 8.4 — Find which opcodes are free

```
comm -23 all_clean.txt used_clean.txt > free_from_extensions.txt
cat free_from_extensions.txt
```

`comm` compares two sorted files. `-23` shows lines only in the first file — opcodes that are NOT used.

### Step 8.5 — Computing MATCH and MASK

Now we have the opcode (`0x0B` = custom-0). We choose funct3=0 and funct7=1. These three values uniquely identify our instruction.

**MATCH** is a 32-bit number with opcode, funct3, and funct7 in their correct bit positions, and all register fields set to zero:

```
MATCH = opcode | (funct3 << 12) | (funct7 << 25)
      = 0x0B   | (0 << 12)      | (1 << 25)
      = 0x0B   | 0x00000000     | 0x02000000
      = 0x0200000B
```

What does `<< 25` mean? It shifts bits left by 25 positions. funct7 occupies bits [31:25], so its value needs to land at bit position 25.

**MASK** tells the CPU which bits identify this instruction (1 = matters, 0 = ignore):

```
funct7 [31:25] = 1111111  (these identify the instruction)
rs2    [24:20] = 00000    (these vary per usage)
rs1    [19:15] = 00000    (these vary per usage)
funct3 [14:12] = 111      (these identify the instruction)
rd     [11:7]  = 00000    (these vary per usage)
opcode [6:0]   = 1111111  (these identify the instruction)

Concatenate: 1111111_00000_00000_111_00000_1111111
Group into hex nibbles: FE 00 70 7F
MASK = 0xFE00707F
```

**Verification:** `(instruction & MASK) == MATCH` tells the CPU "this is the `attn` instruction."

---

## 9. Our Instruction: attn

### What it represents

The Transformer attention mechanism:

```
Attention(Q, K, V) = softmax(Q × Kᵀ / √dₖ) × V
```

Where:
- **Q** (Query), **K** (Key), **V** (Value) — matrices of shape [seq_len × d_k]
- **dₖ** — dimension of key vectors (used for scaling)
- **softmax** — applied row-wise to normalize attention weights

### Why two pointer arguments?

You cannot pass three entire matrices through CPU registers. Registers are 64 bits wide — they hold one number. So we pass **memory addresses** (pointers) to structs:

- `rs1` points to a struct containing the matrix dimensions
- `rs2` points to a struct containing three pointers to Q, K, and V in memory

A hardware accelerator would read the struct, DMA the matrices from memory, and compute the attention.

### The struct definitions

```c
typedef struct {
    int rows;      // number of rows (sequence length)
    int cols;      // number of columns (sequence length)
    int seq_len;   // sequence length
    int d_model;   // model dimension (d_k)
} attn_dims_t;

typedef struct {
    float *Q;      // pointer to Query matrix
    float *K;      // pointer to Key matrix
    float *V;      // pointer to Value matrix
} attn_qkv_t;
```

---

# Part 3 — Layer 1: Teaching the Toolchain (Files 1–6)

These 6 files register the `attn` instruction encoding and the `__builtin_riscv_attn()` compiler builtin. This is automated by the script:

```bash
cd custom_attn/scripts && ./automate_instruction.sh add attn 2
```

But here we explain every file in detail so you understand what the script does.

## 10. File 1 — riscv-opc.h (The Encoding Registry)

**Full path:** `binutils/include/opcode/riscv-opc.h`

**What this file is:** A massive header file containing the binary encoding of every RISC-V instruction. Both the assembler and the disassembler include this file.

**What we added:**

```c
#define MATCH_ATTN  0x0200000b
#define MASK_ATTN   0xfe00707f
```

These define the binary identity of our instruction (section 8.5 explained the math).

```c
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)
```

This macro registers the instruction in a table. It expands differently depending on context — sometimes for the assembler, sometimes for the disassembler.

**Without these definitions**, neither the assembler nor the disassembler would know that the bit pattern `0x0200000b` corresponds to an instruction called `attn`. You would see raw hex like `0200000b` instead of the readable `attn a0,a0,a1`.

---

## 11. File 2 — riscv-opc.c (The Assembler's Lookup Table)

**Full path:** `binutils/opcodes/riscv-opc.c`

**What this file is:** The assembler's main lookup table. When you write `attn a0, a0, a1` in assembly, the assembler looks up "attn" in this table to find out how to encode it.

**What we added:**

```c
{"attn", 0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0},
```

Every field explained:

| Field | Value | Meaning |
|-------|-------|---------|
| `"attn"` | — | The mnemonic text |
| `0` | — | Instruction length override. 0 = default (32 bits) |
| `INSN_CLASS_I` | — | Instruction class. `I` = base integer ISA. Does not require float hardware. |
| `"d,s,t"` | — | **Operand format.** `d` = rd (destination, bits [11:7]), `s` = rs1 (source 1, bits [19:15]), `t` = rs2 (source 2, bits [24:20]) |
| `MATCH_ATTN` | `0x0200000b` | The MATCH value from file 1 |
| `MASK_ATTN` | `0xfe00707f` | The MASK value from file 1 |
| `match_opcode` | — | Function that checks `(insn & MASK) == MATCH` |
| `0` | — | No special flags |

**Without this entry**, writing `attn` in assembly would give: `Error: unrecognized opcode 'attn'`.

---

## 12. File 3 — riscv-ftypes.def (Function Type Signature)

**Full path:** `gcc/gcc/config/riscv/riscv-ftypes.def`

**What this file is:** Defines C function type signatures for GCC builtins. GCC needs to know argument and return types for every builtin.

**What we added:**

```c
DEF_RISCV_FTYPE (2, (DI, DI, DI))
```

| Part | Meaning |
|------|---------|
| `DEF_RISCV_FTYPE` | Macro that creates a function type |
| `2` | Number of arguments |
| `(DI, DI, DI)` | Type list: (return_type, arg1_type, arg2_type) |

**DI** = **Double Integer** = 64-bit integer. On RV64, this is the size of a pointer (`unsigned long`). Other type codes: `SI` = 32-bit int, `SF` = 32-bit float, `DF` = 64-bit double.

This creates a type named `RISCV_DI_FTYPE_DI_DI` — "returns 64-bit int, takes two 64-bit int arguments."

**Without this**, the builtin registration in file 4 would fail because the type would not exist.

---

## 13. File 4 — riscv-builtins.cc (Builtin Registration)

**Full path:** `gcc/gcc/config/riscv/riscv-builtins.cc`

**What this file is:** Registers all RISC-V builtin functions. A **builtin** is a function the compiler recognizes specially — instead of generating a function call, it generates specific machine instructions directly.

When you write `__builtin_riscv_attn(x, y)` in C, GCC does **not** look for a library function. It checks its builtin table, finds our entry, and emits the `attn` instruction directly. One C function call → one machine instruction. No overhead.

**What we added — three pieces:**

### Piece 1: Type mapping

```c
#define RISCV_ATYPE_DI long_integer_type_node
```

Maps our type code `DI` to GCC's internal type object. `long_integer_type_node` is GCC's representation of the `long` type.

### Piece 2: Availability predicate

```c
AVAIL (always_enabled, (!0))
```

Creates a function that returns `true` (`!0`). This means our builtin is **always available**, regardless of which ISA extensions are enabled.

### Piece 3: Builtin registration

```c
DIRECT_BUILTIN (attn, RISCV_DI_FTYPE_DI_DI, always_enabled),
```

| Part | Meaning |
|------|---------|
| `DIRECT_BUILTIN` | Maps directly to one instruction |
| `attn` | GCC creates `__builtin_riscv_attn()` from this name |
| `RISCV_DI_FTYPE_DI_DI` | Function type from file 3 |
| `always_enabled` | Availability predicate from piece 2 |

**Without this file**, calling `__builtin_riscv_attn()` would give: `error: implicit declaration of function '__builtin_riscv_attn'`.

---

## 14. File 5 — riscv.md (Machine Description)

**Full path:** `gcc/gcc/config/riscv/riscv.md`

**What this file is:** GCC's Machine Description for RISC-V, written in **RTL** (Register Transfer Language). This tells GCC how to convert high-level operations into actual assembly instructions.

Every instruction the compiler can emit has a pattern here. When GCC decides to emit `attn`, it looks for the pattern named `"riscv_attn"`.

**What we added — two pieces:**

### Piece 1: UNSPEC constant

```
UNSPEC_ATTN
```

Added inside the `(define_c_enum "unspec" [...])` block.

**What is UNSPEC?** GCC's internal representation has standard operations like `plus`, `mult`, `and`. But `attn` is completely custom — there is no standard operation for "compute attention." `UNSPEC` tells GCC: "this is a special operation — do not try to optimize, simplify, or reorder it. Just emit it as-is."

Without `UNSPEC`, GCC might remove our instruction entirely (thinking it has no effect) or try to combine it with other operations.

### Piece 2: Instruction pattern

```lisp
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

This is dense but logical. Every part:

| Part | Meaning |
|------|---------|
| `define_insn "riscv_attn"` | Defines instruction pattern named `riscv_attn` |
| `set (match_operand:DI 0 ... "=r")` | Output: 64-bit integer in a register. `=` = write-only, `r` = general register. This is `rd`. |
| `match_operand:DI 1 ... "r"` | Input 1: 64-bit integer register (read). This is `rs1`. |
| `match_operand:DI 2 ... "r"` | Input 2: 64-bit integer register (read). This is `rs2`. |
| `UNSPEC_ATTN` | Our custom operation identifier |
| `""` | Condition: empty = always valid |
| `"attn\t%0,%1,%2"` | Assembly template: `%0` = rd, `%1` = rs1, `%2` = rs2 |
| `set_attr "type" "arith"` | Tells the scheduler this is an arithmetic instruction |

So if GCC chooses registers a0, a0, a1, the output assembly is: `attn	a0,a0,a1`

**Without this pattern**, GCC would know `__builtin_riscv_attn()` exists (from file 4) but not know what assembly to emit. You would get: `internal compiler error: unrecognizable insn`.

---

## 15. File 6 — rv_custom (Opcode Registry)

**Full path:** `riscv-opcodes/extensions/rv_custom`

This registers the instruction in the riscv-opcodes format so the automation script can track which opcode slots are in use. It prevents future instructions from accidentally using the same slot.

---

# Part 4 — Building and Verifying the Builtin

## 16. Building the Toolchain — Every Command Explained

### Installing build dependencies

```bash
sudo apt-get install -y autoconf automake autotools-dev curl python3 \
  libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex \
  texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev
```

| Package | Why |
|---------|-----|
| `autoconf`, `automake` | GNU build system — generates `configure` and `Makefile` |
| `libmpc-dev`, `libmpfr-dev`, `libgmp-dev` | Math libraries GCC needs for constant folding |
| `build-essential` | Host compiler (gcc, g++, make) |
| `bison`, `flex` | Parser/lexer generators for GCC's C parser |
| `texinfo` | Documentation formatting (required even without building docs) |

### Clone, patch, and build

```bash
# Clone the repository
git clone https://github.com/Yash-Awasthi/riscv-gnu-toolchain.git
cd riscv-gnu-toolchain
git submodule update --init binutils gcc riscv-opcodes

# Apply Layer 1 patches (automated)
cd custom_attn/scripts
chmod +x automate_instruction.sh
./automate_instruction.sh add attn 2
cd ../..

# Apply Layer 2 (GIMPLE pass — see Part 5 for full explanation)
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/
# (plus build system modifications — detailed in section 26)

# Build
mkdir -p build && cd build
../configure --prefix=$HOME/riscv --with-arch=rv64gc --with-abi=lp64d
make -j$(nproc)
export PATH=$HOME/riscv/bin:$PATH
```

| Flag | Meaning |
|------|---------|
| `--prefix=$HOME/riscv` | Install built tools into `~/riscv/bin/` |
| `--with-arch=rv64gc` | Target RV64GC (Integer + Multiply + Atomic + Float + Double + Compressed) |
| `--with-abi=lp64d` | ABI: Long/Pointer = 64-bit, doubles in float registers |
| `make -j$(nproc)` | Compile with all CPU cores in parallel (much faster) |

`make` takes 30-60+ minutes. It builds both binutils and GCC.

---

## 17. The Demo C Program — Line by Line

This is `demo/mainbuiltin.c` — the explicit builtin path:

```c
long run_attention(long dims_addr, long qkv_addr)
{
    return __builtin_riscv_attn(dims_addr, qkv_addr);
}
```

**`__builtin_riscv_attn(dims_addr, qkv_addr)`** — The compiler builtin we registered. When GCC encounters this:
1. Looks up `attn` in its builtin table (file 4)
2. Finds RTL pattern `"riscv_attn"` (file 5)
3. Emits: `attn <rd>, <rs1>, <rs2>`

The builtin compiles to a **single machine instruction**. No function call overhead at runtime.

---

## 18. Compiling and Verifying — Every Command Explained

```bash
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c mainbuiltin.c -o mainbuiltin.o
```

| Flag | Meaning |
|------|---------|
| `-O2` | Optimization level 2. Makes output cleaner. |
| `-march=rv64gc` | Target ISA |
| `-ffreestanding` | No standard library assumed (bare-metal) |
| `-nostdlib` | Do not link standard libraries |
| `-c` | Compile only — produce `.o`, do not link |

```bash
riscv64-unknown-elf-objdump -d mainbuiltin.o
```

`-d` = disassemble the object file.

---

## 19. Reading the objdump Output

```
0000000000000000 <run_attention>:
   0:   02b5050b    attn    a0,a0,a1
   4:   8082        ret
```

| Part | Meaning |
|------|---------|
| `0:` | Offset 0 (first instruction) |
| `02b5050b` | Raw 32-bit machine code in hex |
| `attn` | Our custom instruction — the disassembler recognized it! |
| `a0,a0,a1` | rd=a0, rs1=a0, rs2=a1 |

Let us manually verify `02b5050b`:

```
Binary: 0000_0010_1011_0101_0000_0101_0000_1011

[31:25] funct7 = 0000001 = 1 = 0x01     ✓ (our funct7)
[24:20] rs2    = 01011   = 11 = a1      ✓
[19:15] rs1    = 01010   = 10 = a0      ✓
[14:12] funct3 = 000     = 0            ✓
[11:7]  rd     = 01010   = 10 = a0      ✓
[6:0]   opcode = 0001011 = 0x0B         ✓ (custom-0)
```

Every bit matches. The toolchain works for the explicit builtin path.

**`4: 8082  ret`** — This is only 16 bits (a **compressed instruction** from the C extension). `ret` means "return from function."

---

# Part 5 — Layer 2: Automatic Detection (GIMPLE Pass)

This is the advanced part. Everything above taught the toolchain to recognize `attn` when you explicitly ask for it. Now we teach the **compiler** to automatically detect the attention pattern in plain C loops and replace them with `attn` — no builtin call needed.

## 20. What is GIMPLE?

When you write C code, GCC does not compile it directly to assembly. It first converts your code into an **intermediate representation** called **GIMPLE**.

GIMPLE is a simplified form of your code where:
- Every complex expression is broken into simple 3-address statements
- All control flow is explicit (no hidden short-circuits)
- Loops and branches use basic blocks and edges

### Example: C to GIMPLE

Your C code:
```c
scores[i*n + j] += Q[i*d + k] * K[j*d + k];
```

Becomes GIMPLE (simplified):
```
_1 = i * d;
_2 = _1 + k;
_3 = Q[_2];          // load from Q
_4 = j * d;
_5 = _4 + k;
_6 = K[_5];          // load from K
_7 = _3 * _6;        // multiply
_8 = i * n;
_9 = _8 + j;
_10 = scores[_9];    // load current value
_11 = _10 + _7;      // accumulate
scores[_9] = _11;    // store back
```

Every line is one simple operation. Variables like `_1`, `_2` are **SSA names** (Static Single Assignment) — each variable is assigned exactly once. This makes it easy for the compiler (and our pass) to trace where values come from.

**Why does this matter?** Our GIMPLE pass reads this intermediate form. It does not read your original C code. It looks at the GIMPLE statements inside loops and recognizes patterns like "multiply two array elements and accumulate" = matrix multiplication.

---

## 21. GCC's Optimization Pipeline

GCC processes your code through a series of stages. Here is the simplified pipeline:

```
C source code
    │
    ▼
Parsing (builds AST — Abstract Syntax Tree)
    │
    ▼
Gimplification (AST → GIMPLE)
    │
    ▼
SSA Construction (variables → SSA names)
    │
    ▼
Early Optimization Passes
    │  (constant propagation, dead code elimination, inlining)
    │
    ▼
Loop Optimization Passes     ◄── Our pass runs RIGHT AFTER this
    │  (loop unrolling, vectorization, etc.)
    │
    ▼
███ riscv_attn_detect ███    ◄── OUR GIMPLE PASS
    │  (detects attention pattern, replaces with attn instruction)
    │
    ▼
Late Optimization Passes
    │
    ▼
RTL Generation (GIMPLE → Register Transfer Language)
    │
    ▼
Register Allocation
    │
    ▼
Assembly Output
```

Our pass runs after loop optimization because by that point:
1. Loops have been normalized (canonical form with clear headers and exits)
2. Memory accesses have been analyzed
3. The loop tree structure is available for inspection

**Important:** The pass only runs at `-O2` or higher. At `-O0` (no optimization), loop optimization is skipped, so our pass never sees the loops.

---

## 22. What is a Compiler Pass?

A **compiler pass** is a function that walks through your code's intermediate representation (GIMPLE), analyzes it, and optionally transforms it.

GCC has hundreds of built-in passes: dead code elimination, constant propagation, loop unrolling, vectorization, etc. Each pass does one specific thing.

Our pass (`riscv_attn_detect`) is a custom pass we wrote and registered into GCC's pipeline. It is a **GIMPLE optimization pass** — it reads GIMPLE, looks for a specific pattern, and replaces it.

### Structure of a GCC pass

Every pass in GCC follows this structure:

```cpp
class pass_riscv_attn_detect : public gimple_opt_pass
{
public:
  pass_riscv_attn_detect (gcc::context *ctxt)
    : gimple_opt_pass (data, ctxt) {}

  // Called for every function in the compilation unit
  unsigned int execute (function *fun) override
  {
    // 1. Look at the function's loops
    // 2. Check if they match the attention pattern
    // 3. If yes, replace them with the attn instruction
    return 0;
  }
};
```

GCC calls `execute()` once for every function being compiled. If the function has loops that match the attention pattern, we replace them. If not, we do nothing and return.

---

## 23. Our GIMPLE Pass — The Big Picture

The file `src/riscv-attn-detect.cc` is ~1300 lines of C++. Here is the conceptual overview.

### What the pass does

1. **Collects all top-level loops** in the function
2. **Slides a window of 4 consecutive loops** across them
3. **Checks each window** against the 4-stage attention pattern:

```
Loop 1: matmul     — score[i][j] += Q[i][k] * K[j][k]     (Q × Kᵀ)
Loop 2: scale      — score[i][j] *= 1/sqrt(d)              (÷ √dₖ)
Loop 3: softmax    — exp, sum, normalize per row            (softmax)
Loop 4: matmul     — out[i][j] += score[i][k] * V[k][j]    (× V)
```

4. **If all 4 match**, replaces all loops with a single `attn` instruction

### The loop tree

GCC organizes loops in a tree structure. Consider this code:

```c
for (i = ...) {           // Loop 1 (top-level)
    for (j = ...) {       //   Loop 1a (child of Loop 1)
        for (k = ...) {   //     Loop 1b (child of 1a)
            ...
        }
    }
}
for (i = ...) {           // Loop 2 (top-level)
    for (j = ...) {       //   Loop 2a (child of Loop 2)
        ...
    }
}
```

The **loop tree** looks like:

```
Root
├── Loop 1 (i)
│   └── Loop 1a (j)
│       └── Loop 1b (k)
└── Loop 2 (i)
    └── Loop 2a (j)
```

Our pass only looks at **top-level loops** (direct children of root). For the attention pattern, we expect 4 top-level loops:
1. A triple-nested loop (matmul 1)
2. A double-nested loop (scale)
3. A loop with 2-3 children (softmax)
4. A triple-nested loop (matmul 2)

### GCC quirk: reverse order

GCC's loop tree lists children in **reverse program order**. So if your code has loops A, B, C, D in that order, GCC's loop list has them as D, C, B, A. Our pass reverses them back before pattern matching.

---

## 24. Detection Phase — How the Pass Reads Loops

For each of the 4 stages, we have a detector function.

### Stage 1 & 4: `is_matmul_pattern()`

Detects a triple-nested loop performing matrix multiplication:

```c
for (i = 0; i < n; i++)
    for (j = 0; j < n; j++) {
        float sum = 0.0f;
        for (k = 0; k < d; k++)
            sum += Q[i*d + k] * K[j*d + k];
        scores[i*n + j] = sum;
    }
```

What the detector looks for in GIMPLE:
1. **Triple nesting:** The loop has a child, which has a child (3 levels deep)
2. **Inner loop body** contains a `MULT_EXPR` (multiplication) and `PLUS_EXPR` (addition) — the accumulation pattern `sum += a * b`
3. **Memory accesses** to array elements via `MEM_REF` or `TARGET_MEM_REF`

The function `find_matmul_reduction()` walks the GIMPLE statements in the innermost loop body, looking for:
```
_temp = _a * _b;       // MULT_EXPR
_sum = _sum_old + _temp; // PLUS_EXPR (accumulation)
```

If both operands of the multiplication come from memory loads (array accesses), and the result is accumulated, it is a matmul.

### Stage 2: `is_elementwise_div()`

Detects a double-nested loop performing element-wise scaling:

```c
for (i = 0; i < n; i++)
    for (j = 0; j < n; j++)
        scores[i*n + j] *= scale;
```

What the detector looks for:
1. **Double nesting:** The loop has exactly one child (2 levels)
2. **Inner loop body** contains a `MULT_EXPR` where one operand is a **loop-invariant scalar** (the scale factor — it does not change across iterations)
3. The other operand and the store target are the same array

### Stage 3: `is_softmax_pattern()`

Detects the softmax computation:

```c
for (i = 0; i < n; i++) {
    float row_sum = 0.0f;
    for (j = 0; j < n; j++) {
        scores[i*n + j] = __builtin_expf(scores[i*n + j]);
        row_sum += scores[i*n + j];
    }
    for (j = 0; j < n; j++)
        scores[i*n + j] /= row_sum;
}
```

What the detector looks for:
1. **Outer loop** with 2 or 3 **child loops** (not just nesting — sibling loops under one parent)
2. One child contains a call to `__builtin_expf()` (the `exp` function)
3. Another child contains `RDIV_EXPR` (division — the normalization step)

Why 2 or 3 children? A numerically stable softmax has 3 steps (find max, exp+sum, normalize = 3 children). A simple softmax combines exp+sum into one loop (2 children). Our detector handles both.

---

## 25. Replacement Phase — How the Pass Emits the Instruction

When all 4 stages match, the pass replaces all loops with a single instruction. Here is how:

### Step 1: Extract function parameters

The pass uses `DECL_ARGUMENTS` to get the original function parameters:

```c
void attention(int n, int d, float *Q, float *K, float *V, float *out)
                ^^^    ^^^          ^^^        ^^^        ^^^
```

It walks the argument chain: `n` → `d` → `Q` → `K` → `V`.

### Step 2: Build structs on the stack

The pass inserts GIMPLE statements that build two structs on the stack:

```c
// dims struct at sp+56
*(int*)(sp+56) = n;        // rows
*(int*)(sp+60) = n;        // cols
*(int*)(sp+64) = n;        // seq_len
*(int*)(sp+68) = d;        // d_model

// qkv struct at sp+72
*(float**)(sp+72) = Q;
*(float**)(sp+80) = K;
*(float**)(sp+88) = V;
```

### Step 3: Create a fresh basic block

This is where it gets tricky. We need to insert our instruction **before** the first loop, redirecting execution to skip all 4 loops. We use `split_edge()`:

```
BEFORE:
  [preheader] ──edge──> [loop1 header]

AFTER split_edge():
  [preheader] ──> [NEW BLOCK] ──> [loop1 header]
```

`split_edge()` creates a fresh, empty basic block. We insert our instruction into this new block.

### Step 4: Emit the `attn` instruction

We use **volatile inline assembly** to emit the instruction:

```c
asm volatile (".insn r 0x0b, 0, 0x01, x0, %0, %1"
              : /* no outputs */
              : "r" (dims_ptr), "r" (qkv_ptr)
              : "memory");
```

The `.insn r` directive tells the assembler to encode an R-type instruction with:
- opcode = `0x0b` (custom-0)
- funct3 = `0`
- funct7 = `0x01`
- rd = `x0` (zero register — we do not need a return value)
- rs1 = `%0` (dims pointer, chosen by register allocator)
- rs2 = `%1` (qkv pointer, chosen by register allocator)

**Why volatile?** The `volatile` keyword tells GCC: "Do NOT remove this, even if you think the result is unused." Without it, GCC's Dead Code Elimination (DCE) pass would see that the instruction has no visible output and delete it. This was one of the hardest bugs to fix (see section 28, Fix 11).

**Why not use the builtin?** We tried `__builtin_riscv_attn()` first. The problem: it returns `unsigned long`, but we never use the return value. DCE saw the unused return value and removed the entire call. Even marking it volatile in various ways did not prevent DCE. Volatile inline assembly is the only construct GCC **guarantees** it will never remove.

### Step 5: Redirect control flow

After inserting the instruction, we redirect the new block's outgoing edge to jump **past** all 4 loops, directly to the function's exit:

```
BEFORE:
  [preheader] ──> [NEW BLOCK with attn] ──> [loop1 header] ──> ... ──> [loop4] ──> [exit]

AFTER redirect:
  [preheader] ──> [NEW BLOCK with attn] ──> [exit]

  [loop1 header] ──> ... ──> [loop4]   ← now unreachable dead code
```

All 4 original loops become unreachable. GCC's built-in dead code elimination cleans them up automatically.

---

## 26. Integrating the Pass into GCC

The GIMPLE pass requires 4 modifications to the GCC build system. These are done manually after the automation script.

### File 7a: Copy the source

```bash
cp custom_attn/src/riscv-attn-detect.cc gcc/gcc/config/riscv/riscv-attn-detect.cc
```

This is the ~1300-line pass source file.

### File 7b: Add to build objects (t-riscv)

```bash
echo '' >> gcc/gcc/config/riscv/t-riscv
echo '# GIMPLE attention detection pass' >> gcc/gcc/config/riscv/t-riscv
echo 'EXTRA_OBJS += riscv-attn-detect.o' >> gcc/gcc/config/riscv/t-riscv
```

`EXTRA_OBJS` tells GCC's build system to compile and link our file. Without this, the `.cc` file would sit there uncompiled.

### File 7c: Add build rule (Makefile.in)

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

This tells `make` how to compile our `.cc` file and what header dependencies it has. The indented lines **must** use tab characters (not spaces) — this is a Makefile requirement.

### File 7d: Register the pass (riscv.cc)

```bash
# Add the include
sed -i '/#include "tm_p.h"/a #include "context.h"' gcc/gcc/config/riscv/riscv.cc

# Register the pass
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

What this does:
- `#include "context.h"` — needed for `gcc::context`, which is GCC's global state object
- `make_pass_riscv_attn_detect(g)` — creates an instance of our pass
- `reference_pass_name = "loop"` — insert after the "loop" pass
- `PASS_POS_INSERT_AFTER` — insert *after* (not before) the reference pass

This is how GCC's pass manager works: you tell it "insert my pass after the 'loop' pass" and it handles the scheduling.

---

## 27. Verifying Auto-Detection

After building with both Layer 1 and Layer 2:

```bash
# Compile with auto-detection
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c custom_attn/demo/mainloops.c -o mainloops.o

# Check the output
riscv64-unknown-elf-objdump -d mainloops.o
```

Expected output (key section):

```
000000000000001a <.L2>:
  1a:   00a05d63    blez    a0,34 <.L1>     # if n <= 0, skip
  1e:   dc2a        sw      a0,56(sp)       # dims.rows = n
  20:   de2a        sw      a0,60(sp)       # dims.cols = n
  22:   c0aa        sw      a0,64(sp)       # dims.seq_len = n
  24:   c2ae        sw      a1,68(sp)       # dims.d_model = d
  26:   e4b2        sd      a2,72(sp)       # qkv.Q = Q
  28:   e8b6        sd      a3,80(sp)       # qkv.K = K
  2a:   ecba        sd      a4,88(sp)       # qkv.V = V
  2c:   183c        addi    a5,sp,56        # a5 = &dims
  2e:   00b8        addi    a4,sp,72        # a4 = &qkv
  30:   02e7800b    attn    zero,a5,a4      # ALL 4 LOOPS REPLACED
```

**No loop code at all.** The compiler:
1. Detected the 4-stage attention pattern
2. Built the dims and qkv structs on the stack from the function arguments
3. Emitted a single `attn` instruction
4. Eliminated all original loop code

### GIMPLE dump verification

You can see the pass's debug output:

```bash
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c mainloops.c -o mainloops.o \
    -fdump-tree-all-details

grep "ATTENTION PATTERN DETECTED" mainloops.c.*riscv_attn_detect
```

Output:
```
*** ATTENTION PATTERN DETECTED ***
Loops: 1 (matmul1) → 2 (scale) → 3 (softmax) → 4 (matmul2)
```

---

## 28. The 11 Fixes — What Went Wrong and Why

Building this pass required solving 11 bugs, mostly related to GCC 15.2.0's internal behavior. These are documented here because each one teaches something about how GCC works internally.

### Fix 1: TARGET_MEM_REF vs MEM_REF

**Problem:** Pattern matching failed — the detector could not find array accesses.

**Root cause:** GCC has two ways to represent memory accesses: `MEM_REF` (simple pointer dereference) and `TARGET_MEM_REF` (optimized memory reference with base + index + offset). After loop optimization, GCC 15 converts most `MEM_REF` nodes to `TARGET_MEM_REF`. Our detector only checked for `MEM_REF`.

**Fix:** Added `TARGET_MEM_REF` checks alongside `MEM_REF` in all pattern-matching functions.

### Fix 2: PLUS_EXPR operand ordering

**Problem:** Matmul detection missed some cases.

**Root cause:** In `sum += a * b`, the GIMPLE `PLUS_EXPR` can have the multiplication result in either rhs1 or rhs2. We only checked one position.

**Fix:** Check both positions of the PLUS_EXPR operands.

### Fix 3: Loop ordering (reverse program order)

**Problem:** Pattern matching found the loops in wrong order — matched scale before matmul.

**Root cause:** GCC's loop tree lists child loops in **reverse program order**. Code with loops A, B, C, D gets listed as D, C, B, A.

**Fix:** Reverse the collected loop list before pattern matching. Added a guard: skip reversal when list length ≤ 1 to prevent unsigned integer underflow (which would crash the compiler on **every** function, breaking the entire toolchain build including newlib).

### Fix 4: Softmax 2-child support

**Problem:** Softmax detection failed for simple (non-numerically-stable) softmax.

**Root cause:** Simple softmax produces 2 child loops (exp+sum combined, normalize), not 3 (max-reduce, exp+sum, normalize). The detector required exactly 3.

**Fix:** Accept 2 or 3 children. Auto-detect which child has `__builtin_expf()` calls to handle both orderings.

### Fix 5: Store target extraction

**Problem:** Matmul detection could not find where the result was stored.

**Root cause:** The accumulator variable (`sum`) lives in the innermost loop, but the store to the scores array happens in the **middle** loop (after the inner loop completes). We only searched the innermost loop.

**Fix:** Scan the middle loop (`loop_outer(innermost)`) for memory stores.

### Fix 6: Function parameter extraction

**Problem:** Segfault during the replacement phase.

**Root cause:** SSA base pointers from pattern matching are deep in address arithmetic chains. Calling `fold_convert()` on them caused segfaults.

**Fix:** Instead of tracing SSA chains, use `DECL_ARGUMENTS` to get the original function parameters (`n`, `d`, `Q`, `K`, `V`) directly from the function declaration.

### Fix 7: Header dependencies

**Problem:** Various compilation errors when building the pass.

**Fix:** Sorted out includes:
- Added `cfghooks.h` for `redirect_edge_and_branch()`
- Removed unused `tree-data-ref.h`
- Fixed include ordering
- Added `context.h` to `riscv.cc` for `gcc::context`

### Fix 8: Builtin lookup — NULL gaps

**Problem:** `__builtin_riscv_attn` not found during replacement.

**Root cause:** The RISC-V target builtin table has NULL entries (gaps). Our iteration broke on the first NULL entry, never reaching `__builtin_riscv_attn` at index 506.

**Fix:** Skip NULL and `error_mark_node` entries with `continue`, iterate up to 5000 entries.

*Note: This fix is historical — the current implementation uses inline asm instead of the builtin call (Fix 11), so this code path is no longer used. But the fix remains in the code.*

### Fix 9: Code inserted after terminating branch

**Problem:** The `attn` instruction was emitted but never executed.

**Root cause:** `gsi_last_bb(insert_bb)` followed by `gsi_insert_after()` placed the replacement code **after** the block's terminating branch instruction. Assembly instructions after a branch are unreachable dead code.

**Fix:** Use `split_edge()` to create a fresh, empty basic block (with no terminator). Insert code using `gsi_start_bb()` on the new block. This guarantees the code is reachable.

### Fix 10: exit_bb NULL — single_exit() returns NULL

**Problem:** Could not find where to redirect control flow after the `attn` instruction.

**Root cause:** `single_exit(loop4)` returns NULL in GCC 15 for loops with complex exit structure or stale loop analysis info.

**Fix:** Three-level fallback:
1. Try `single_exit(l4)`
2. If NULL: manually scan loop 4's basic blocks for edges leaving the loop using `flow_bb_inside_loop_p()`
3. Last resort: use the function's `EXIT_BLOCK` predecessor

### Fix 11: DCE removes builtin call (unused result)

**Problem:** The `attn` instruction was being generated correctly, then deleted by a later optimization pass. The final objdump showed the struct setup code but no `attn` instruction.

**Root cause:** `__builtin_riscv_attn()` returns `unsigned long`, but the return value was never used. GCC's Dead Code Elimination pass saw the unused result and removed the entire call — even though the instruction has important side effects (it triggers a hardware accelerator).

**Fix:** Switched from the builtin call to **volatile inline assembly**:

```c
asm volatile (".insn r 0x0b, 0, 0x01, x0, %0, %1" : : "r"(dims), "r"(qkv) : "memory");
```

Volatile asm is the one construct GCC **guarantees** it will never remove, regardless of optimization level. This fixed the issue permanently.

---

# Part 6 — Extras

## 29. R4-Type — 3-Input Instructions

If your custom instruction needs 3 source registers, use **R4-type**:

```
31    27 26 25 24    20 19    15 14  12 11     7 6      0
┌────────┬─────┬──────┬────────┬──────┬────────┬────────┐
│  rs3   │ f2  │ rs2  │  rs1   │funct3│   rd   │ opcode │
└────────┴─────┴──────┴────────┴──────┴────────┴────────┘
  5 bits  2 bits 5 bits 5 bits  3 bits  5 bits   7 bits
```

The funct7 field splits into rs3 (5 bits) + funct2 (2 bits). Use `--inputs 3` with the automation script.

---

## 30. The Automation Script — How It Works

The file `scripts/automate_instruction.sh` (and `.py`) automates Layer 1:

1. **Scans** `riscv-opc.h` for all existing `MATCH_*` values
2. **Finds** the first unused (opcode, funct3, funct7) combination
3. **Patches** all 6 source files with the new instruction
4. **Is idempotent** — running it twice does not create duplicates

Key functions:

| Function | What it does |
|----------|-------------|
| `find_free_opcode()` | Scans custom-0 through custom-3 for unused slots |
| `compute_match_mask()` | Calculates MATCH and MASK from (opcode, funct3, funct7) |
| Modify functions | Insert code at the right anchor point in each file |

Usage:
```bash
cd custom_attn/scripts
./automate_instruction.sh add attn 2          # Add 2-input instruction
./automate_instruction.sh delete attn         # Remove instruction
./automate_instruction.sh list                # List custom instructions
./automate_instruction.sh scan 2              # Show free opcode slots
```

---

## 31. Quick Reference Card

```
Instruction:     attn rd, rs1, rs2
Opcode slot:     custom-0 (0x0B)
Format:          R-type
funct3:          0x0
funct7:          0x01
MATCH:           0x0200000B
MASK:            0xFE00707F
GCC builtin:     __builtin_riscv_attn(unsigned long, unsigned long)
Operand format:  "d,s,t"
Insn class:      INSN_CLASS_I
GCC type:        RISCV_DI_FTYPE_DI_DI
UNSPEC:          UNSPEC_ATTN
RTL pattern:     "riscv_attn"
GIMPLE pass:     riscv_attn_detect (runs after "loop" pass)
Target:          riscv64-unknown-elf
Architecture:    rv64gc / lp64d

Files modified (Layer 1 — automated):
  1. binutils/include/opcode/riscv-opc.h    — MATCH/MASK macros
  2. binutils/opcodes/riscv-opc.c           — Opcode table entry
  3. gcc/gcc/config/riscv/riscv-ftypes.def  — Function type
  4. gcc/gcc/config/riscv/riscv-builtins.cc — Builtin registration
  5. gcc/gcc/config/riscv/riscv.md          — Machine description
  6. riscv-opcodes/extensions/rv_custom      — Opcode registry

Files modified (Layer 2 — manual):
  7a. gcc/gcc/config/riscv/riscv-attn-detect.cc  — GIMPLE pass (NEW)
  7b. gcc/gcc/config/riscv/t-riscv                — Build objects
  7c. gcc/gcc/Makefile.in                         — Build rule
  7d. gcc/gcc/config/riscv/riscv.cc               — Pass registration
```
