/* mainbuiltin.c — Explicit builtin demo
 *
 * Uses __builtin_riscv_attn() directly to emit the custom attn instruction.
 * This is the "production" path: no pattern matching needed.
 *
 * Compile:
 *   riscv64-unknown-elf-gcc -O2 -march=rv64gc -mabi=lp64d \
 *       -ffreestanding -nostdlib -c mainbuiltin.c -o mainbuiltin.o
 *
 * Verify:
 *   riscv64-unknown-elf-objdump -d mainbuiltin.o
 *
 * Expected output includes:
 *   attn    a0,a0,a1
 */

long run_attention(long dims_addr, long qkv_addr)
{
    return __builtin_riscv_attn(dims_addr, qkv_addr);
}
