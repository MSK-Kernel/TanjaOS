#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

void main(char* args) {
    (void)args;

    if (!store_is_persistent()) {
        print("sync: no persistent disk found, running from RAM\n");
        return;
    }

    store_save();
    print("Filesystem synced to disk.\n");
}

