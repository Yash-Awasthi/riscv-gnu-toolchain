	.file	"test2.c"
	.option nopic
	.attribute arch, "rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_zmmul1p0_zaamo1p0_zalrsc1p0_zca1p0_zcd1p0"
	.attribute unaligned_access, 0
	.attribute stack_align, 16
	.text
	.align	1
	.globl	attention
	.type	attention, @function
attention:
	ble	a4,zero,.L1
 #APP
	.word 0x0200000b
 #NO_APP
.L1:
	ret
	.size	attention, .-attention
	.ident	"GCC: (g5115c7e447-dirty) 15.2.0"
	.section	.note.GNU-stack,"",@progbits
