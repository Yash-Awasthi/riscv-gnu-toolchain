/**
 * custom_attn/demo/main.c
 *
 * Demo program for the custom RISC-V "attn" instruction.
 * Group 9 — Attention Mechanism Custom Opcode
 *
 * This program demonstrates the custom "attn" instruction added to
 * the RISC-V GNU toolchain. The instruction computes the Transformer
 * attention mechanism:  softmax(Q * K^T / sqrt(d)) * V
 *
 * Instruction:  attn rd, rs1, rs2
 *   rs1 = address of attn_dims_t   (matrix dimensions)
 *   rs2 = address of attn_qkv_t    (Q / K / V matrix pointers)
 *   rd  = result / status output
 *
 * NO inline assembly is used. The custom instruction is emitted
 * by the compiler via __builtin_riscv_attn().
 *
 * Compile:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \
 *       -c main.c -o main.o
 *
 * Disassemble:
 *   riscv64-unknown-elf-objdump -d main.o
 *
 * Expected output in objdump:
 *   <run_attention>:
 *      0:  00b5050b   attn  a0,a0,a1
 *      4:  8082       ret
 */

/* No #include needed — we use only basic C types (unsigned long, int, float) */

/* ------------------------------------------------------------------ */
/* Struct 1: Matrix dimensions                                         */
/*   rows    = number of rows in Q/K/V matrices                       */
/*   cols    = number of columns  (= head_dim d_k)                    */
/*   seq_len = sequence length (number of tokens)                     */
/*   d_model = full model embedding dimension                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int rows;
    int cols;
    int seq_len;
    int d_model;
} attn_dims_t;

/* ------------------------------------------------------------------ */
/* Struct 2: Q, K, V matrix storage                                   */
/*   Q = query  matrix pointer  (seq_len * d_head floats)             */
/*   K = key    matrix pointer  (seq_len * d_head floats)             */
/*   V = value  matrix pointer  (seq_len * d_head floats)             */
/* ------------------------------------------------------------------ */
typedef struct {
    float *Q;
    float *K;
    float *V;
} attn_qkv_t;

/* ------------------------------------------------------------------ */
/* run_attention                                                       */
/*                                                                     */
/* Calls the custom RISC-V builtin __builtin_riscv_attn which the     */
/* modified GCC maps to the "attn" machine instruction.               */
/*                                                                     */
/* __builtin_riscv_attn(unsigned long, unsigned long) -> unsigned long */
/*   arg0 = address of attn_dims_t cast to unsigned long              */
/*   arg1 = address of attn_qkv_t  cast to unsigned long              */
/*   returns unsigned long  (result / status)                         */
/*                                                                     */
/* Kept __attribute__((noinline)) so the compiler does not inline     */
/* this function into main, ensuring the "attn" instruction is        */
/* visible in objdump as a standalone function.                        */
/* ------------------------------------------------------------------ */
__attribute__((noinline))
unsigned long run_attention(unsigned long dims_addr,
                            unsigned long qkv_addr)
{
    return __builtin_riscv_attn(dims_addr, qkv_addr);
}

/* ------------------------------------------------------------------ */
/* main: constructs the two structs and calls run_attention            */
/* ------------------------------------------------------------------ */
int main(void)
{
    /* 2x2 attention head with 4-token sequence
     *
     * In a real Transformer this would be:
     *   Attention(Q, K, V) = softmax(Q * K^T / sqrt(d_k)) * V
     *
     * The custom instruction performs this entire computation
     * in hardware given the struct addresses.
     */
    float Q[4] = {0.10f, 0.20f, 0.30f, 0.40f};
    float K[4] = {0.50f, 0.60f, 0.70f, 0.80f};
    float V[4] = {0.90f, 1.00f, 1.10f, 1.20f};

    attn_dims_t dims = {
        .rows    = 2,
        .cols    = 2,
        .seq_len = 4,
        .d_model = 8
    };

    attn_qkv_t qkv = {
        .Q = Q,
        .K = K,
        .V = V
    };

    /*
     * Pass addresses of both structs to the custom instruction.
     * 'volatile' prevents the compiler from removing the call
     * even though the result is not used further.
     */
    volatile unsigned long result = run_attention(
        (unsigned long)&dims,
        (unsigned long)&qkv
    );

    (void)result;   /* suppress unused-variable warning */
    return 0;
}
