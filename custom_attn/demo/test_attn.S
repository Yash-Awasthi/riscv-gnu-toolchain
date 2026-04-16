# ============================================================
# custom_attn/demo/test_attn.S
#
# Standalone assembly test for the custom 'attn' instruction.
# Uses .insn r directive to encode the R-type custom-0 instruction.
#
# Instruction:  attn rd, rs1, rs2
#   opcode = 0x0b (custom-0)
#   funct3 = 0x0
#   funct7 = 0x00
#
# Assemble:
#   riscv64-unknown-elf-as -march=rv64imac -o test_attn.o test_attn.S
# Disassemble:
#   riscv64-unknown-elf-objdump -d test_attn.o
# ============================================================

.text
.globl run_attention
.type run_attention, @function

# ---------------------------------------------------------------
# run_attention:
#   a0 = address of attn_dims_t  (matrix dimensions struct)
#   a1 = address of attn_qkv_t   (Q/K/V matrix pointers struct)
#   returns a0 = result/status
# ---------------------------------------------------------------
run_attention:
    # Custom attn instruction: attn a0, a0, a1
    # .insn r  opcode, funct3, funct7, rd, rs1, rs2
    .insn r 0x0b, 0, 0, a0, a0, a1
    ret

.globl main
.type main, @function

# ---------------------------------------------------------------
# main: constructs structs on the stack and calls the instruction
# ---------------------------------------------------------------
main:
    addi sp, sp, -80
    sd   ra, 72(sp)

    # --- Set up attn_dims_t on stack (offset 0) ---
    li   t0, 2          # rows = 2
    sw   t0, 0(sp)
    li   t0, 2          # cols = 2
    sw   t0, 4(sp)
    li   t0, 4          # seq_len = 4
    sw   t0, 8(sp)
    li   t0, 8          # d_model = 8
    sw   t0, 12(sp)

    # --- Set up attn_qkv_t on stack (offset 16) ---
    # Q pointer
    la   t0, Q_data
    sd   t0, 16(sp)
    # K pointer
    la   t0, K_data
    sd   t0, 24(sp)
    # V pointer
    la   t0, V_data
    sd   t0, 32(sp)

    # --- Execute custom attn instruction ---
    # a0 = &dims (sp+0), a1 = &qkv (sp+16)
    mv   a0, sp
    addi a1, sp, 16
    .insn r 0x0b, 0, 0, a0, a0, a1

    # return 0
    li   a0, 0
    ld   ra, 72(sp)
    addi sp, sp, 80
    ret

.section .rodata
Q_data: .float 0.10, 0.20, 0.30, 0.40
K_data: .float 0.50, 0.60, 0.70, 0.80
V_data: .float 0.90, 1.00, 1.10, 1.20
