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
    ; GRUB leaves the multiboot magic in eax and a pointer to the
    ; multiboot info struct (mods_count/mods_addr etc.) in ebx.
    ; cdecl pushes args right-to-left, so push ebx first, eax second,
    ; giving kernel_main(magic, mbi_addr).
    push ebx
    push eax
    call kernel_main
    cli
    hlt

section .bss
align 16
stack_bottom: resb 16384
stack_top:
