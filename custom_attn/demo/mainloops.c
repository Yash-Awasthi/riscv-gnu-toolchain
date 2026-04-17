/*
 * mainloops.c — Transformer Attention in plain C loops
 *
 * Implements the standard scaled dot-product attention:
 *   Attention(Q, K, V) = softmax(Q × K^T / √d_k) × V
 *
 * When compiled with the patched RISC-V GCC toolchain at -O2,
 * the GIMPLE auto-detection pass (riscv_attn_detect) recognizes
 * the 4-stage loop pattern and replaces ALL loops with a single
 * custom hardware instruction:
 *
 *   attn zero, a5, a4
 *
 * No explicit builtin call required — just plain C loops.
 *
 * Compile & verify:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
 *       -ffreestanding -nostdlib -c mainloops.c -o mainloops.o
 *   riscv64-unknown-elf-objdump -d mainloops.o
 *
 * Verified disassembly (no loop code remains):
 *
 *   0000000000000000 <attention>:
 *     ┌─ sqrtf guard: "float scale = 1.0f / sqrtf((float)d)"
 *     │  The compiler checks if d < 0 before computing sqrt.
 *     │  If d < 0, it branches to .L6 (error handler).
 *     │  Otherwise falls through to .L2 (main path).
 *     │
 *      0:   d005f553    fcvt.s.w    fa0,a1          # fa0 = (float)d
 *      4:   f00007d3    fmv.w.x     fa5,zero        # fa5 = 0.0
 *      8:   7159        addi        sp,sp,-112      # allocate stack frame
 *      a:   f486        sd          ra,104(sp)      # save return address
 *      c:   00102873    frflags     a6              # save FP exception flags
 *     10:   a0f517d3    flt.s       a5,fa0,fa5      # a5 = (d < 0.0) ? 1 : 0
 *     14:   00181073    fsflags     a6              # restore FP flags
 *     18:   e38d        bnez        a5,3a <.L6>     # if d < 0 → jump to .L6
 *     │
 *   000000000000001a <.L2>:
 *     │  Main path — reached after sqrtf succeeds (or from .L6 return).
 *     │  First checks: is n <= 0? If so, skip straight to .L1 (return).
 *     │  Otherwise: build dims/qkv structs on stack, emit attn instruction.
 *     │
 *     1a:   00a05d63    blez        a0,34 <.L1>     # if n <= 0 → skip to .L1
 *     1e:   dc2a        sw          a0,56(sp)       # dims.rows = n
 *     20:   de2a        sw          a0,60(sp)       # dims.cols = n
 *     22:   c0aa        sw          a0,64(sp)       # dims.seq_len = n
 *     24:   c2ae        sw          a1,68(sp)       # dims.d_model = d
 *     26:   e4b2        sd          a2,72(sp)       # qkv.Q = Q
 *     28:   e8b6        sd          a3,80(sp)       # qkv.K = K
 *     2a:   ecba        sd          a4,88(sp)       # qkv.V = V
 *     2c:   183c        addi        a5,sp,56        # a5 = &dims
 *     2e:   00b8        addi        a4,sp,72        # a4 = &qkv
 *     30:   02e7800b    attn        zero,a5,a4      # <-- ALL 4 LOOPS REPLACED
 *     │
 *   0000000000000034 <.L1>:
 *     │  Function epilogue — reached from:
 *     │    (a) n <= 0 at .L2 (blez), or
 *     │    (b) after attn instruction at 0x30
 *     │
 *     34:   70a6        ld          ra,104(sp)      # restore return address
 *     36:   6165        addi        sp,sp,112       # deallocate stack frame
 *     38:   8082        ret                         # return to caller
 *     │
 *   000000000000003a <.L6>:
 *     │  sqrtf error handler — reached when d < 0.
 *     │  Saves caller-saved registers (a0-a4) to stack, calls sqrtf
 *     │  (which handles the NaN/error), restores registers, jumps
 *     │  back to .L2 to continue with the (NaN) scale value.
 *     │
 *     3a:   f43a        sd          a4,40(sp)       # save a4 (V)
 *     3c:   f036        sd          a3,32(sp)       # save a3 (K)
 *     3e:   ec32        sd          a2,24(sp)       # save a2 (Q)
 *     40:   e82e        sd          a1,16(sp)       # save a1 (d)
 *     42:   e42a        sd          a0,8(sp)        # save a0 (n)
 *     44:   00000097    auipc       ra,0x0          # call sqrtf
 *     48:   000080e7    jalr        ra              #   (reloc filled by linker)
 *     4c:   7722        ld          a4,40(sp)       # restore a4
 *     4e:   7682        ld          a3,32(sp)       # restore a3
 *     50:   6662        ld          a2,24(sp)       # restore a2
 *     52:   65c2        ld          a1,16(sp)       # restore a1
 *     54:   6522        ld          a0,8(sp)        # restore a0
 *     56:   b7d1        j           1a <.L2>        # jump back to main path
 *
 * All 4 loop nests (matmul, scale, softmax, matmul) are gone.
 * The compiler builds dims{rows,cols,seq_len,d_model} and qkv{Q,K,V}
 * structs on the stack, then emits the single attn instruction.
 *
 * Labels explained:
 *   .L2 — main path (after sqrtf guard passes)
 *   .L1 — function return (epilogue)
 *   .L6 — sqrtf error handler (d < 0 edge case)
 *   None of these are loops. All loop code is eliminated.
 */

