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
| funct7   | [31:25] | 7     | `0000000`  | `0x00` | Function code (extended)       |

### Encoding Constants

| Constant    | Value          | Description                                   |
|-------------|----------------|-----------------------------------------------|
| MATCH_ATTN  | `0x0000000b`   | Fixed bits that identify the attn instruction  |
| MASK_ATTN   | `0xfe00707f`   | Bit mask for funct7 + funct3 + opcode fields   |

### Operand Fields

| Operand | Register Class | ABI Name | Role                                          |
|---------|----------------|----------|-----------------------------------------------|
| rd      | Integer (x0-x31) | a0     | Output: result/status register                 |
| rs1     | Integer (x0-x31) | a0     | Input: address of `attn_dims_t` struct         |
| rs2     | Integer (x0-x31) | a1     | Input: address of `attn_qkv_t` struct          |

### Assembly Syntax

```asm
attn  rd, rs1, rs2
```

Example:
```asm
attn  a0, a0, a1    # encoded as 0x00b5050b
```

### Instruction Format (R-type)

```
 31        25 24    20 19    15 14  12 11     7 6      0
┌───────────┬────────┬────────┬──────┬────────┬────────┐
│  funct7   │  rs2   │  rs1   │funct3│   rd   │ opcode │
│  0000000  │ 01011  │ 01010  │ 000  │ 01010  │0001011 │
│   0x00    │  a1    │  a0    │ 0x0  │  a0    │  0x0b  │
└───────────┴────────┴────────┴──────┴────────┴────────┘
          = 0x00B5050B  (attn a0, a0, a1)
```

### Newly Added Items

#### New Opcode Entry (binutils/opcodes/riscv-opc.c)
```c
{"attn", 0, INSN_CLASS_I, "d,s,t", MATCH_ATTN, MASK_ATTN, match_opcode, 0}
```

| Field        | Value         | Meaning                                     |
|--------------|---------------|---------------------------------------------|
| name         | `"attn"`      | Assembly mnemonic                            |
| xlen         | `0`           | Works on any XLEN (rv32/rv64)                |
| isa          | `INSN_CLASS_I`| Base integer ISA (no extensions needed)      |
| operands     | `"d,s,t"`     | Format: rd(int), rs1(int), rs2(int)          |
| match        | `MATCH_ATTN`  | = 0x0000000b                                 |
| mask         | `MASK_ATTN`   | = 0xfe00707f                                 |
| match_func   | `match_opcode`| Standard opcode matcher                      |
| pinfo        | `0`           | No special flags                             |

#### New GCC Builtin
```
__builtin_riscv_attn(unsigned long dims_addr, unsigned long qkv_addr)
```

| Property     | Value                    |
|--------------|--------------------------|
| Name         | `__builtin_riscv_attn`   |
| Return type  | `unsigned long` (DI)     |
| Argument 1   | `unsigned long` (DI) — address of attn_dims_t |
| Argument 2   | `unsigned long` (DI) — address of attn_qkv_t  |
| Type macro   | `RISCV_DI_FTYPE_DI_DI`  |
| Availability | `always_enabled`         |
| UNSPEC       | `UNSPEC_ATTN`            |
| RTL type     | `"arith"` (integer ALU)  |

### Input Struct Definitions

```c
/* Struct 1: Matrix dimensions */
typedef struct {
    int rows;      /* number of rows in Q/K/V         */
    int cols;      /* number of columns (= head_dim)  */
    int seq_len;   /* sequence length (tokens)        */
    int d_model;   /* full model embedding dimension  */
} attn_dims_t;

/* Struct 2: Q/K/V matrix pointers */
typedef struct {
    float *Q;      /* Query  matrix pointer */
    float *K;      /* Key    matrix pointer */
    float *V;      /* Value  matrix pointer */
} attn_qkv_t;
```

### Mathematical Operation

The `attn` instruction performs the Transformer attention mechanism:

```
Attention(Q, K, V) = softmax(Q · K^T / √d_k) · V
```

Where:
- Q = Query matrix   (seq_len × d_head)
- K = Key matrix     (seq_len × d_head)
- V = Value matrix   (seq_len × d_head)
- d_k = head dimension (cols in attn_dims_t)
