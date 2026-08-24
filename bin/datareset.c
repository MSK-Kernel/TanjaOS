#include "bin.h"

#define MAX_RESPONSE 8

extern void print(const char* s);
extern void read_line(char* buf, int max);
extern void clear_screen(void);
extern void config_reset(void);
extern void setup_wizard(void);
extern void store_save(void);
extern void fs_init(void);

void cmd_datareset(char* args) {
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
    config_reset();
    fs_init();
    store_save();

    clear_screen();
    setup_wizard();

    // setup_wizard() only sets is_setup after all three setup prompts finish.
    // Only then do we persist the newly-created account.
    store_save();
}
