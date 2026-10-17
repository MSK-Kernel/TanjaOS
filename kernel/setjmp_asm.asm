; setjmp_asm.asm - i386 setjmp/longjmp for the kernel libc.
; jmp_buf layout (int array, byte offsets):
;   0: ebx   4: esi   8: edi   12: ebp   16: esp   20: return address
global setjmp
global longjmp

section .text

; int setjmp(jmp_buf env)   -- cdecl: [esp]=ret, [esp+4]=env
setjmp:
        mov     eax, [esp+4]          ; env pointer
        mov     [eax+0], ebx
        mov     [eax+4], esi
        mov     [eax+8], edi
        mov     [eax+12], ebp
        lea     ecx, [esp+4]           ; caller esp right after the call
        mov     [eax+16], ecx
        mov     ecx, [esp]             ; return address
        mov     [eax+20], ecx
        xor     eax, eax
        ret

; void longjmp(jmp_buf env, int val)  -- cdecl, never returns
longjmp:
        mov     eax, [esp+4]           ; env
        mov     ecx, [esp+8]           ; val
        test    ecx, ecx
        jnz     .have_val
        mov     ecx, 1
.have_val:
        mov     ebx, [eax+0]
        mov     esi, [eax+4]
        mov     edi, [eax+8]
        mov     ebp, [eax+12]
        mov     edx, [eax+20]          ; target pc
        mov     esp, [eax+16]         ; switch stack last
        mov     eax, ecx               ; setjmp returns val
        jmp     edx
