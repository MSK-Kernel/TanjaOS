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
extern void linux_int80_stub(void);

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

/* Register block as pushed by pusha (the layout is fixed by the
 * hardware instruction, lowest address first): */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
} fault_regs_t;

extern int tanja_exit_jmp[5];    /* kernel/libc.c, set by elf_run() */
extern int lx_unwind_reason;
extern uint32_t lx_fault_vec, lx_fault_eip, lx_fault_cr2;
extern uint32_t lx_fault_regs[8];
extern int tanja_exit_active;
extern uint8_t __kernel_end;     /* programs load above the kernel */

static const char* fault_name(uint32_t v) {
    switch (v) {
    case 0:  return "divide error";
    case 1:  return "debug";
    case 2:  return "nmi";
    case 3:  return "breakpoint";
    case 4:  return "overflow";
    case 5:  return "bound range exceeded";
    case 6:  return "invalid opcode";
    case 7:  return "no math unit (FPU off?)";
    case 8:  return "double fault";
    case 9:  return "coprocessor segment overrun";
    case 10: return "invalid tss";
    case 11: return "segment not present";
    case 12: return "stack fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "x87 floating-point error";
    case 17: return "alignment check";
    case 18: return "machine check";
    case 19: return "simd floating-point exception";
    default: return "unknown exception";
    }
}

static void fault_line(const char* label, uint32_t v) {
    print(label);
    fault_print_hex(v);
    print("\n");
}

/* Called from exception_common in idt_asm.asm. Two cases:
 *
 *  - the fault happened inside a LOADED PROGRAM (eip above the
 *    kernel image while one is running): terminate that program and
 *    return to the shell.  A bad user program must not take the
 *    whole OS down.
 *  - anything else is a kernel fault: print a full dump (faulting
 *    instruction address, error code, register state, instruction
 *    bytes) and halt.  With eip + bytes, an objdump of the built
 *    kernel names the exact faulting instruction.
 */
void fault_handler(uint32_t vector, uint32_t err, uint32_t eip,
                   uint32_t cs, uint32_t eflags, const fault_regs_t* r) {
    asm volatile ("cli");

    if ((uint32_t)(uintptr_t)&__kernel_end <= eip && tanja_exit_active) {
        print("\nprogram fault: ");
        print(fault_name(vector));
        print(" (eip ");
        fault_print_hex(eip);
        print(") - program terminated\n");
        /* unwind exactly like exit(): back into elf_run, which
         * restores its own state and the program break */
        {
            extern int lx_unwind_reason;
            lx_unwind_reason = 1;
            lx_fault_vec = vector;
            lx_fault_eip = eip;
            lx_fault_cr2 = 0;
            if (vector == 14) {
                uint32_t cr2v;
                asm volatile ("mov %%cr2, %0" : "=r"(cr2v));
                lx_fault_cr2 = cr2v;
            }
            lx_fault_regs[0]=r->eax; lx_fault_regs[1]=r->ecx;
            lx_fault_regs[2]=r->edx; lx_fault_regs[3]=r->ebx;
            lx_fault_regs[4]=r->esp; lx_fault_regs[5]=r->ebp;
            lx_fault_regs[6]=r->esi; lx_fault_regs[7]=r->edi;
        }
        asm volatile ("sti");
        __builtin_longjmp((void**)tanja_exit_jmp, 1);
    }

    print("\n\nKERNEL FAULT #");
    fault_print_hex(vector);
    print(" - ");
    print(fault_name(vector));
    print("\n");
    fault_line("eip     = 0x", eip);
    fault_line("cs      = 0x", cs);
    fault_line("eflags  = 0x", eflags);
    fault_line("err     = 0x", err);
    if (vector == 14) {
        uint32_t cr2;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        fault_line("cr2     = 0x", cr2);
    }
    fault_line("eax     = 0x", r->eax);
    fault_line("ecx     = 0x", r->ecx);
    fault_line("edx     = 0x", r->edx);
    fault_line("ebx     = 0x", r->ebx);
    fault_line("esp     = 0x", r->esp);
    fault_line("ebp     = 0x", r->ebp);
    fault_line("esi     = 0x", r->esi);
    fault_line("edi     = 0x", r->edi);
#if 0  /* full extras (stack frame, GOT.PLT, bytes, syscall tail) -
         * disabled while debugging: keep the fault compact so the
         * pre-fault debug prints stay on the VGA screen */
    fault_line("ebx     = 0x", r->ebx);
    {
        /* the caller: [esp] holds the return address of the faulting
         * call (this OS runs programs in kernel mode, so esp is the
         * program's own stack even for a "kernel" fault) */
        int k;
        for (k = 2; k < 8; k++) {
            uint32_t v = *(volatile const uint32_t*)(r->esp + 4*k);
            print("  stk[");
            fault_print_hex(k);
            print("] = 0x");
            fault_print_hex(v);
            print("\n");
        }
    }
    {
        /* GOT.PLT dump: ebx is the PIC register pointing at .got.plt
         * (set by _start before any PLT call) - GOT[1] link_map,
         * GOT[2] _dl_runtime_resolve tell us if lazy binding works */
        int k;
        for (k = 0; k < 8; k++) {
            uint32_t v = *(volatile const uint32_t*)(r->ebx + 4*k);
            print("  got[");
            fault_print_hex(k);
            print("] = 0x");
            fault_print_hex(v);
            print("\n");
        }
    }
    print("bytes   =");
    {
        /* up to 8 instruction bytes at the faulting eip */
        int i;
        for (i = 0; i < 8; i++) {
            uint8_t b = *(volatile const uint8_t*)(eip + i);
            print(" ");
            fault_print_hex(b);
        }
    }
    print("\n");
    {
        extern void lx_log_dump(void);
        lx_log_dump();
    }
#endif
    print("\nsystem halted\n");
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

    // Linux syscall gate (compat layer, kernel/linux.c).  Ring 0, so
    // no privilege switch is needed - just a pusha/dispatch/iretd.
    idt_set_gate(0x80, (uint32_t)linux_int80_stub, cur_cs, 0x8E);

    pic_remap();

    asm volatile ("lidt %0" : : "m"(idt_ptr));
    asm volatile ("sti");
}
