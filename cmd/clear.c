#include "cmd.h"

void cmd_clear(char* args) {
    (void)args;
    extern void clear_screen(void);
    clear_screen();
}
