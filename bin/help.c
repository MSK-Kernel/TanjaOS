#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

#define MAX_NAMES 128
#define HELP_TERM_WIDTH 80
#define NAME_CAP  40

static char names[MAX_NAMES][NAME_CAP];
static int ncount;

/* Add a name unless it is already listed. */
static void add_name(const char* src, int len) {
    int i;
    if (len <= 0) return;
    if (len >= NAME_CAP) len = NAME_CAP - 1;
    for (i = 0; i < ncount; i++)
        if (strncmp(names[i], src, (unsigned int)len) == 0 && names[i][len] == 0)
            return;
    if (ncount >= MAX_NAMES) return;
    for (i = 0; i < len; i++) names[ncount][i] = src[i];
    names[ncount][len] = 0;
    ncount++;
}

/* Alphabetical insertion sort. */
static void sort_names(void) {
    int i, j;
    for (i = 1; i < ncount; i++) {
        char key[NAME_CAP];
        memcpy(key, names[i], NAME_CAP);
        j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            memcpy(names[j + 1], names[j], NAME_CAP);
            j--;
        }
        memcpy(names[j + 1], key, NAME_CAP);
    }
}

/* Collect the kernel built-ins. */
static void collect_builtins(void) {
    static char buf[1024];
    int n = builtin_names(buf, (int)sizeof(buf));
    int i = 0;
    while (i < n) {
        int start = i;
        while (i < n && buf[i] != '\n') i++;
        add_name(buf + start, i - start);
        if (i < n) i++;
    }
}

/* Collect the commands in a directory (strips trailing '/' and the
 * '.o' suffix). */
static void collect_dir(const char* dir) {
    static char buf[4096];
    uint32_t size = 0;

    if (fs_list_directory(dir, buf, &size) != 0 || size == 0)
        return;

    int i = 0;
    while (i < (int)size) {
        int start = i;
        while (buf[i] && buf[i] != '\n') i++;
        int end = i;
        if (buf[i] == '\n') i++;

        if (end > start) {
            int len = end - start;
            if (buf[end - 1] == '/') len--;
            if (len > 2 && buf[start + len - 2] == '.' &&
                buf[start + len - 1] == 'o')
                len -= 2;
            add_name(buf + start, len);
        }
    }
}

void main(char* args) {
    (void)args;

    /* Built-ins first so a same-named /bin file is de-duplicated away. */
    collect_builtins();
    collect_dir("/bin");
    sort_names();

    print("\nAvailable commands:\n\n");

    int i;
    int col = 0;
    for (i = 0; i < ncount; i++) {
        int len = 0;
        while (names[i][len]) len++;
        if (col + len + 3 >= HELP_TERM_WIDTH) { print("\n"); col = 0; }
        if (col) print(" ");
        print(names[i]);
        col += len + 3;
    }
    if (ncount)
        print("\n\n");
}
