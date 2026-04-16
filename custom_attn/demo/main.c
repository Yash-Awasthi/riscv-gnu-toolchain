/*
 * custom_attn/demo/main.c
 *
 * Demo for the custom RISC-V "attn" instruction.
 * Group 9 — Attention Mechanism Custom Opcode
 *
 * Instruction:  attn rd, rs1, rs2
 *   rs1 = address of attn_dims_t   (matrix dimensions)
 *   rs2 = address of attn_qkv_t    (Q / K / V matrix pointers)
 *   rd  = result / status output
 *
 * The custom instruction is emitted by GCC via __builtin_riscv_attn().
 * No inline assembly is used.
 *
 * Compile:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \
 *       -ffreestanding -nostdinc -c main.c -o main.o
 *
 * Disassemble:
 *   riscv64-unknown-elf-objdump -d main.o
 *
 * Expected objdump output:
 *   <run_attention>:
 *      0:  00b5050b   attn  a0,a0,a1
 *      4:  8082       ret
 *
 * WHY noinline is required (not just cosmetic):
 *   GCC thinks attn is a pure integer ALU operation — it has no idea
 *   that the instruction dereferences the pointers in rs1/rs2 to read
 *   memory. Without noinline, GCC may reorder the attn instruction
 *   BEFORE the struct data is written to the stack (since it sees no
 *   memory dependency). The noinline wrapper forces a function call
 *   boundary, which guarantees all stores are flushed before attn
 *   executes. This is a correctness requirement, not a cosmetic choice.
 */

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

    attn_dims_t dims = {2, 2, 4, 8};
    attn_qkv_t qkv = {Q, K, V};

    volatile unsigned long result = run_attention(
        (unsigned long)&dims,
        (unsigned long)&qkv
    );

    (void)result;
    return 0;
}
