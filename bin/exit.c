#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* exit - leave the shell (logs the session out). */
void main(char* args) {
    (void)args;
    acc_shell_exit();
}
