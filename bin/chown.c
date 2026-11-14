#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

#define CO_MAX 24

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

/* chown [-R] <user> <file>... */
void main(char* args) {
    char buf[512];
    char* tv[CO_MAX];
    int n, recursive = 0, i, first = 0, idx;
    if (!args) { print("usage: chown [-R] <user> <file>...\n"); return; }
    strncpy(buf, args, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    n = split_args(buf, tv, CO_MAX);
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-R")) recursive = 1;
        else { first = i; break; }
    }
    if (n - first < 2) { print("usage: chown [-R] <user> <file>...\n"); return; }
    idx = acc_user_find(tv[first]);
    if (idx < 0) { print("chown: invalid user: "); print(tv[first]); print("\n"); return; }
    for (i = first + 1; i < n; i++) {
        const char* path = tv[i];
        int uid = acc_user_uid(idx);
        int r = recursive ? acc_chown_recursive(path, uid) : fs_chown(path, uid);
        if (r != 0) {
            if (fs_get_mode(path) < 0) {
                print("chown: cannot access '"); print(path);
                print("': No such file or directory\n");
            } else {
                print("chown: changing ownership of '"); print(path);
                print("': Operation not permitted\n");
            }
        }
    }
}
