; ============================================================
; TanjaOS - Linux compat assembly glue
; ------------------------------------------------------------
; linux_int80_stub : IDT gate 0x80 -> linux_syscall (C)
; linux_enter      : switch to the guest stack, jump to e_entry
; linux_exit_restore: unwind exit() back into linux_run
;
; The guest runs in ring 0 like every TanjaOS program, so int
; 0x80 needs no privilege switch - just pusha, dispatch, popa,
; iretd.  The stack switch in linux_enter saves the kernel stack
; pointer; exit() restores it, which "returns" linux_enter's call
; frame and unwinds straight back into linux_run().
; ============================================================

section .data
kernel_esp_save: dd 0

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
    iretd

; void linux_enter(uint32_t entry [esp+4], uint32_t sp [esp+8])
linux_enter:
    mov  [kernel_esp_save], esp
    mov  eax, [esp+4]
    mov  esp, [esp+8]
    jmp  eax                    ; never returns to this frame

; void linux_exit_restore(void) - noreturn
linux_exit_restore:
    mov  esp, [kernel_esp_save]
    ret                         ; back into linux_run's caller frame
