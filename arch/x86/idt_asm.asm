section .text
global irq0_stub
extern timer_handler

irq0_stub:
    pusha
    call timer_handler
    popa
    iret
