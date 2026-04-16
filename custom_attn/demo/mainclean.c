/*
 * mainclean.c — No-wrapper version of the attn demo
 *
 * THIS FILE IS FOR DEMONSTRATION ONLY.
 * It compiles and the attn instruction appears in objdump,
 * but it will produce WRONG RESULTS on real hardware at -O2.
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │              THE REORDERING PROBLEM                      │
 * ├─────────────────────────────────────────────────────────┤
 * │                                                         │
 * │  What you wrote:           What GCC executes at -O2:    │
 * │  ─────────────────         ─────────────────────────    │
 * │  1. Q[4] = {...}           1. a5 = &dims (address)      │
 * │  2. K[4] = {...}           2. a4 = &qkv  (address)      │
 * │  3. V[4] = {...}           3. attn a5,a5,a4  ← HERE!    │
 * │  4. dims = {2,2,4,8}      4. store result               │
 * │  5. qkv = {Q,K,V}         5. write Q to stack ← late!  │
 * │  6. attn(&dims,&qkv)      6. write K to stack           │
 * │  7. store result           7. write dims to stack        │
 * │                            8. write qkv pointers         │
 * │                            9. return 0                   │
 * │                                                         │
 * │  GCC moved attn to step 3 — before the struct data      │
 * │  is written to memory. The instruction reads garbage.    │
 * └─────────────────────────────────────────────────────────┘
 *
 * WHY does GCC do this?
 *
 *   GCC sees __builtin_riscv_attn as:
 *     unsigned long f(unsigned long x, unsigned long y) -> unsigned long
 *
 *   Two integers in, one integer out. Pure register operation.
 *   GCC has NO IDEA that x and y are pointers the hardware
 *   will dereference. Since the address values (sp+56, sp+72)
 *   are available early, GCC moves attn up for pipeline efficiency.
 *   This is valid for real ALU ops like add/mul, but attn secretly
 *   reads memory through those pointers.
 *
 * WHAT HAPPENS ON REAL HARDWARE?
 *
 *   attn reads from &dims and &qkv on the stack, but those addresses
 *   contain whatever was there from a previous call — uninitialized
 *   garbage. No crash, no error, just silently wrong output.
 *   The worst kind of bug.
 *
 * THE FIX — noinline wrapper (see main.c):
 *
 *   __attribute__((noinline))
 *   unsigned long run_attention(unsigned long a, unsigned long b) {
 *       return __builtin_riscv_attn(a, b);
 *   }
 *
 *   At a function call boundary, the calling convention GUARANTEES:
 *     1. All memory writes before call are committed
 *     2. Arguments are in a0/a1
 *     3. Callee sees consistent memory
 *   So attn always runs AFTER struct data is in memory.
 *
 * ALTERNATIVES:
 *
 *   1. asm volatile("" ::: "memory") barrier before the builtin
 *      — works but fragile, easy to forget
 *
 *   2. Mark the builtin as memory-aware in riscv.md
 *      — proper fix but complex toolchain work
 *
 *   3. Compile with -O0 (no optimization)
 *      — works but produces bloated unoptimized code
 *
 * SUMMARY:
 *   ┌────────────────────────┬──────────┬───────────┬───────────────┐
 *   │ Version                │ Compiles │ In objdump│ Correct (HW)  │
 *   ├────────────────────────┼──────────┼───────────┼───────────────┤
 *   │ No wrapper, -O2        │ Yes      │ Yes       │ NO (reordered)│
 *   │ No wrapper, -O0        │ Yes      │ Yes       │ Yes (slow)    │
 *   │ No wrapper + asm fence │ Yes      │ Yes       │ Yes           │
 *   │ noinline wrapper, -O2  │ Yes      │ Yes       │ Yes (best)    │
 *   └────────────────────────┴──────────┴───────────┴───────────────┘
 *
 * Compile:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64imac -mabi=lp64 \
 *       -ffreestanding -nostdinc -c mainclean.c -o mainclean.o
 *
 * Disassemble:
 *   riscv64-unknown-elf-objdump -d mainclean.o
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

int main(void)
{
    float Q[4] = {0.10f, 0.20f, 0.30f, 0.40f};
    float K[4] = {0.50f, 0.60f, 0.70f, 0.80f};
    float V[4] = {0.90f, 1.00f, 1.10f, 1.20f};

    attn_dims_t dims = {2, 2, 4, 8};
    attn_qkv_t qkv = {Q, K, V};

    volatile unsigned long result = __builtin_riscv_attn(
        (unsigned long)&dims,
        (unsigned long)&qkv
    );

    (void)result;
    return 0;
}
