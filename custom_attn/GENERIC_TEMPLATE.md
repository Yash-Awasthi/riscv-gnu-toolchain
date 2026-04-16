# Generic Template for Adding Custom RISC-V Instructions

## Overview

This document provides a step-by-step template for adding any new custom
instruction to the RISC-V GNU toolchain. Follow these steps to add your
own custom instruction (e.g., for accelerating a specific mathematical
operation).

---

## Prerequisites

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install -y autoconf automake autotools-dev curl python3 \
    libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex \
    texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev

# Clone the toolchain
git clone https://github.com/riscv-collab/riscv-gnu-toolchain
cd riscv-gnu-toolchain
git submodule update --init --depth 1 binutils gcc newlib
```

---

## Step 1: Choose Opcode Slot and Encoding

RISC-V reserves four opcode slots for custom instructions:

| Slot     | opcode[6:2] | opcode[6:0] | Full opcode |
|----------|-------------|-------------|-------------|
| custom-0 | 0x02        | 0001011     | 0x0b        |
| custom-1 | 0x0a        | 0101011     | 0x2b        |
| custom-2 | 0x16        | 1011011     | 0x5b        |
| custom-3 | 0x1e        | 1111011     | 0x7b        |

### Choose instruction format:

**R-type** (3 registers, most common):
```
| funct7 [31:25] | rs2 [24:20] | rs1 [19:15] | funct3 [14:12] | rd [11:7] | opcode [6:0] |
```

**I-type** (2 registers + 12-bit immediate):
```
| imm[11:0] [31:20] | rs1 [19:15] | funct3 [14:12] | rd [11:7] | opcode [6:0] |
```

### Calculate MATCH and MASK:

```python
# For R-type with custom-0:
opcode  = 0x0b       # bits [6:0]
funct3  = YOUR_F3    # bits [14:12]
funct7  = YOUR_F7    # bits [31:25]

MATCH = (funct7 << 25) | (funct3 << 12) | opcode
MASK  = 0xfe00707f   # masks funct7 + funct3 + opcode for R-type
```

---

## Step 2: Modify Binutils (Assembler + Disassembler)

### File 1: `binutils/include/opcode/riscv-opc.h`

Add MATCH/MASK defines after `/* Instruction opcode macros. */`:
```c
#define MATCH_YOUR_INSN  0x________  /* your calculated MATCH value */
#define MASK_YOUR_INSN   0x________  /* your calculated MASK value  */
```

Add DECLARE_INSN near the other DECLARE_INSN lines:
```c
DECLARE_INSN(your_insn, MATCH_YOUR_INSN, MASK_YOUR_INSN)
```

### File 2: `binutils/opcodes/riscv-opc.c`

Add entry to `riscv_opcodes[]` array:
```c
/* name,  xlen, isa,          operands, match,           mask,            match_func,   pinfo */
{"your_insn", 0, INSN_CLASS_I, "d,s,t",  MATCH_YOUR_INSN, MASK_YOUR_INSN, match_opcode, 0},
```

**Operand format strings:**

| Code | Meaning              | Register type    |
|------|----------------------|------------------|
| `d`  | rd  (destination)    | Integer          |
| `s`  | rs1 (source 1)       | Integer          |
| `t`  | rs2 (source 2)       | Integer          |
| `D`  | rd  (destination)    | Floating-point   |
| `S`  | rs1 (source 1)       | Floating-point   |
| `T`  | rs2 (source 2)       | Floating-point   |
| `R`  | rs3 (source 3)       | Floating-point   |

**Instruction class:**
- `INSN_CLASS_I` — base integer (no extensions required)
- `INSN_CLASS_F` — requires F (single-precision float) extension
- `INSN_CLASS_D` — requires D (double-precision float) extension

---

## Step 3: Modify GCC (Compiler Builtin)

### File 3: `gcc/gcc/config/riscv/riscv-ftypes.def`

Add function type at end of file:
```c
/* DEF_RISCV_FTYPE(num_args, (RETURN_TYPE, ARG1_TYPE, ..., ARGN_TYPE)) */

/* For integer-pointer operands (address inputs): */
DEF_RISCV_FTYPE (2, (DI, DI, DI))     /* unsigned long fn(unsigned long, unsigned long) */

