section .text
global irq0_stub
global exception_common
global exc_stub_table
extern timer_handler
extern fault_handler

irq0_stub:
    pusha
    call timer_handler
    popa
    iret

; Table of per-vector entry points. idt.c installs one per exception
; vector (0..31). Each stub pushes its vector number and jumps to the
; common handler, so fault_handler always finds the vector at a fixed
; stack offset regardless of whether the CPU pushed an error code.
exc_stub_table:
%assign i 0
%rep 32
    dd exc_stub_%[i]
%assign i i+1
%endrep

%assign i 0
%rep 32
exc_stub_%[i]:
    push dword i
    jmp near exception_common
%assign i i+1
%endrep

exception_common:
    pusha
    mov eax, [esp+32]        ; vector number pushed by the stub
    push eax
    call fault_handler
    add esp, 4
    popa
    add esp, 4              ; pop the pushed vector
    iret
