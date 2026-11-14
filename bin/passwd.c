#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* account password buffer (kernel PW_LEN) */
#define PW_LEN 32

static int next_arg(char** pp, char* out, int max) {
    char* p = *pp; int n = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && n < max - 1) out[n++] = *p++;
    out[n] = 0; *pp = p;
    return n > 0;
}

/* passwd [user] - mirrors util-linux passwd. A blank new password erases
 * the password (this system's passwordless-account feature). */
void main(char* args) {
    char name[64], a[64], *p = args;
    int idx;
    if (!next_arg(&p, a, sizeof a))
        strncpy(name, acc_cur_name(), sizeof name - 1), name[sizeof name - 1] = 0;
    else strncpy(name, a, sizeof name - 1), name[sizeof name - 1] = 0;

    idx = acc_user_find(name);
    if (idx < 0) { print("passwd: user '"); print(name); print("' does not exist\n"); return; }
    if (!streq(name, acc_cur_name()) && acc_cur_uid() != 0) {
        print("passwd: You may not view or modify password information for ");
        print(name); print(".\n");
        return;
    }
    print("Changing password for ");
    print(acc_user_name(idx));
    print(".\n");

    if (acc_cur_uid() != 0 && acc_user_pass(idx)[0]) {
        char cur[PW_LEN];
        print("Current password: ");
        read_line_secret(cur, PW_LEN);
        if (!streq(cur, acc_user_pass(idx))) {
            print("passwd: Authentication token manipulation error\n");
            print("passwd: password unchanged\n");
            return;
        }
    }
    {
        char pw1[PW_LEN], pw2[PW_LEN];
        int attempt;
        for (attempt = 0; attempt < 2; attempt++) {
            print("New password: ");
            read_line_secret(pw1, PW_LEN);
            if (!pw1[0]) {
                acc_user_set_pass(idx, "");
                print("passwd: password erased\n");
                return;
            }
            print("Retype new password: ");
            read_line_secret(pw2, PW_LEN);
            if (streq(pw1, pw2)) {
                acc_user_set_pass(idx, pw1);
                print("passwd: password updated successfully\n");
                return;
            }
            print("Sorry, passwords do not match.\n");
        }
        print("passwd: Authentication token manipulation error\n");
        print("passwd: password unchanged\n");
    }
}
