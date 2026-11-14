section .multiboot
align 4
    dd 0x1BADB002
    dd 0x03
    dd -(0x1BADB002 + 0x03)

section .text
global _start
extern kernel_main

_start:
    cli

    lgdt [gdt_ptr]
    jmp 0x08:gdt_ready

gdt_ready:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
    cli
    hlt

section .data
align 16
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:
gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .bss
align 16
stack_bottom: resb 65536
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
