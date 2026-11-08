@ MC/PS kernel: start-up, exception handlers, and starting a program.
@ Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
@
@ Written from the public PocketStation documentation. Kernel RAM (0..1FFh) is laid out as the
@ apps expect it: the vectors at 00h jump through the handler addresses at 20h; the IRQ stack
@ tops at 180h and the FIQ stack at 200h; ComFlags at 0C0h, the alarm setting at 0D8h, the SWI
@ table pointer at 0E0h, the callbacks at 0F8h. Ours: our variables from 040h, and the SWI
@ handler's stack above them
@ (060h..0BFh, top 0C0h). Calls never touch the program's own RAM or stack (MGS Integral reads
@ what's left below its stack pointer and expects it untouched).

        .syntax unified
        .arm
        .section .vectors, "ax"
        .global _vectors
_vectors:
        ldr     pc, =reset          @ power-on: address 0 is this ROM until the remap
        ldr     pc, =undef_entry
        ldr     pc, =swi_entry
        ldr     pc, =pabort_entry
        ldr     pc, =dabort_entry
        ldr     pc, =reset
        ldr     pc, =irq_entry
        ldr     pc, =fiq_entry
        .ltorg

        .text
        .global reset
reset:
        msr     cpsr_c, #0xD3       @ SVC, IRQ and FIQ off
        mov     r0, #0x06000000
        mov     r1, #1              @ F_CTRL: RAM at address 0 from now on
        str     r1, [r0]
        @ The vectors in RAM: each LDR PC,[PC,#18h], through the addresses at 20h.
        mov     r0, #0
        ldr     r1, =0xE59FF018
        mov     r2, #8
1:      str     r1, [r0], #4
        subs    r2, r2, #1
        bne     1b
        adr     r1, handler_addrs
        mov     r2, #8
2:      ldr     r3, [r1], #4
        str     r3, [r0], #4
        subs    r2, r2, #1
        bne     2b
        @ Stacks
        msr     cpsr_c, #0xD2       @ IRQ
        mov     sp, #0x180
        msr     cpsr_c, #0xD1       @ FIQ
        mov     sp, #0x200
        msr     cpsr_c, #0xD7       @ abort
        mov     sp, #0xC0
        msr     cpsr_c, #0xDB       @ undefined
        mov     sp, #0xC0
        msr     cpsr_c, #0xDF       @ system (the user stack)
        mov     sp, #0x800
        msr     cpsr_c, #0xD3       @ SVC: boots on the program RAM (nothing there yet)
        mov     sp, #0x800
        bl      kernel_boot         @ doesn't return: starts the menu

handler_addrs:
        .word   reset, undef_entry, swi_entry, pabort_entry, dabort_entry, reset, irq_entry, fiq_entry

@ --- IRQ: the program's callback first (it acknowledges its own), then the kernel's work.
@ The callback runs in IRQ mode with r0-r3, r12 and lr saved for it, and finds r2-r12 as the
@ interrupted code left them (games keep a base address in r12 and their handlers use it).
        .global irq_entry
irq_entry:
        sub     lr, lr, #4
        stmfd   sp!, {r0-r3, r12, lr}
        mov     r0, #0xFC           @ the IRQ callback
        ldr     r0, [r0]
        cmp     r0, #0
        movne   lr, pc
        bxne    r0
        bl      kernel_irq
        ldmfd   sp!, {r0-r3, r12, pc}^

@ --- FIQ: the program's callback, or the kernel's card port and timer 2 handling.
        .global fiq_entry
fiq_entry:
        sub     lr, lr, #4
        stmfd   sp!, {r0-r3, r12, lr}
        mov     r0, #0x100          @ the FIQ callback
        ldr     r0, [r0]
        cmp     r0, #0
        movne   lr, pc
        bxne    r0
        bl      kernel_fiq
        ldmfd   sp!, {r0-r3, r12, pc}^

@ --- Exceptions a program shouldn't cause: back to the menu.
undef_entry:
pabort_entry:
dabort_entry:
        msr     cpsr_c, #0xD3
        mov     sp, #0xC0
        mov     r0, #0
        bl      kernel_crash

@ --- SWI. Its address ends in 00h: some games write a zero byte over the low byte of the
@ SWI handler's address (at 028h), harmless only then.
        .balign 256
        .global swi_entry
swi_entry:
        stmfd   sp!, {r1-r3, r12, lr}
        mrs     r12, spsr
        stmfd   sp!, {r12}
        tst     r12, #0x20          @ called from Thumb?
        ldrhne  r12, [lr, #-2]
        andne   r12, r12, #0xFF
        ldreq   r12, [lr, #-4]
        andeq   r12, r12, #0xFF     @ SWI 100h and up mirror 00h..FFh
        cmp     r12, #0x18
        bhi     swi_done            @ 19h and up: nothing
        ldr     lr, =swi_table
        ldr     r12, [lr, r12, lsl #2]
        mov     lr, pc              @ in SVC mode, IRQs off, on the kernel's stack
        bx      r12
swi_done:
        ldmfd   sp!, {r12}
        msr     spsr_cxsf, r12
        ldmfd   sp!, {r1-r3, r12, pc}^

@ --- Starts a program: r0 = entry (bit 0 Thumb), r1 = parameter. Fresh stacks, user mode,
@ interrupts on, r0 = parameter, sp = 800h, nothing to return to.
        .global launch
launch:
        msr     cpsr_c, #0xD2
        mov     sp, #0x180
        msr     cpsr_c, #0xD1
        mov     sp, #0x200
        msr     cpsr_c, #0xDF
        mov     sp, #0x800
        mov     lr, #0
        msr     cpsr_c, #0xD3
        mov     sp, #0xC0
        mov     r2, #0x200          @ the program's RAM starts cleared
        mov     r3, #0
3:      str     r3, [r2], #4
        cmp     r2, #0x800
        bne     3b
        bic     lr, r0, #1
        mov     r2, #0x10           @ user mode, IRQ and FIQ on
        tst     r0, #1
        orrne   r2, r2, #0x20       @ Thumb
        msr     spsr_cxsf, r2
        mov     r0, r1
        mov     r1, #0
        mov     r2, #0
        mov     r3, #0
        mov     r4, #0
        mov     r5, #0
        mov     r6, #0
        mov     r7, #0
        mov     r8, #0
        mov     r9, #0
        mov     r10, #0
        mov     r11, #0
        mov     r12, #0
        movs    pc, lr

@ --- CustomSwi2: the program's own privileged routine (callback[0]), with r0-r6, r8-r10.
@ Runs from swi_entry in System mode; the routine returns with bx lr.
        .global swi_custom2
swi_custom2:
        mov     r12, #0xF8          @ the SWI2 callback
        ldr     r12, [r12]
        cmp     r12, #0
        beq     swi_reset
        bx      r12

@ --- Reset: from the top, the way the reset button does.
        .global swi_reset
swi_reset:
        msr     cpsr_c, #0xD3
        mov     r0, #1
        bl      kernel_reset_clock
        ldr     pc, =reset
        .ltorg
