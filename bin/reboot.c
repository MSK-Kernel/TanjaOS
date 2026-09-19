/* ============================================================
 * TANJA OS BIN COMMAND - reboot
 * ------------------------------------------------------------
 * A standalone ELF32 user program, compiled by the Makefile with
 * plain gcc -m32 (freestanding) and embedded in the kernel image
 * (via home.tar) as a /bin command at boot.  Replace or add commands by editing
 * or dropping files into bin/.
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

void main(char* args) {
    (void)args;
    print("Rebooting...\n");
    for (volatile int i = 0; i < 1000000; i++);
    while ((inb(0x64) & 0x02) != 0);
    outb(0x64, 0xFE);
    asm volatile ("lidt 0\n" "int $0");
    while (1);
}

