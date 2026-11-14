#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

#define PW_LEN 32
#define UM_MAX 24

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

static void copy_str(char* dst, const char* src, int cap) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* usermod [-p pass] [-l new] [-u uid] [-aG groups] [-L] [-U] <name>
 * Success is silent, like shadow's usermod. */
void main(char* args) {
    char buf[512];
    char* tv[UM_MAX];
    const char* pass = 0, *newname = 0, *glist = 0;
    char* name = 0;
    int uid = 0, lock = 0, unlock = 0, append_groups = 0;
    int n, i, idx, changed = 0;

    if (acc_cur_uid() != 0) { print("usermod: only root can modify users\n"); return; }
    if (!args) args = "";
    strncpy(buf, args, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    n = split_args(buf, tv, UM_MAX);

    for (i = 0; i < n; i++) {
        if (tv[i][0] != '-' || !tv[i][1]) {
            if (!name) name = tv[i];
            else {
                print("usage: usermod [-p pass] [-l new] [-u uid] [-aG groups] [-L] [-U] <name>\n");
                return;
            }
            continue;
        }
        {
            const char* fl = tv[i] + 1;
            while (*fl) {
                if (*fl == 'a') { append_groups = 1; fl++; }
                else if (*fl == 'L') { lock = 1; fl++; }
                else if (*fl == 'U') { unlock = 1; fl++; }
                else if (*fl == 'p' || *fl == 'l' || *fl == 'u' || *fl == 'G') {
                    const char* val;
                    if (fl[1]) val = fl + 1;
                    else if (i + 1 < n) val = tv[++i];
                    else {
                        print("usermod: option requires an argument -- ");
                        print_n(fl, 1); print("\n");
                        return;
                    }
                    if (*fl == 'p') pass = val;
                    else if (*fl == 'l') newname = val;
                    else if (*fl == 'u') uid = atoi_safe(val);
                    else glist = val;
                    break;
                } else {
                    print("usermod: invalid option -- ");
                    print_n(fl, 1); print("\n");
                    return;
                }
            }
        }
    }
    if (!name) {
        print("usage: usermod [-p pass] [-l new] [-u uid] [-aG groups] [-L] [-U] <name>\n");
        return;
    }
    if (append_groups && !glist) {
        print("usermod: -a flag is only allowed with the -G flag\n");
        return;
    }

    /* validate the whole -G list first: all groups must exist */
    if (glist) {
        char gb[256];
        char* gv[16];
        int gn, k;
        copy_str(gb, glist, sizeof gb);
        for (k = 0; gb[k]; k++) if (gb[k] == ',') gb[k] = ' ';
        gn = split_args(gb, gv, 16);
        for (k = 0; k < gn; k++) {
            if (acc_group_find(gv[k]) < 0) {
                print("usermod: group '"); print(gv[k]); print("' does not exist\n");
                return;
            }
        }
    }

    idx = acc_user_find(name);
    if (idx < 0) { print("usermod: user '"); print(name); print("' does not exist\n"); return; }

    if (newname) {
        int gi;
        if (streq(newname, "root")) { print("usermod: name 'root' is reserved\n"); return; }
        if (acc_user_find(newname) >= 0) {
            print("usermod: user '"); print(newname); print("' already exists\n"); return;
        }
        if (acc_user_set_name(idx, newname) != 0) {
            print("usermod: cannot rename\n"); return;
        }
        /* carry group memberships over to the new name */
        for (gi = 0; gi < acc_group_count(); gi++) {
            if (acc_group_is_member(gi, name)) {
                acc_group_member_del(gi, name);
                acc_group_member_add(gi, newname);
            }
        }
        changed = 1;
    }
    if (pass) {
        acc_user_set_pass(idx, pass);
        changed = 1;
    }
    if (uid > 0) {
        acc_user_set_uid(idx, uid);
        changed = 1;
    }
    if (lock) {
        char tmp[PW_LEN];
        const char* cur;
        if (acc_user_uid(idx) == 0) { print("usermod: cannot lock root\n"); return; }
        cur = acc_user_pass(idx);
        if (cur[0] != '!') {
            int j = 0;
            tmp[j++] = '!';
            while (cur[j - 1] && j < PW_LEN - 1) { tmp[j] = cur[j - 1]; j++; }
            tmp[j] = 0;
            acc_user_set_pass(idx, tmp);
        }
        changed = 1;
    }
    if (unlock) {
        const char* cur = acc_user_pass(idx);
        if (cur[0] == '!') acc_user_set_pass(idx, cur + 1);
        changed = 1;
    }
    if (glist) {
        char gb[256];
        char* gv[16];
        int gn, k, gi;
        copy_str(gb, glist, sizeof gb);
        for (k = 0; gb[k]; k++) if (gb[k] == ',') gb[k] = ' ';
        gn = split_args(gb, gv, 16);
        if (!append_groups) acc_group_clear_user(acc_user_name(idx));
        for (k = 0; k < gn; k++) {
            gi = acc_group_find(gv[k]);
            if (gi >= 0) acc_group_member_add(gi, acc_user_name(idx));
        }
        changed = 1;
    }
    if (!changed) print("usermod: no changes specified\n");
}
