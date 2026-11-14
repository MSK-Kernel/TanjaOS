#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* hostname [name] - show or set the system hostname. Setting it needs
 * root, which the filesystem layer already enforces on the config. */
void main(char* args) {
    if (args && args[0]) {
        if (acc_cur_uid() != 0) {
            print("hostname: you must be root to change the hostname\n");
            return;
        }
        acc_set_hostname(args);
        print("Hostname updated\n");
    } else {
        print(acc_hostname());
        print("\n");
    }
}
