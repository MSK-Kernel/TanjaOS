#include "bin.h"

void cmd_poweroff(char* args) {
    (void)args;
    extern void print(const char* s);
    extern void outw(uint16_t port, uint16_t val);
    print("Powering off...\n");
    for (volatile int i = 0; i < 1000000; i++);

    /* There's no real ACPI table parsing in this kernel yet, so a
     * genuine hardware shutdown (finding the FADT, writing the
     * SLP_TYPa/SLP_EN bits into PM1a_CNT_BLK) isn't available. What
     * *is* available, and what every hobby OS reaches for first, is
     * the small set of well-known "magic port" shutdown backdoors
     * that QEMU, Bochs, and VirtualBox expose for exactly this case.
     * Try each in turn - the ones that don't apply to the current
     * environment are simply ignored by real/emulated hardware. */
    outw(0x604, 0x2000);  /* QEMU (PIIX4/ICH9 "isa-debug-exit"-style ACPI shutdown) */
    outw(0xB004, 0x2000); /* Older QEMU / Bochs */
    outw(0x4004, 0x3400); /* VirtualBox */

    /* If none of those did anything (most likely: running on real
     * hardware), there's nothing left to try without real ACPI
     * support. Halt the CPU so it's at least safe to switch off by
     * hand, rather than leaving the shell running. */
    print("Shutdown port had no effect on this machine - halting.\n");
    print("It is now safe to turn off your computer.\n");
    asm volatile ("cli");
    while (1) asm volatile ("hlt");
}
