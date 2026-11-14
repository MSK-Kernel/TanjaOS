#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

#define UD_MAX 16

static int split_args(char* s, char** tv, int max) {
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        tv[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

/* userdel [-r] <name> - success is silent, like shadow's userdel */
void main(char* args) {
    char buf[256];
    char* tv[UD_MAX];
    char* name = 0;
    int rmhome = 0, n, i;
    if (acc_cur_uid() != 0) { print("userdel: only root can delete users\n"); return; }
    if (!args) args = "";
    strncpy(buf, args, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    n = split_args(buf, tv, UD_MAX);
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-r")) rmhome = 1;
        else if (tv[i][0] == '-' && tv[i][1]) {
            print("userdel: unknown option: "); print(tv[i]); print("\n"); return;
        }
        else if (!name) name = tv[i];
        else { print("usage: userdel [-r] <name>\n"); return; }
    }
    if (!name) { print("usage: userdel [-r] <name>\n"); return; }
    if (streq(name, "root")) { print("userdel: cannot delete the 'root' account\n"); return; }
    if (acc_user_del(name) != 0) {
        print("userdel: user '"); print(name); print("' does not exist\n"); return;
    }
    if (rmhome) {
        char home[96];
        int len = 0, k;
        const char* pre = "/home/";
        for (k = 0; pre[k] && len < 80; k++) home[len++] = pre[k];
        for (k = 0; name[k] && len < 94; k++) home[len++] = name[k];
        home[len] = 0;
        if (fs_directory_exists(home)) fs_delete_directory_recursive(home);
    }
}
