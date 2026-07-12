#include "cmd.h"

void cmd_reboot(char* args) {
    (void)args;
    extern void print(const char* s);
    extern uint8_t inb(uint16_t port);
    extern void outb(uint16_t port, uint8_t val);
    print("Rebooting...\n");
    for (volatile int i = 0; i < 1000000; i++);
    while ((inb(0x64) & 0x02) != 0);
    outb(0x64, 0xFE);
    asm volatile ("lidt 0\n" "int $0");
    while (1);
}
