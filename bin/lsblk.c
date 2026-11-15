#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

/* lsblk - list block devices
 *
 * The devices come from /dev (devfs): one node per registered block
 * device. The model and sector count of each one come from the block
 * device registry, looked up by the node's name. */

#define LB_MAX 16

static int slen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

/* registry rows: "name model sectors" */
static char r_name[LB_MAX][32];
static char r_model[LB_MAX][48];
static char r_sectors[LB_MAX][24];
static int r_count;

static void load_registry(void) {
    static char buf[4096];
    uint32_t sz = sizeof(buf);
    int i = 0;
    r_count = 0;
    if (vfs_list_bdevs(buf, &sz) != 0) return;
    while (i < (int)sz && r_count < LB_MAX) {
        int start = i, end, sp1 = -1, sp2 = -1, k, p;
        while (i < (int)sz && buf[i] != '\n') i++;
        end = i;
        if (i < (int)sz) i++;
        if (end <= start) continue;
        /* first space ends the name, last space starts the sectors */
        for (k = start; k < end; k++) {
            if (buf[k] == ' ') {
                if (sp1 < 0) sp1 = k;
                sp2 = k;
            }
        }
        if (sp1 < 0) sp1 = sp2 = end;
        p = 0;
        for (k = start; k < sp1 && p < 31; k++) r_name[r_count][p++] = buf[k];
        r_name[r_count][p] = 0;
        p = 0;
        for (k = sp1 + 1; k < sp2 && p < 47; k++) r_model[r_count][p++] = buf[k];
        r_model[r_count][p] = 0;
        p = 0;
        for (k = sp2 + 1; k < end && p < 23; k++) r_sectors[r_count][p++] = buf[k];
        r_sectors[r_count][p] = 0;
        r_count++;
    }
}

static int find_row(const char* name) {
    int i;
    for (i = 0; i < r_count; i++)
        if (streq(r_name[i], name)) return i;
    return -1;
}

void main(char* args) {
    static char list[2048];
    uint32_t sz = 0;
    int i = 0, shown = 0;
    (void)args;

    if (fs_list_directory("/dev", list, &sz) != 0) {
        print("lsblk: cannot read /dev (is devfs mounted? try: mount -t devfs none /dev)\n");
        return;
    }

    load_registry();

    print("NAME     MODEL                                    SECTORS\n");
    while (i < (int)sz) {
        char name[32];
        int p = 0, row, pad;
        while (i < (int)sz && list[i] != '\n' && p < 31)
            name[p++] = list[i++];
        name[p] = 0;
        while (i < (int)sz && list[i] != '\n') i++;      /* skip overlong name */
        if (i < (int)sz) i++;
        if (!name[0]) continue;

        row = find_row(name);
        print(name);
        pad = 9 - slen(name);
        while (pad-- > 0) print(" ");
        print(row >= 0 ? r_model[row] : "");
        pad = 41 - (row >= 0 ? slen(r_model[row]) : 0);
        while (pad-- > 0) print(" ");
        print(row >= 0 ? r_sectors[row] : "?");
        print("\n");
        shown++;
    }
    if (!shown) print("(no block devices)\n");
}
