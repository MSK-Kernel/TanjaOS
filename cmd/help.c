#include "cmd.h"

void cmd_help(char* args) {
    (void)args;
    extern void list_commands(void);
    list_commands();
}
