/* mainloops.c — Plain C attention with automatic detection
 *
 * Writes the full attention mechanism as plain C loops:
 *   Attention(Q, K, V) = softmax(Q * K^T / sqrt(d_k)) * V
 *
 * The GIMPLE optimization pass (riscv_attn_detect) automatically
 * recognizes the 4-stage pattern and replaces all loops with a
 * single 'attn' hardware instruction — no builtin calls needed.
 *
 * Compile:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
 *       -ffreestanding -nostdlib -c mainloops.c -o mainloops.o
 *
 * Verify:
 *   riscv64-unknown-elf-objdump -d mainloops.o
 *
 * Expected: the 'attention' function disassembly contains 'attn a0,a0,a1'
 * instead of nested loop code.
 */

void attention(int n, int d,
               float *Q, float *K, float *V, float *out)
{
    float scores[64*64];
    float scale = 1.0f / __builtin_sqrtf((float)d);
    int i, j, k;

    /* Stage 1: Q * K^T (matrix multiply) */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            float sum = 0.0f;
            for (k = 0; k < d; k++)
                sum += Q[i*d + k] * K[j*d + k];
            scores[i*n + j] = sum;
        }

    /* Stage 2: element-wise scale by 1/sqrt(d_k) */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            scores[i*n + j] *= scale;

    /* Stage 3: softmax per row */
    for (i = 0; i < n; i++) {
        float row_sum = 0.0f;
        for (j = 0; j < n; j++) {
            scores[i*n + j] = __builtin_expf(scores[i*n + j]);
            row_sum += scores[i*n + j];
        }
        for (j = 0; j < n; j++)
            scores[i*n + j] /= row_sum;
    }

    /* Stage 4: scores * V (matrix multiply) */
    for (i = 0; i < n; i++)
        for (j = 0; j < d; j++) {
            float sum = 0.0f;
            for (k = 0; k < n; k++)
                sum += scores[i*n + k] * V[k*d + j];
            out[i*d + j] = sum;
        }
}
