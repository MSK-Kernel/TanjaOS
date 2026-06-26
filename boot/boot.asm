BITS 32
GLOBAL _start
EXTERN kernel_main

; ================= MULTIBOOT HEADER =================
; Must be in first 8KB of file for GRUB

SECTION .multiboot
ALIGN 4
    DD 0x1BADB002            ; magic
    DD 0x00000003            ; flags (align modules + mem info)
    DD -(0x1BADB002 + 0x00000003) ; checksum

; ================= ENTRY POINT =================
SECTION .text

_start:
    cli                      ; disable interrupts

    mov esp, stack_top      ; set stack

    call kernel_main        ; jump into C kernel

.hang:
    hlt
    jmp .hang

; ================= STACK =================
SECTION .bss
ALIGN 16
stack_bottom:
    resb 16384              ; 16 KB stack
stack_top:
