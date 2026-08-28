#include "bin.h"

// sleep <seconds> - e.g. `sleep 2` or `sleep 0.5`. Delegates to
// timer_delay_ms(), which halts the CPU between PIT ticks instead of
// busy-spinning, so this doesn't peg a core while waiting.
void cmd_sleep(char* args) {
    if (!args) args = "";
    while (*args == ' ') args++;

    if (!*args) {
        print("Usage: sleep <seconds>\n");
        return;
    }

    char* p = args;
    uint32_t whole = 0;

    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (uint32_t)(*p - '0');
        p++;
    }

    uint32_t ms = whole * 1000;

    if (*p == '.') {
        p++;
        uint32_t scale = 100; // tenths, then hundredths, then thousandths
        while (*p >= '0' && *p <= '9' && scale > 0) {
            ms += (uint32_t)(*p - '0') * scale;
            scale /= 10;
            p++;
        }
    }

    timer_delay_ms(ms);
}
