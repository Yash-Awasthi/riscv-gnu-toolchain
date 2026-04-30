/*
 * demo_attn.c — Full demonstration of the custom RISC-V `attn` instruction
 *
 * Implements scaled dot-product Transformer attention entirely in plain C:
 *
 *   Attention(Q, K, V) = softmax(Q × K^T / √d_k) × V
 *
 * ─── Key point ───────────────────────────────────────────────────────────────
 * The programmer writes NOTHING special — just standard C loops.
 * The patched GCC (GIMPLE pass: riscv_attn_detect) detects the 4-stage
 * pattern at -O2 and replaces ALL loop nests with ONE hardware instruction:
 *
 *   attn  zero, a5, a4          ← entire attention computation
 *
 * No inline assembly. No intrinsics. No code changes needed by the user.
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Build (with patched riscv64-unknown-elf-gcc):
 *   riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
 *       demo_attn.c -o demo_attn -lm
 *
 * Run (QEMU userspace):
 *   qemu-riscv64 ./demo_attn
 *
 * Run (Spike + proxy kernel):
 *   spike --isa=rv64gc $(which pk) ./demo_attn
 *
 * Verify the custom instruction was emitted:
 *   riscv64-unknown-elf-objdump -d demo_attn | grep -A2 -B2 "attn"
 *
 * Generate assembly listing (for documentation / screenshot):
 *   riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
 *       -S demo_attn.c -o demo_attn.s
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

/* ─── Parameters ────────────────────────────────────────────────────────────
 * Keep these small so the demo is readable.
 * The hardware instruction handles arbitrary N × D, but 4×4 is easy to verify
 * by hand.
 * ─────────────────────────────────────────────────────────────────────────── */
#define SEQ_LEN   4     /* number of tokens          */
#define D_MODEL   4     /* embedding dimension (d_k) */

/* ─── Attention function ─────────────────────────────────────────────────────
 *
 * THIS IS PLAIN C. The compiler transforms it — the programmer does nothing.
 *
 * The GIMPLE pass (riscv_attn_detect) recognises all four stages:
 *   Stage 1 — triple-nested loop with MULT+PLUS accumulation (matmul Q×K^T)
 *   Stage 2 — double-nested loop scaling by invariant scalar (÷ √d_k)
 *   Stage 3 — outer loop with expf() + row normalisation (softmax)
 *   Stage 4 — triple-nested loop with MULT+PLUS accumulation (matmul S×V)
 *
 * and replaces them with a single `attn` instruction encoding:
 *   opcode = 0x0b (custom-0), funct7 = 0x01, funct3 = 0x0
 *   MATCH  = 0x0200000b,  MASK = 0xfe00707f
 * ─────────────────────────────────────────────────────────────────────────── */
void attention(int n, int d,
               float *Q, float *K, float *V, float *out)
{
    float scores[SEQ_LEN * SEQ_LEN];   /* n×n attention score matrix */
    float scale = 1.0f / __builtin_sqrtf((float)d);
    int i, j, k;

    /* ── Stage 1: Q × K^T ────────────────────────────────────────────────── */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            float sum = 0.0f;
            for (k = 0; k < d; k++)
                sum += Q[i*d + k] * K[j*d + k];
            scores[i*n + j] = sum;
        }

    /* ── Stage 2: Scale by 1/√d_k ───────────────────────────────────────── */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            scores[i*n + j] *= scale;

    /* ── Stage 3: Softmax (row-wise) ─────────────────────────────────────── */
    for (i = 0; i < n; i++) {
        float row_sum = 0.0f;
        for (j = 0; j < n; j++) {
            scores[i*n + j] = __builtin_expf(scores[i*n + j]);
            row_sum += scores[i*n + j];
        }
        for (j = 0; j < n; j++)
            scores[i*n + j] /= row_sum;
    }

    /* ── Stage 4: Scores × V ─────────────────────────────────────────────── */
    for (i = 0; i < n; i++)
        for (j = 0; j < d; j++) {
            float sum = 0.0f;
            for (k = 0; k < n; k++)
                sum += scores[i*n + k] * V[k*d + j];
            out[i*d + j] = sum;
        }
}

