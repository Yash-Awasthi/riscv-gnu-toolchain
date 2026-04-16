/*
 * main_idiom.c — Plain C attention mechanism (NO builtins, NO inline asm)
 *
 * The GIMPLE idiom detection pass (riscv-attn-detect.cc) recognizes
 * the 4-stage attention pattern in this file and silently replaces it
 * with __builtin_riscv_attn(), which lowers to the custom `attn`
 * instruction via the existing machine description.
 *
 * The user writes PLAIN C. The compiler does the rest.
 *
 * ┌─────────────────────────────────────────────────────────────────┐
 * │  What the user writes          What the compiler emits          │
 * │  ─────────────────────         ──────────────────────────       │
 * │  for loops computing:          attn a0, a0, a1                  │
 * │    Q×K^T → scale → softmax     (single custom instruction)      │
 * │    → score×V                                                    │
 * └─────────────────────────────────────────────────────────────────┘
 *
 * How the pass detects it:
 *
 *   Stage 1: Triple-nested loop with reduction score[i][j] += Q[i][k] * K[j][k]
 *            → Detected as matmul with transposed K (both access K via [j][k])
 *
 *   Stage 2: Double-nested loop score[i][j] /= sqrt(d_model)
 *            → Detected as elementwise division on same base array
 *
 *   Stage 3: Outer loop with 3 inner children:
 *              (a) max-reduction over row
 *              (b) sum += exp(score[i][j] - max) accumulation
 *              (c) score[i][j] = exp(score[i][j] - max) / sum normalization
 *            → Detected as softmax pattern
 *
 *   Stage 4: Triple-nested loop output[i][j] += score[i][k] * V[k][j]
 *            → Detected as matmul with score as first operand
 *
 *   All 4 stages operate on the same 'score' array → confirmed as
 *   self-attention. Replaced with attn(dims, qkv) call.
 *
 * Compile:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \
 *       -lm -fdump-tree-riscv_attn_detect-details \
 *       -c main_idiom.c -o main_idiom.o
 *
 * Verify detection (check GIMPLE dump):
 *   cat main_idiom.c.*t.riscv_attn_detect
 *   → Should show "ATTENTION PATTERN DETECTED"
 *
 * Verify instruction (check disassembly):
 *   riscv64-unknown-elf-objdump -d main_idiom.o | grep attn
 *   → Should show:  attn  a0, a0, a1  (or similar register allocation)
 *
 * Compare with explicit-builtin version (main.c):
 *   Both should produce the same attn instruction in objdump.
 *   main.c uses __builtin_riscv_attn() explicitly.
 *   main_idiom.c uses plain C — the compiler inserts the builtin automatically.
 */

#include <math.h>   /* sqrtf(), expf() — standard C, nothing special */

/*
 * attention() — Standard scaled dot-product attention
 *
 * This is the exact formula from "Attention Is All You Need" (Vaswani et al.):
 *   Attention(Q, K, V) = softmax(Q × K^T / √d_model) × V
 *
 * Written as plain nested loops. No builtins. No asm. No annotations.
 * The compiler's GIMPLE pass detects this pattern and emits `attn`.
 */
void attention(int seq_len, int d_model,
               float Q[seq_len][d_model],
               float K[seq_len][d_model],
               float V[seq_len][d_model],
               float output[seq_len][d_model])
{
    float score[seq_len][seq_len];
    int i, j, k;

    /* ── Stage 1: Q × K^T → score ──────────────────────────────────
     * score[i][j] = Σ_k  Q[i][k] * K[j][k]
     *
     * Note: K is accessed as K[j][k], NOT K[k][j].
     * This means we're computing Q × K^T (K transposed), because
     * both Q and K use k as the reduction dimension.
     */
    for (i = 0; i < seq_len; i++)
        for (j = 0; j < seq_len; j++)
        {
            score[i][j] = 0.0f;
            for (k = 0; k < d_model; k++)
                score[i][j] += Q[i][k] * K[j][k];
        }

    /* ── Stage 2: Scale by 1/√d_model ──────────────────────────────
     * Prevents dot products from growing too large, which would push
     * softmax into saturation (gradients → 0).
     */
    float scale = sqrtf((float)d_model);
    for (i = 0; i < seq_len; i++)
        for (j = 0; j < seq_len; j++)
            score[i][j] /= scale;

    /* ── Stage 3: Softmax per row ──────────────────────────────────
     * For each row i:
     *   1. Find max (for numerical stability)
     *   2. Compute sum of exp(score - max)
     *   3. Normalize: score[i][j] = exp(score[i][j] - max) / sum
     */
    for (i = 0; i < seq_len; i++)
    {
        /* 3a. Max-reduction for numerical stability */
        float max_val = score[i][0];
        for (j = 1; j < seq_len; j++)
            if (score[i][j] > max_val)
                max_val = score[i][j];

        /* 3b. Sum of exp(score - max) */
        float sum = 0.0f;
        for (j = 0; j < seq_len; j++)
            sum += expf(score[i][j] - max_val);

        /* 3c. Normalize */
        for (j = 0; j < seq_len; j++)
            score[i][j] = expf(score[i][j] - max_val) / sum;
    }

    /* ── Stage 4: score × V → output ──────────────────────────────
     * output[i][j] = Σ_k  score[i][k] * V[k][j]
     *
     * Note: V is accessed as V[k][j] (standard matmul, no transpose).
     */
    for (i = 0; i < seq_len; i++)
        for (j = 0; j < d_model; j++)
        {
            output[i][j] = 0.0f;
            for (k = 0; k < seq_len; k++)
                output[i][j] += score[i][k] * V[k][j];
        }
}


/*
 * main() — Small test driver
 *
 * Uses 2×2 matrices (seq_len=2, d_model=2) for easy verification.
 * The attention() function above contains the pattern the GIMPLE pass
 * detects. After compilation, objdump should show `attn` in the
 * attention() function — not loops.
 */
int main(void)
{
    /* 2×2 test data (same as main.c for comparison) */
    float Q[4]   = {0.10f, 0.20f, 0.30f, 0.40f};
    float K[4]   = {0.50f, 0.60f, 0.70f, 0.80f};
    float V[4]   = {0.90f, 1.00f, 1.10f, 1.20f};
    float out[4] = {0};

    /* Call attention with seq_len=2, d_model=2 */
    attention(2, 2,
              (float (*)[2])Q,
              (float (*)[2])K,
              (float (*)[2])V,
              (float (*)[2])out);

    /* Result is in out[].
     * With the GIMPLE pass active, the attention() function body
     * has been replaced by a single `attn` instruction. */
    volatile float sink = out[0] + out[1] + out[2] + out[3];
    (void)sink;

    return 0;
}