void attention(int n, int d,
               float *Q, float *K, float *V, float *out)
{
    float scores[64*64];
    float scale = 1.0f / __builtin_sqrtf((float)d);
    int i, j, k;

    /*
     * Stage 1: Q × K^T (matrix multiply)
     * score[i][j] = sum_k( Q[i][k] * K[j][k] )
     *
     * Detected by: is_matmul_pattern() — triple-nested loop (i, j, k)
     * with MULT_EXPR + PLUS_EXPR accumulation into a 2D array.
     * Inner loop has: sum += A[i][k] * B[j][k]
     */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            float sum = 0.0f;
            for (k = 0; k < d; k++)
                sum += Q[i*d + k] * K[j*d + k];
            scores[i*n + j] = sum;
        }

    /*
     * Stage 2: Scale by 1/√d_k
     * score[i][j] *= 1/sqrt(d)
     *
     * Detected by: is_elementwise_div() — double-nested loop (i, j)
     * with MULT_EXPR by a loop-invariant scalar (the scale factor).
     */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            scores[i*n + j] *= scale;

    /*
     * Stage 3: Softmax per row
     * score[i][j] = exp(score[i][j]) / sum_j(exp(score[i][j]))
     *
     * Detected by: is_softmax_pattern() — outer loop (i) with 2-3
     * child loops containing __builtin_expf() calls and RDIV_EXPR
     * normalization. Handles both 2-child (exp+sum combined, then
     * normalize) and 3-child (max-reduce, exp+sum, normalize) forms.
     */
    for (i = 0; i < n; i++) {
        float row_sum = 0.0f;
        for (j = 0; j < n; j++) {
            scores[i*n + j] = __builtin_expf(scores[i*n + j]);
            row_sum += scores[i*n + j];
        }
        for (j = 0; j < n; j++)
            scores[i*n + j] /= row_sum;
    }

    /*
     * Stage 4: Scores × V (matrix multiply)
     * out[i][j] = sum_k( score[i][k] * V[k][j] )
     *
     * Detected by: is_matmul_pattern() — same triple-nested pattern
     * as Stage 1, but writing to the output array.
     */
    for (i = 0; i < n; i++)
        for (j = 0; j < d; j++) {
            float sum = 0.0f;
            for (k = 0; k < n; k++)
                sum += scores[i*n + k] * V[k*d + j];
            out[i*d + j] = sum;
        }
}
