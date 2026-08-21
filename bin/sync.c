#include "bin.h"

void cmd_sync(char* args) {
    (void)args;
    extern void store_save(void);
    extern int store_is_persistent(void);

    if (!store_is_persistent()) {
        print("sync: no persistent disk found, running RAM-only\n");
        return;
    }

    store_save();
    print("Filesystem synced to disk.\n");
}