/* For floating-point operands: */
DEF_RISCV_FTYPE (3, (SF, SF, SF, SF)) /* float fn(float, float, float) */
```

Common type codes:
- `DI` = doubleword integer (64-bit, for addresses on rv64)
- `SI` = single integer (32-bit)
- `SF` = single-precision float
- `DF` = double-precision float
- `VOID` = void return

### File 4: `gcc/gcc/config/riscv/riscv-builtins.cc`

**Addition A:** Add ATYPE mapping (if not already present):
```c
/* After: #define RISCV_ATYPE_SI intSI_type_node */
#define RISCV_ATYPE_DI long_integer_type_node   /* for DI type */
#define RISCV_ATYPE_SF float_type_node          /* for SF type */
```

**Addition B:** Register the builtin:
```c
/* Inside riscv_builtins[] array, after #include "corev.def" */
DIRECT_BUILTIN (your_insn, RISCV_DI_FTYPE_DI_DI, always_enabled),
```

The builtin will be callable as: `__builtin_riscv_your_insn(arg1, arg2)`

**Availability flags:**
- `always_enabled` — always available regardless of -march
- `hard_float` — requires FPU (-march includes F or D)

### File 5: `gcc/gcc/config/riscv/riscv.md`

**Addition A:** Add UNSPEC constant:
```
(define_c_enum "unspec" [
  ...
  UNSPEC_YOUR_INSN    ;; add inside existing list
  ...
])
```

**Addition B:** Add instruction pattern (at end of file):
```
;; Integer operands version:
(define_insn "riscv_your_insn"
  [(set (match_operand:DI 0 "register_operand" "=r")
        (unspec:DI [(match_operand:DI 1 "register_operand" "r")
                    (match_operand:DI 2 "register_operand" "r")]
                   UNSPEC_YOUR_INSN))]
  ""
  "your_insn\t%0,%1,%2"
  [(set_attr "type" "arith")
   (set_attr "mode" "DI")])

;; Float operands version (3 inputs):
(define_insn "riscv_your_insn"
  [(set (match_operand:SF 0 "register_operand" "=f")
        (unspec:SF [(match_operand:SF 1 "register_operand" "f")
                    (match_operand:SF 2 "register_operand" "f")
                    (match_operand:SF 3 "register_operand" "f")]
                   UNSPEC_YOUR_INSN))]
  "TARGET_HARD_FLOAT"
  "your_insn\t%0,%1,%2,%3"
  [(set_attr "type" "fmadd")
   (set_attr "mode" "SF")])
```

---

## Step 4: Build the Toolchain

```bash
cd riscv-gnu-toolchain
mkdir build && cd build

# For rv64 bare-metal target:
../configure --prefix=$HOME/riscv_custom \
             --with-arch=rv64imac --with-abi=lp64
make -j$(nproc)
make install
```

---

## Step 5: Write C Test Program

```c
#include <stdint.h>

/* NO inline assembly — use the compiler builtin directly */
unsigned long result = __builtin_riscv_your_insn(arg1, arg2);
```

---

## Step 6: Compile and Verify

```bash
export PATH=$HOME/riscv_custom/bin:$PATH

# Compile
riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 -c test.c -o test.o

# Disassemble — should show your_insn mnemonic
riscv64-unknown-elf-objdump -d test.o

# Generate hex dump
riscv64-unknown-elf-objcopy -O binary -j .text test.o test.bin
od -An -tx4 -w4 -v test.bin > hexdump.txt
```

---

## Summary: 6 Files to Modify

| # | File                                        | What to Add                    |
|---|---------------------------------------------|--------------------------------|
| 1 | `binutils/include/opcode/riscv-opc.h`      | MATCH, MASK, DECLARE_INSN      |
| 2 | `binutils/opcodes/riscv-opc.c`             | Opcode table entry             |
| 3 | `gcc/gcc/config/riscv/riscv-ftypes.def`    | Function type signature        |
| 4 | `gcc/gcc/config/riscv/riscv-builtins.cc`   | ATYPE mapping + DIRECT_BUILTIN |
| 5 | `gcc/gcc/config/riscv/riscv.md`            | UNSPEC enum + define_insn      |

After modifying these 6 files, rebuild the toolchain and your custom
instruction will be available as `__builtin_riscv_your_insn()` in C code.
