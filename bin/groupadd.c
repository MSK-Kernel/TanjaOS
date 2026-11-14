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

/* groupadd <group> */
void main(char* args) {
    char name[64], *p = args;
    if (acc_cur_uid() != 0) { print("groupadd: only root can create groups\n"); return; }
    if (!next_arg(&p, name, sizeof name)) { print("usage: groupadd <group>\n"); return; }
    {
        int r = acc_group_add(name);
        if (r == -2) { print("groupadd: group '"); print(name); print("' already exists\n"); }
        else if (r == -3) print("groupadd: group table full\n");
        /* success is silent, like shadow's groupadd */
    }
}
