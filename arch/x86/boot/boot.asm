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
stack_bottom: resb 65536
stack_top:
