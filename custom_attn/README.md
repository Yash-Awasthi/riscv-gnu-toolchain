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
- `rs1`: address of dimensions struct `{rows, cols, seq_len, d_model}`
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

### Path B: Automatic Detection (GIMPLE Pass)

Write plain C loops — the compiler detects the pattern and emits `attn` automatically:

```c
void attention(int n, int d, float *Q, float *K, float *V, float *out)
{
    float scores[64*64];
    float scale = 1.0f / __builtin_sqrtf((float)d);

    // Stage 1: Q * K^T
    for (i...) for (j...) { sum=0; for (k...) sum += Q[..]*K[..]; scores[..]=sum; }

    // Stage 2: scale by 1/sqrt(d)
    for (i...) for (j...) scores[..] *= scale;

    // Stage 3: softmax per row
    for (i...) { for (j...) { scores[..]=expf(scores[..]); sum+=...; } for (j...) scores[..]/=sum; }

    // Stage 4: scores * V
    for (i...) for (j...) { sum=0; for (k...) sum += scores[..]*V[..]; out[..]=sum; }
}
```

Both paths produce: `attn a0,a0,a1` (encoding `0x02b5050b`)

## Toolchain Modifications

### Files 1–6: Instruction Encoding & Builtin

| # | File | What |
|---|------|------|
| 1 | `binutils/include/opcode/riscv-opc.h` | MATCH/MASK macros |
| 2 | `binutils/opcodes/riscv-opc.c` | Opcode table entry |
| 3 | `gcc/config/riscv/riscv-ftypes.def` | Function type |
| 4 | `gcc/config/riscv/riscv-builtins.cc` | Builtin registration |
| 5 | `gcc/config/riscv/riscv.md` | Machine description pattern |
| 6 | `riscv-opcodes/extensions/rv_custom` | Official registration |

### File 7: GIMPLE Auto-Detection Pass

| File | Action | What |
|------|--------|------|
| `gcc/config/riscv/riscv-attn-detect.cc` | **NEW** | GIMPLE pass (~1200 lines) |
| `gcc/config/riscv/t-riscv` | Modified | `EXTRA_OBJS += riscv-attn-detect.o` |
| `gcc/Makefile.in` | Modified | Build rule for the pass |
| `gcc/config/riscv/riscv.cc` | Modified | `register_pass()` in `riscv_option_override()` |

The pass runs as pass #366 in the GCC pipeline (after loop optimization).
It detects 4 consecutive top-level loops matching:
**matmul → scale → softmax → matmul** and replaces them with the `attn` instruction.

## Build

```bash
cd ~/dc/riscv-gnu-toolchain
mkdir -p build && cd build
../configure --prefix=$HOME/riscv --with-arch=rv64gc --with-abi=lp64d
make -j$(nproc)
export PATH=$HOME/riscv/bin:$PATH
```

## Verify

```bash
# Path A: Explicit builtin
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c demo/mainbuiltin.c -o mainbuiltin.o
riscv64-unknown-elf-objdump -d mainbuiltin.o
# Output: 0: 02b5050b  attn  a0,a0,a1

# Path B: Auto-detected loops
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c demo/mainloops.c -o mainloops.o
riscv64-unknown-elf-objdump -d mainloops.o
# Output: attn  a0,a0,a1 (replaces all loop code)

# GIMPLE dump verification
riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
    -ffreestanding -nostdlib -c demo/mainloops.c -o mainloops.o \
    -fdump-tree-all-details
grep "ATTENTION PATTERN DETECTED" mainloops.c.366t.riscv_attn_detect
# Output: *** ATTENTION PATTERN DETECTED ***
```

## Demo Files

| File | Description |
|------|-------------|
| `demo/mainbuiltin.c` | Explicit `__builtin_riscv_attn()` call |
| `demo/mainloops.c` | Plain C loops (auto-detected by GIMPLE pass) |

## Project Structure

```
custom_attn/
├── README.md                    # This file
├── implementation_log.txt       # Detailed technical log
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
    └── riscv-attn-detect.cc     # GIMPLE pass source
```
