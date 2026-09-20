section .multiboot
align 4
    dd 0x1BADB002
    dd 0x03
    dd -(0x1BADB002 + 0x03)

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
    cli
    hlt

section .bss
align 16
; 64 KB kernel stack. The recursive-descent C compiler (cc.c) can
; recurse deep enough to overflow the old 16 KB stack and scribble
; past stack_bottom into the BSS-resident filesystem tables, which
; then makes the next `ls` fault and (with no exception handlers)
; triple-fault-reboot the machine.
stack_bottom: resb 65536
stack_top:
