global setjmp
global longjmp

section .text

setjmp:
        mov     eax, [esp+4]
        mov     [eax+0], ebx
        mov     [eax+4], esi
        mov     [eax+8], edi
        mov     [eax+12], ebp
        lea     ecx, [esp+4]
        mov     [eax+16], ecx
        mov     ecx, [esp]
        mov     [eax+20], ecx
        xor     eax, eax
        ret

longjmp:
        mov     eax, [esp+4]
        mov     ecx, [esp+8]
        test    ecx, ecx
        jnz     .have_val
        mov     ecx, 1
.have_val:
        mov     ebx, [eax+0]
        mov     esi, [eax+4]
        mov     edi, [eax+8]
        mov     ebp, [eax+12]
        mov     edx, [eax+20]
        mov     esp, [eax+16] 
        mov     eax, ecx
        jmp     edx
