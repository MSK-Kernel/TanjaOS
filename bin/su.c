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

/* su [-] [user] - switch the session user. Without a name it goes to
 * root. "-" is the classic login-shell form and behaves the same. root
 * becomes anyone without a password; everyone else must give the TARGET
 * user's password. The change lasts until exit logs out. */
void main(char* args) {
    char name[64], a[64], *p = args;
    int idx, have = 0;
    while (next_arg(&p, a, sizeof a)) {
        if (streq(a, "-") || streq(a, "-l") || streq(a, "-login")) continue;
        if (!have) { strncpy(name, a, sizeof name - 1); name[sizeof name - 1] = 0; have = 1; }
        else { print("usage: su [-] [name]\n"); return; }
    }
    if (!have) { strncpy(name, "root", sizeof name - 1); name[sizeof name - 1] = 0; }
    idx = acc_user_find(name);
    if (idx < 0) { print("su: user "); print(name); print(" does not exist\n"); return; }
    /* an empty target password needs no prompt, like the login screen */
    if (fs_cur_uid != 0 && acc_user_pass(idx)[0]) {
        char pw[PW_LEN];
        print("Password: ");
        read_line_secret(pw, PW_LEN);
        if (!streq(pw, acc_user_pass(idx))) {
            print("su: Authentication failure\n");
            return;
        }
    }
    acc_session_user(name);
}
