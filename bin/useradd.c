#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

#define PW_LEN 32
#define UA_MAX 16

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

static int atoi_safe(const char* p) {
    int v = 0;
    if (!p) return 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v;
}

/* useradd [-p password] [-u uid] <name> - success is silent, like shadow's */
void main(char* args) {
    char buf[256];
    char* tv[UA_MAX];
    const char* pass = 0;
    char* name = 0;
    int uid = 0, n, i;
    if (acc_cur_uid() != 0) { print("useradd: only root can add users\n"); return; }
    if (!args) args = "";
    strncpy(buf, args, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    n = split_args(buf, tv, UA_MAX);
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-p") && i + 1 < n) pass = tv[++i];
        else if (streq(tv[i], "-u") && i + 1 < n) uid = atoi_safe(tv[++i]);
        else if (!name) name = tv[i];
        else { print("usage: useradd [-p password] [-u uid] <name>\n"); return; }
    }
    if (!name) { print("usage: useradd [-p password] [-u uid] <name>\n"); return; }
    if (streq(name, "root")) { print("useradd: user 'root' already exists\n"); return; }
    if (acc_user_find(name) >= 0) {
        print("useradd: user '"); print(name); print("' already exists\n"); return;
    }
    {
        char pw[PW_LEN];
        if (!pass) {
            print("New password: ");
            read_line_secret(pw, PW_LEN);
            if (!pw[0]) { print("Password cannot be empty\n"); return; }
            pass = pw;
        }
        {
            int r = acc_user_add(name, pass, uid);
            if (r == -1) print("useradd: invalid name\n");
            else if (r == -2) print("useradd: user table full\n");
        }
    }
}
