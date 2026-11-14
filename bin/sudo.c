#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* account password buffer (kernel PW_LEN) */
#define PW_LEN 32

/* sudo <command> - run one command as root.
 *
 * The sudoers gate: while a "wheel" group exists AND has members, only
 * they may sudo (root always may). With no wheel group, every user may,
 * as before. The password asked for is the INVOKING user's own, verified
 * against their own account, with three tries - real sudo rules. */
void main(char* args) {
    int w, self, tries;
    if (!args || !args[0]) { print("usage: sudo <command>\n"); return; }
    if (acc_cur_uid() == 0) { execute_command(args); return; }

    w = acc_group_find("wheel");
    if (w >= 0 && acc_group_member_count(w) > 0 &&
        !acc_group_is_member(w, acc_cur_name())) {
        print(acc_cur_name());
        print(" is not in the sudoers file.  This incident will be reported.\n");
        return;
    }

    self = acc_user_find(acc_cur_name());
    if (self < 0) { print("sudo: unknown account\n"); return; }

    for (tries = 0; tries < 3; tries++) {
        char pw[PW_LEN];
        print("[sudo] password for ");
        print(acc_cur_name());
        print(": ");
        read_line_secret(pw, PW_LEN);
        if (pw[0] == 0) { print("sudo: no password was provided\n"); return; }
        if (acc_user_pass(self)[0] && streq(pw, acc_user_pass(self))) {
            int saved = fs_cur_uid;
            fs_cur_uid = 0;
            execute_command(args);
            fs_cur_uid = saved;
            return;
        }
        if (tries < 2) print("Sorry, try again.\n");
    }
    print("sudo: 3 incorrect password attempts\n");
}
