#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

static int next_arg(char** pp, char* out, int max) {
    char* p = *pp; int n = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && n < max - 1) out[n++] = *p++;
    out[n] = 0; *pp = p;
    return n > 0;
}

/* groupdel <group> */
void main(char* args) {
    char name[64], *p = args;
    int gi;
    if (acc_cur_uid() != 0) { print("groupdel: only root can delete groups\n"); return; }
    if (!next_arg(&p, name, sizeof name)) { print("usage: groupdel <group>\n"); return; }
    gi = acc_group_find(name);
    if (gi < 0) { print("groupdel: group '"); print(name); print("' does not exist\n"); return; }
    acc_group_del(gi);
    /* success is silent, like shadow's groupdel */
}
