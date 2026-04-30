# Custom Instruction: Opcode and Operand Fields

## Instruction: `attn` (Attention Mechanism — Group 9)

### Opcode Fields

| Field    | Bits    | Width | Value      | Hex    | Description                    |
|----------|---------|-------|------------|--------|--------------------------------|
| opcode   | [6:0]   | 7     | `0001011`  | `0x0b` | custom-0 opcode slot           |
| rd       | [11:7]  | 5     | varies     | —      | Destination integer register   |
| funct3   | [14:12] | 3     | `000`      | `0x0`  | Function code (sub-operation)  |
| rs1      | [19:15] | 5     | varies     | —      | Source integer register 1      |
| rs2      | [24:20] | 5     | varies     | —      | Source integer register 2      |
| funct7   | [31:25] | 7     | `0000001`  | `0x01` | Function code (extended)       |

### Encoding Constants

| Constant    | Value          | How it is computed                                       |
|-------------|----------------|----------------------------------------------------------|
| MATCH_ATTN  | `0x0200000b`   | `(funct7=0x01 << 25) \| (funct3=0 << 12) \| 0x0b`       |
| MASK_ATTN   | `0xfe00707f`   | Masks bits [31:25] + [14:12] + [6:0] (R-type standard)  |

Verification:
```
0x0200000b = 0000 0010 0000 0000 0000 0000 0000 1011
                  ^funct7=1                    ^opcode=0x0b (custom-0)
```

### Operand Fields

| Operand | Register Class   | Typical ABI | Role                                    |
|---------|------------------|-------------|-----------------------------------------|
| rd      | Integer (x0-x31) | a0          | Output: result/status register          |
| rs1     | Integer (x0-x31) | a0          | Input: address of `attn_dims_t` struct  |
| rs2     | Integer (x0-x31) | a1          | Input: address of `attn_qkv_t` struct   |

### Assembly Syntax

```asm
attn  rd, rs1, rs2
```

Examples:
```asm
attn  a0, a0, a1    # Explicit builtin path: encoded as 0x02b5050b
attn  zero, a5, a4  # GIMPLE pass (auto-detected loops)
```

### Instruction Format (R-type) — Bit-level Diagram

```
 31        25 24    20 19    15 14  12 11     7 6      0
+-----------+--------+--------+------+--------+--------+
|  funct7   |  rs2   |  rs1   |funct3|   rd   | opcode |
|  0000001  | 01011  | 01010  | 000  | 01010  |0001011 |
|   0x01    |  a1    |  a0    | 0x0  |  a0    |  0x0b  |
+-----------+--------+--------+------+--------+--------+
          = 0x02B5050B  (attn a0, a0, a1)
```

---

## Newly Added Items (Assignment Requirement #5)

### 1. Opcode Header (`binutils/include/opcode/riscv-opc.h`)

```c
#define MATCH_ATTN  0x0200000b
#define MASK_ATTN   0xfe00707f
DECLARE_INSN(attn, MATCH_ATTN, MASK_ATTN)
```

### 2. Opcode Table Entry (`binutils/opcodes/riscv-opc.c`)

```c
{"attn", 0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0}
```

| Field        | Value          | Meaning                                     |
|--------------|----------------|---------------------------------------------|
| name         | `"attn"`       | Assembly mnemonic                            |
| xlen         | `0`            | Works on any XLEN (rv32/rv64)                |
| isa          | `INSN_CLASS_I` | Base integer ISA — no F/D extension needed   |
| operands     | `"d,s,t"`      | rd (int), rs1 (int), rs2 (int)               |
| match        | `MATCH_ATTN`   | `0x0200000b`                                 |
| mask         | `MASK_ATTN`    | `0xfe00707f`                                 |
| match_func   | `match_opcode` | Standard opcode matcher                      |
| pinfo        | `0`            | No special flags                             |

### 3. Function Type (`gcc/gcc/config/riscv/riscv-ftypes.def`)

```c
DEF_RISCV_FTYPE (2, (DI, DI, DI))
/* Result: unsigned long __builtin_riscv_attn(unsigned long, unsigned long) */
```

### 4. GCC Builtin (`gcc/gcc/config/riscv/riscv-builtins.cc`)

```c
#define RISCV_ATYPE_DI long_integer_type_node       /* new */
AVAIL (always_enabled, (!0))                         /* new */
DIRECT_BUILTIN (attn, RISCV_DI_FTYPE_DI_DI, always_enabled),  /* new */
```

| Property     | Value                    |
|--------------|--------------------------|
| Name         | `__builtin_riscv_attn`   |
| Return type  | `unsigned long` (DI)     |
| Argument 1   | `unsigned long` — address of `attn_dims_t` |
| Argument 2   | `unsigned long` — address of `attn_qkv_t`  |
| Type macro   | `RISCV_DI_FTYPE_DI_DI`  |
| Availability | `always_enabled`         |
| UNSPEC       | `UNSPEC_ATTN`            |
| RTL type     | `"arith"` (integer ALU)  |

### 5. Machine Description (`gcc/gcc/config/riscv/riscv.md`)

UNSPEC constant added to enum:
```
UNSPEC_ATTN
```

RTL pattern:
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

### 6. riscv-opcodes Entry (`riscv-opcodes/extensions/rv_custom`)

```
attn rd rs1 rs2 31..25=1 14..12=0 6..2=0x02 1..0=3
```

---

## Input Struct Definitions (C Interface)

```c
/* Struct 1: Matrix dimensions — address passed in rs1 */
typedef struct {
    int rows;      /* number of rows in Q/K/V         */
    int cols;      /* number of columns (= head_dim)  */
    int seq_len;   /* sequence length (tokens)        */
    int d_model;   /* full model embedding dimension  */
} attn_dims_t;

/* Struct 2: Q/K/V matrix pointers — address passed in rs2 */
typedef struct {
    float *Q;   /* Query  matrix (seq_len x d_head, single-precision float) */
    float *K;   /* Key    matrix (seq_len x d_head, single-precision float) */
    float *V;   /* Value  matrix (seq_len x d_head, single-precision float) */
} attn_qkv_t;
```

**Note:** All matrix elements are **single-precision (SP) floating-point** (`float`, IEEE 754 binary32),
satisfying the assignment requirement for single-precision FP instructions.

---

## Mathematical Operation

```
Attention(Q, K, V) = softmax(Q * K^T / sqrt(d_k)) * V
```

Stages performed by the single `attn` instruction:

| Stage | Operation              | C loop nests replaced |
|-------|------------------------|-----------------------|
| 1     | Q x K^T                | 3 nested loops        |
| 2     | Scale by 1/sqrt(d_k)   | 2 nested loops        |
| 3     | Row-wise softmax       | 2 nested loops        |
| 4     | Scores x V             | 3 nested loops        |

**Total: 10 loop nests replaced by 1 instruction.**

---

## Quick Reference Card

```
Mnemonic : attn rd, rs1, rs2
Encoding : 0000001_rs2_rs1_000_rd_0001011
MATCH    : 0x0200000b
MASK     : 0xfe00707f
Slot     : custom-0 (opcode=0x0b, bits[6:2]=0x02)
funct3   : 0x0   (3 bits)
funct7   : 0x01  (7 bits)
Builtin  : __builtin_riscv_attn(dims_addr, qkv_addr)
Data     : single-precision float arrays (IEEE 754 binary32)
```
