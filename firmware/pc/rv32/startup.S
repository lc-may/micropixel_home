// SPDX-License-Identifier: Apache-2.0
.section .text.start,"ax",@progbits
.global _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __stack_top
    la t0, mp_rv_trap
    csrw mtvec, t0
    li t0, 0x6000
    csrs mstatus, t0
    csrw fcsr, zero
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call main
    call mp_rv_exit
.align 2
mp_rv_trap:
    csrr a0, mcause
    csrr a1, mepc
    csrr a2, mtval
    la sp, __stack_top
    call mp_rv_trap_report
3:  wfi
    j 3b
