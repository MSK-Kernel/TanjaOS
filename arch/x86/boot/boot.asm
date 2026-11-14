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

; Use OUR segment layout, not whatever the bootloader left behind.
; Some firmware/GRUB paths (UEFI CSM in particular) hand over with a
; GDT where the code segment is not at 0x08 - the kernel then put that
; inherited selector into every IDT gate, and the first interrupt
; faulted with #GP. Flat 4 GiB segments make the loader irrelevant.
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
    dq 0x0000000000000000        ; 0x00 null
    dq 0x00CF9A000000FFFF        ; 0x08 ring0 code, base 0, 4 GiB
    dq 0x00CF92000000FFFF        ; 0x10 ring0 data, base 0, 4 GiB
gdt_end:
gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .bss
align 16
stack_bottom: resb 65536
stack_top:

; Non-allocating note so ld does not assume an executable stack.
section .note.GNU-stack noalloc noexec nowrite progbits
