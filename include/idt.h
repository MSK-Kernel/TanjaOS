#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Sets up a minimal IDT: PIC remapped to vectors 0x20-0x2F, only IRQ0
// (the PIT timer) unmasked, and a real handler counting elapsed
// milliseconds. Call once, after timer_init() has programmed the PIT
// divisor. Enables interrupts (sti) as its last step.
void idt_init(void);

// Real elapsed milliseconds since idt_init() was called, driven by
// actual PIT interrupts rather than a busy-wait guess.
uint32_t get_uptime_ms(void);

#endif
