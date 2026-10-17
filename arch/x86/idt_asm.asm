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
;
; The common handler then passes a uniform record to the C handler:
;   vector, error code (0 for vectors that push none), eip, cs,
;   eflags, and a pointer to the pusha register block.  This is what
;   makes a fault on real hardware diagnosable from a photo: the
;   faulting instruction address and the register state are on screen.
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
    ; error code: vectors 8 and 10..14 push one, everything else not
    mov ecx, 0
    mov ebx, eax
    cmp ebx, 8
    je .got_ec
    cmp ebx, 10
    jb .no_ec
    cmp ebx, 14
    jbe .got_ec
.no_ec:
    mov edx, [esp+36]        ; eip
    mov esi, [esp+40]        ; cs
    mov edi, [esp+44]        ; eflags
    jmp .push_args
.got_ec:
    mov ecx, [esp+36]        ; error code
    mov edx, [esp+40]        ; eip
    mov esi, [esp+44]        ; cs
    mov edi, [esp+48]        ; eflags
.push_args:
    push esp                 ; -> pusha register block
    push edi                 ; eflags
    push esi                 ; cs
    push edx                 ; eip
    push ecx                 ; error code
    push eax                 ; vector
    call fault_handler
    add esp, 24
    popa
    add esp, 4              ; pop the pushed vector
    iret
