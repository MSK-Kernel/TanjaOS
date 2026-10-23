kernel_esp_save: dd 0
kernel_ebp_save: dd 0
kernel_ebx_save: dd 0
kernel_esi_save: dd 0
kernel_edi_save: dd 0

section .text
global linux_int80_stub
global linux_enter
global linux_exit_restore
extern linux_syscall

linux_int80_stub:
    pusha
    push esp                    ; &regs frame (pusha order)
    call linux_syscall
    add esp, 4
    popa                        ; eax slot carries the result
    iret

; void linux_enter(uint32_t entry [esp+4], uint32_t sp [esp+8])
; Switch to the program stack and jump to the entry point, never
; returning.  The callee-saved registers of the interrupted kernel
; frame are stashed so linux_exit_restore can resume that frame
; exactly as if linux_enter had returned normally.
;
; The program is entered with a clean Linux-style register state:
; esp = the prepared initial stack, eax = entry, and every other
; general-purpose register zeroed.  This matters: glibc's _start
; passes %edx to __libc_start_main as rtld_fini, and a leftover
; kernel address there gets registered via atexit() and called at
; exit() time.  %edx MUST be 0 for static binaries.
linux_enter:
    mov  [kernel_esp_save], esp
    mov  [kernel_ebp_save], ebp
    mov  [kernel_ebx_save], ebx
    mov  [kernel_esi_save], esi
    mov  [kernel_edi_save], edi
    mov  eax, [esp+4]
    mov  esp, [esp+8]
    xor  ecx, ecx               ; Linux leaves no meaningful state here
    xor  edx, edx               ; rtld_fini = NULL (static ELF: no ld.so)
    xor  ebx, ebx
    xor  esi, esi
    xor  edi, edi
    xor  ebp, ebp               ; _start expects ebp == 0
    cld                         ; direction flag clear per ABI
    jmp  eax                    ; never returns to this frame

; void linux_exit_restore(void) - noreturn
; Resume the kernel frame that called linux_enter: restore the
; callee-saved registers it expects to still hold, put the kernel
; stack back and "return" from linux_enter into linux_run's caller.
linux_exit_restore:
    mov  ebx, [kernel_ebx_save]
    mov  esi, [kernel_esi_save]
    mov  edi, [kernel_edi_save]
    mov  ebp, [kernel_ebp_save]
    mov  esp, [kernel_esp_save]
    sti                         ; the int 0x80 gate cleared IF
    ret                         ; back into linux_run's caller frame
