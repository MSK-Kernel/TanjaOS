#include <stdint.h>
#include "../include/idt.h"

typedef struct __attribute__((packed)) {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

#define IDT_ENTRIES 256

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t idt_ptr;

// kernel.c already has timer_handler() (increments boot_ticks, sends
// EOI) sitting unused - looks like an earlier attempt at this that
// never got wired to an actual IDT. Reusing it here instead of adding
// a duplicate counter.
extern void timer_handler(void);
extern uint32_t boot_ticks;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void idt_set_gate(int n, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[n].base_low  = (uint16_t)(handler & 0xFFFF);
    idt[n].base_high = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].sel       = sel;
    idt[n].always0   = 0;
    idt[n].flags     = flags;
}

// Defined in idt_asm.asm: saves registers, calls timer_handler(),
// restores registers, iret. timer_handler() itself sends the EOI.
extern void irq0_stub(void);

uint32_t get_uptime_ms(void) {
    return boot_ticks;
}

// Standard PIC remap: IRQ0-7 -> vectors 0x20-0x27, IRQ8-15 -> 0x28-0x2F
// (moving them off the CPU exception vectors 0x00-0x1F where they sit
// by default). Only IRQ0 (timer) is left unmasked - everything else,
// including the keyboard (still handled by polling elsewhere in this
// kernel, not IRQ1), stays masked so nothing fires without a handler.
static void pic_remap(void) {
    outb(0x20, 0x11); // ICW1: init, edge triggered, cascade mode, ICW4 needed
    outb(0xA0, 0x11);
    outb(0x21, 0x20); // ICW2: master IRQs start at vector 0x20
    outb(0xA1, 0x28); // ICW2: slave IRQs start at vector 0x28
    outb(0x21, 0x04); // ICW3: tell master there's a slave at IRQ2
    outb(0xA1, 0x02); // ICW3: tell slave its cascade identity
    outb(0x21, 0x01); // ICW4: 8086 mode
    outb(0xA1, 0x01);
    outb(0x21, 0xFE); // mask all master IRQs except IRQ0 (timer)
    outb(0xA1, 0xFF); // mask all slave IRQs
}

void idt_init(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)&idt;

    int i;
    for (i = 0; i < IDT_ENTRIES; i++) idt_set_gate(i, 0, 0, 0); // not present

    // Vector 0x20 = IRQ0 after remap. This kernel doesn't install its
    // own GDT, so it depends on whatever flat code segment the
    // bootloader set up - and that's NOT guaranteed to be the same
    // value across bootloaders. GRUB uses 0x10 here; QEMU's own
    // built-in direct -kernel multiboot loader (bypassing GRUB
    // entirely) sets up a different GDT and uses a different selector.
    // Reading %cs at runtime instead of hardcoding either one makes
    // this work under any multiboot-compliant loader.
    uint16_t cur_cs;
    asm volatile ("mov %%cs, %0" : "=r"(cur_cs));

    // 0x8E = present, ring 0, 32-bit interrupt gate.
    idt_set_gate(0x20, (uint32_t)irq0_stub, cur_cs, 0x8E);

    pic_remap();

    asm volatile ("lidt %0" : : "m"(idt_ptr));
    asm volatile ("sti");
}
