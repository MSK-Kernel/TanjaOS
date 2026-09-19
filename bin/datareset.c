/* ============================================================
 * TANJA OS BIN COMMAND - datareset
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

#define MAX_RESPONSE 8


void main(char* args) {
    (void)args;

    char response[MAX_RESPONSE];

    print("Are you sure you want to reset all data? [Y/n]: ");
    read_line(response, MAX_RESPONSE);

    if (!((response[0] == 'Y' || response[0] == 'y') && response[1] == 0) &&
        !((response[0] == 'N' || response[0] == 'n') && response[1] == 0)) {
        print("Invalid option. Please enter Y or N.\n");
        return;
    }

    if (response[0] == 'N' || response[0] == 'n') {
        print("Data reset cancelled.\n");
        return;
    }

    // Persist the reset state BEFORE starting setup. If setup is interrupted
    // by reboot/poweroff, the next boot will see is_setup == 0 and launch
    // the setup wizard just like a fresh installation.
    print("Wiping data...\n");
    config_reset();
    print("Applying settings...\n");
    fs_init();
    fs_seed_home();
    store_save();
    setup_wizard();

    // setup_wizard() only sets is_setup after all three setup prompts finish.
    // Only then do we persist the newly-created account.
    store_save();
}

