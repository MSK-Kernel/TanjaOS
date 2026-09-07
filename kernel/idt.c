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

extern void timer_handler(void);
extern uint32_t boot_ticks;

extern uint32_t exc_stub_table[32];

extern void print(const char* s);

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

extern void irq0_stub(void);

uint32_t get_uptime_ms(void) {
    return boot_ticks;
}

static void fault_print_hex(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;
    for (i = 7; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
    buf[8] = 0;
    int s = 0;
    while (s < 7 && buf[s] == '0') s++;
    print(buf + s);
}

// Called from exception_common in idt_asm.asm. A fault must be a
// visible halt, not a silent triple-fault reboot.
void fault_handler(uint32_t vector) {
    asm volatile ("cli");
    print("\n\nKERNEL FAULT #");
    fault_print_hex(vector);
    print(" - system halted\n");
    for (;;) asm volatile ("hlt");
}

static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFE); // mask all master IRQs except IRQ0 (timer)
    outb(0xA1, 0xFF); // mask all slave IRQs
}

void idt_init(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)&idt;

    int i;
    for (i = 0; i < IDT_ENTRIES; i++) idt_set_gate(i, 0, 0, 0); // not present

    uint16_t cur_cs;
    asm volatile ("mov %%cs, %0" : "=r"(cur_cs));

    // Install a handler for every CPU exception vector (0x00-0x1F).
    for (i = 0; i < 32; i++)
        idt_set_gate(i, (uint32_t)exc_stub_table[i], cur_cs, 0x8E);

    idt_set_gate(0x20, (uint32_t)irq0_stub, cur_cs, 0x8E);

    pic_remap();

    asm volatile ("lidt %0" : : "m"(idt_ptr));
    asm volatile ("sti");
}
