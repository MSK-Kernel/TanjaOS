#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

void main(char* args) {
    (void)args;

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

