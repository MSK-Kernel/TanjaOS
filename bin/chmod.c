#include <stdint.h>
#include <stddef.h>
#include "tanja.h"

#define CH_MAX 24

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

static int parse_octal(const char* p) {
    int v = 0, i = 0;
    while (p[i] >= '0' && p[i] <= '7' && i < 4) { v = (v << 3) + (p[i] - '0'); i++; }
    return (i > 0) ? v : -1;
}

/* symbolic clauses like +x, -x, u+x, go-rwx, a=rw, "u+x,go-w" */
static int chmod_symbolic(const char* spec, int mode) {
    if (!spec || !spec[0]) return -1;
    while (*spec) {
        int who = 0;
        while (*spec == 'u' || *spec == 'g' || *spec == 'o' || *spec == 'a') {
            if (*spec == 'u') who |= 1;
            if (*spec == 'g') who |= 2;
            if (*spec == 'o') who |= 4;
            if (*spec == 'a') who |= 7;
            spec++;
        }
        if (!who) who = 7;
        {
            char op = *spec++;
            int perm = 0;
            if (op != '+' && op != '-' && op != '=') return -1;
            while (*spec == 'r' || *spec == 'w' || *spec == 'x') {
                if (*spec == 'r') perm |= 4;
                if (*spec == 'w') perm |= 2;
                if (*spec == 'x') perm |= 1;
                spec++;
            }
            {
                int mask = 0;
                if (who & 1) mask |= perm << 6;
                if (who & 2) mask |= perm << 3;
                if (who & 4) mask |= perm;
                if (op == '+') mode |= mask;
                else if (op == '-') mode &= ~mask;
                else {
                    int clr = 0;
                    if (who & 1) clr |= 0700;
                    if (who & 2) clr |= 0070;
                    if (who & 4) clr |= 0007;
                    mode = (mode & ~clr) | mask;
                }
            }
        }
        if (*spec == ',') spec++;
        else if (*spec) return -1;
    }
    return mode & 0777;
}

/* chmod [-R] <mode> <file>... */
void main(char* args) {
    char buf[512];
    char* tv[CH_MAX];
    int n, recursive = 0, i, first = 0;
    if (!args) { print("usage: chmod [-R] <mode> <file>...\n"); return; }
    strncpy(buf, args, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    n = split_args(buf, tv, CH_MAX);
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-R")) recursive = 1;
        else { first = i; break; }
    }
    if (n - first < 2) { print("usage: chmod [-R] <mode> <file>...\n"); return; }
    {
        const char* mode = tv[first];
        for (i = first + 1; i < n; i++) {
            const char* path = tv[i];
            int m, rc;
            if (mode[0] >= '0' && mode[0] <= '7') {
                m = parse_octal(mode);
                if (m < 0) { print("chmod: invalid mode: "); print(mode); print("\n"); return; }
            } else {
                int cur = fs_get_mode(path);
                if (cur < 0) {
                    print("chmod: cannot access '"); print(path);
                    print("': No such file or directory\n");
                    continue;
                }
                m = chmod_symbolic(mode, cur);
                if (m < 0) { print("chmod: invalid mode: "); print(mode); print("\n"); return; }
            }
            rc = recursive ? acc_chmod_recursive(path, m) : fs_chmod(path, m);
            if (rc != 0) {
                if (fs_get_mode(path) < 0) {
                    print("chmod: cannot access '"); print(path);
                    print("': No such file or directory\n");
                } else {
                    print("chmod: changing permissions of '"); print(path);
                    print("': Operation not permitted\n");
                }
            }
            /* success is silent, like GNU chmod */
        }
    }
}
