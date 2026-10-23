#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* /bin/doom.o - makes Doom show up in /bin like every other command.
 *
 * Doom itself (ELF + IWAD) is linked straight into the kernel image
 * (see kernel/doom.c); it is far too big for a 320 KiB filesystem slot.
 * `doom` is a registered built-in, so execute_command("doom ...") always
 * resolves to the built-in and never back to this file. */
void main(char* args) {
    char line[300];
    int n = 0;

    if (!cmd_exists("doom")) {          /* guard against self-recursion */
        print("doom: not built into this kernel - rebuild with 'make doom'\n");
        return;
    }

    line[n++] = 'd'; line[n++] = 'o'; line[n++] = 'o'; line[n++] = 'm';
    if (args && *args) {
        line[n++] = ' ';
        while (*args && n < (int)sizeof(line) - 1) line[n++] = *args++;
    }
    line[n] = 0;
    execute_command(line);
}
