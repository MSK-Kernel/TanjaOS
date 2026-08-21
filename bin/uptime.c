#include "bin.h"

void cmd_uptime(char* args) {
    (void)args;
    extern void print(const char* s);
    extern void print_dec(uint32_t n);
    extern void putc(char c);
    extern uint32_t get_uptime_ms(void);

    uint32_t t = get_uptime_ms();
    print("Uptime: ");
    print_dec(t / 1000);
    print(".");
    uint32_t ms = t % 1000;
    if (ms < 100) putc('0');
    if (ms < 10) putc('0');
    print_dec(ms);
    print("s\n");
}
