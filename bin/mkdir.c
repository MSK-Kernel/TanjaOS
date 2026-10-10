#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

static int next_arg(char **pp, char *out, int max) {
    char *p = *pp;
    int n = 0, quote = 0;
    char q = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; return 0; }
    while (*p) {
        if (!quote && (*p == ' ' || *p == '\t')) break;
        if ((*p == '"' || *p == '\'') && (!quote || *p == q)) {
            if (!quote) { quote = 1; q = *p; }
            else quote = 0;
            p++;
            continue;
        }
        if (*p == '\\' && p[1]) { if (n < max-1) out[n++] = p[1]; p += 2; continue; }
        if (n < max-1) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    *pp = p;
    return n > 0;
}

static int mkdir_p(const char *path) {
    char cur[256]; int n = 0, i;
    if (!path || !*path) return -1;
    if (path[0] == '/') cur[n++] = '/';
    for (i = (path[0] == '/') ? 1 : 0; path[i] && n < 255; i++) {
        cur[n++] = path[i]; cur[n] = 0;
        if (path[i] == '/') {
            cur[n-1] = 0;
            if (*cur && !fs_directory_exists(cur) && fs_create_directory(cur) != 0) return -1;
            cur[n-1] = '/';
        }
    }
    cur[n] = 0;
    while (n > 1 && cur[n-1] == '/') cur[--n] = 0;
    if (!fs_directory_exists(cur) && fs_create_directory(cur) != 0) return -1;
    return 0;
}

void main(char* args) {
    int parents = 0, verbose = 0, any = 0;
    char arg[256]; char *p = args;
    while (next_arg(&p, arg, sizeof(arg))) {
        if (arg[0] == '-' && arg[1] && arg[1] != '-') {
            int j;
            for (j = 1; arg[j]; j++) {
                if (arg[j] == 'p') parents = 1;
                else if (arg[j] == 'v') verbose = 1;
                else { print("mkdir: invalid option -"); putc(arg[j]); print("\n"); return; }
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'p' && arg[3] == 0) { parents = 1; continue; }
        if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'v' && arg[3] == 0) { verbose = 1; continue; }
        any = 1;
        if (parents) {
            if (fs_directory_exists(arg)) continue;
            if (mkdir_p(arg) != 0) { print("mkdir: cannot create directory '"); print(arg); print("'\n"); }
            else if (verbose) { print("mkdir: created directory '"); print(arg); print("'\n"); }
        } else {
            if (fs_directory_exists(arg)) { print("mkdir: cannot create directory '"); print(arg); print("': File exists\n"); continue; }
            if (fs_create_directory(arg) != 0) { print("mkdir: cannot create directory '"); print(arg); print("': Error\n"); }
            else if (verbose) { print("mkdir: created directory '"); print(arg); print("'\n"); }
        }
    }
    if (!any) print("Usage: mkdir [-pv] <directory>...\n");
}