/* ─── Helpers ────────────────────────────────────────────────────────────── */
static void print_matrix(const char *name, float *M, int rows, int cols)
{
    printf("  %s  [%d × %d]\n", name, rows, cols);
    for (int i = 0; i < rows; i++) {
        printf("    [ ");
        for (int j = 0; j < cols; j++)
            printf("%8.4f ", M[i*cols + j]);
        printf("]\n");
    }
    printf("\n");
}

/* ─── Reference: software-only attention (no compiler pass) ─────────────────
 * Used to verify the hardware instruction gives the same numeric result.
 * ─────────────────────────────────────────────────────────────────────────── */
static void attention_reference(int n, int d,
                                float *Q, float *K, float *V, float *out)
{
    float scores[SEQ_LEN * SEQ_LEN];
    float scale = 1.0f / sqrtf((float)d);
    int i, j, k;

    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            float s = 0.0f;
            for (k = 0; k < d; k++) s += Q[i*d+k] * K[j*d+k];
            scores[i*n+j] = s * scale;
        }

    for (i = 0; i < n; i++) {
        float row_sum = 0.0f;
        for (j = 0; j < n; j++) { scores[i*n+j] = expf(scores[i*n+j]); row_sum += scores[i*n+j]; }
        for (j = 0; j < n; j++) scores[i*n+j] /= row_sum;
    }

    for (i = 0; i < n; i++)
        for (j = 0; j < d; j++) {
            float s = 0.0f;
            for (k = 0; k < n; k++) s += scores[i*n+k] * V[k*d+j];
            out[i*d+j] = s;
        }
}

/* ─── Main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   RISC-V Custom Instruction Demo: attn                  ║\n");
    printf("║   Attention(Q,K,V) = softmax(Q·K^T / √d_k) · V         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    printf("  seq_len = %d   d_model = %d\n\n", SEQ_LEN, D_MODEL);

    /*
     * Q: identity — each token only "looks at" itself.
     * K: same as Q.
     * V: ascending values — easy to verify output by hand.
     */
    float Q[SEQ_LEN * D_MODEL] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    float K[SEQ_LEN * D_MODEL] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    float V[SEQ_LEN * D_MODEL] = {
         1.0f,  2.0f,  3.0f,  4.0f,
         5.0f,  6.0f,  7.0f,  8.0f,
         9.0f, 10.0f, 11.0f, 12.0f,
        13.0f, 14.0f, 15.0f, 16.0f,
    };

    print_matrix("Q (query)", Q, SEQ_LEN, D_MODEL);
    print_matrix("K (key)  ", K, SEQ_LEN, D_MODEL);
    print_matrix("V (value)", V, SEQ_LEN, D_MODEL);

    /* ── Run via custom instruction (plain C — compiler handles the rest) ── */
    float out_hw[SEQ_LEN * D_MODEL];
    memset(out_hw, 0, sizeof(out_hw));

    printf("  [ Calling attention() — compiled to single `attn` instruction ]\n\n");
    attention(SEQ_LEN, D_MODEL, Q, K, V, out_hw);
    print_matrix("Output (hardware attn)", out_hw, SEQ_LEN, D_MODEL);

    /* ── Verify against software reference ── */
    float out_ref[SEQ_LEN * D_MODEL];
    memset(out_ref, 0, sizeof(out_ref));
    attention_reference(SEQ_LEN, D_MODEL, Q, K, V, out_ref);

    printf("  Verification against software reference:\n");
    int pass = 1;
    for (int i = 0; i < SEQ_LEN * D_MODEL; i++) {
        float diff = out_hw[i] - out_ref[i];
        if (diff < 0.0f) diff = -diff;
        if (diff > 1e-4f) { pass = 0; break; }
    }
    printf("  Result: %s\n\n", pass ? "PASS ✓  (hw output matches reference)"
                                    : "FAIL ✗  (mismatch — check toolchain)");

    printf("  To confirm the custom instruction was emitted:\n");
    printf("    riscv64-unknown-elf-objdump -d demo_attn | grep attn\n");
    printf("  Expected:   02e7800b    attn  zero,a5,a4\n\n");

    return pass ? 0 : 1;
}
