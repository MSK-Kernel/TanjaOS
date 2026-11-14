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

/* groups [user] - the groups a user belongs to. */
void main(char* args) {
    char name[64], *p = args;
    if (!next_arg(&p, name, sizeof name))
        strncpy(name, acc_cur_name(), sizeof name - 1), name[sizeof name - 1] = 0;
    if (acc_user_find(name) < 0) {
        print("groups: user '"); print(name); print("' does not exist\n");
        return;
    }
    print(name); print(" :");
    {
        int gi;
        for (gi = 0; gi < acc_group_count(); gi++) {
            if (acc_group_is_member(gi, name)) {
                print(" "); print(acc_group_name(gi));
            }
        }
    }
    print("\n");
}
