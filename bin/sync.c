/* ============================================================
 * TANJA OS BIN COMMAND - sync
 * ------------------------------------------------------------
 * A standalone ELF32 user program, compiled by the Makefile with
 * plain gcc -m32 (freestanding) and embedded in the kernel image
 * (via home.tar) as a /bin command at boot.  Replace or add commands by editing
 * or dropping files into bin/.
 * ============================================================ */

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

