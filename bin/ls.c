#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

#define LS_MAX_ENTRIES 64
#define LS_NAME_LEN    64
#define LS_TERM_WIDTH  80
#define LS_COL_SPACING 2

static int ls_name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        a++; b++;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;
}

static void ls_build_path(const char* base_args, const char* child, char* out, int out_max) {
    int p = 0;
    if (base_args && *base_args) {
        while (base_args[p] && p < out_max - 2) { out[p] = base_args[p]; p++; }
        if (p > 0 && out[p - 1] != '/') out[p++] = '/';
    }
    int q = 0;
    while (child[q] && p < out_max - 1) out[p++] = child[q++];
    out[p] = 0;
}

/* Format a byte count into out (>= 12 bytes).  human=0 -> plain bytes,
 * human=1 -> 1.5K / 12K / 3.2M style (one decimal under 10, like ls -h). */
static void ls_fmt_size(uint32_t n, int human, char* out) {
    char num[16];
    int o = 0, i;

    if (!human || n < 1024) {
        itoa((int)n, num, 10);
        for (i = 0; num[i]; i++) out[o++] = num[i];
        out[o] = 0;
        return;
    }

    {
        static const char units[] = "KMG";
        uint32_t whole = n, frac = 0;
        int u = 0;
        while (whole >= 1024 && u < 3) {
            frac = ((whole % 1024) * 10) / 1024;
            whole /= 1024;
            u++;
        }
        itoa((int)whole, num, 10);
        for (i = 0; num[i]; i++) out[o++] = num[i];
        if (whole < 10 && frac) {
            out[o++] = '.';
            out[o++] = (char)('0' + frac);
        }
        out[o++] = units[u - 1];
        out[o] = 0;
    }
}

/* Number of entries ls would show inside directory `path`
 * (dotfiles only count when show_hidden).  Uses buf as scratch. */
static int ls_count_entries(const char* path, char* buf, int show_hidden) {
    uint32_t sz = 0;
    int n = 0, i = 0;
    if (fs_list_directory(path, buf, &sz) != 0) return 0;
    while (buf[i]) {
        int start = i;
        while (buf[i] && buf[i] != '\n') i++;
        if (i > start && (show_hidden || buf[start] != '.')) n++;
        if (buf[i] == '\n') i++;
    }
    return n;
}

/* ---- shared entry state --------------------------------------------
 * One set of arrays holds whichever entries are about to be printed:
 * a directory's children, or the plain file operands of a multi-arg
 * ls (what `ls *.c` expands to). */
#define LS_MAX_OPERANDS 64

static char ls_buf[16384];
static char names[LS_MAX_ENTRIES][LS_NAME_LEN];
static int is_dir[LS_MAX_ENTRIES];
static char epath[LS_MAX_ENTRIES][300];
static int disp_len[LS_MAX_ENTRIES];
static uint16_t color[LS_MAX_ENTRIES];
static char sizestr[LS_MAX_ENTRIES][12];

/* Quote-aware tokenizer, same rules as rm/cp: "..." / '...' keep
 * spaces, backslash escapes the next character. */
static int next_arg(char **pp, char *out, int max) {
    char *p=*pp; int n=0, quote=0; char q=0;
    while (*p==' '||*p=='\t') p++; if(!*p){*pp=p;return 0;}
    while(*p){
        if(!quote&&(*p==' '||*p=='\t'))break;
        if((*p=='"'||*p=='\'')&&(!quote||*p==q)){if(!quote){quote=1;q=*p;}else quote=0;p++;continue;}
        if(*p=='\\'&&p[1]){if(n<max-1)out[n++]=p[1];p+=2;continue;}
        if(n<max-1)out[n++]=*p; p++;
    }
    out[n]=0;*pp=p;return n>0;
}

/* Style + print the entries currently loaded into names[]/epath[]/
 * is_dir[]: the -l one-per-line form, or the sorted column grid. */
/* A file is shown green when it is marked executable (chmod +x -
 * the same signal real ls uses) or when its content looks like a
 * program or script. */
extern int fs_get_mode(const char* path);

extern int fs_get_owner(const char* path);
extern int fs_errno;
extern int fs_cur_uid;

/* Green when marked executable (chmod +x - the same signal real ls
 * uses) or when the content looks like a program or script.  Content
 * sniffing is skipped for files the current user may not read, so
 * listings of mixed-permission directories don't spray "permission
 * denied". */
static int ls_is_green(const char* path) {
    int mode = fs_get_mode(path);
    if (mode > 0) {
        if (mode & 0111) return 1;
        if (fs_cur_uid != 0) {
            int owner = fs_get_owner(path);
            int rbit = (owner == fs_cur_uid) ? (mode & 0400) : (mode & 0004);
            if (!rbit) return 0;
        }
    }
    /* Content sniffing is gone: a text file whose line begins with a
     * command word used to be painted green.  The +x bit is now the
     * single source of truth - chmod +x makes a file green. */
    return 0;
}

static void ls_print_entries(int count, int opt_l, int opt_h, int opt_a) {
    int i, max_len = 0, max_size_len = 1;

    for (i = 0; i < count; i++) {
        int len = 0;
        while (names[i][len]) len++;

        if (is_dir[i]) {
            disp_len[i] = len;
            color[i] = COLOR_DIR;
            if (opt_l)
                itoa(ls_count_entries(epath[i], ls_buf, opt_a), sizestr[i], 10);
        } else {
            disp_len[i] = len;
            color[i] = ls_is_green(epath[i])
                       ? COLOR_LIGHT_GREEN : COLOR_WHITE;
            if (opt_l)
                ls_fmt_size(fs_get_file_size(epath[i]), opt_h, sizestr[i]);
        }

        if (disp_len[i] > max_len) max_len = disp_len[i];
        {
            int sl = 0;
            while (sizestr[i][sl]) sl++;
            if (sl > max_size_len) max_size_len = sl;
        }
    }

    /* -l: one entry per line */
    if (opt_l) {
        for (i = 0; i < count; i++) {
            int sl = 0, k;
            while (sizestr[i][sl]) sl++;
            print(is_dir[i] ? "d " : "- ");
            for (k = 0; k < max_size_len - sl; k++) print(" ");
            print(sizestr[i]);
            print("  ");
            print_color(names[i], color[i]);
            print("\n");
        }
        return;
    }

    int col_width = max_len + LS_COL_SPACING;
    int num_cols = LS_TERM_WIDTH / col_width;
    if (num_cols < 1) num_cols = 1;
    if (num_cols > count) num_cols = count;

    int num_rows = (count + num_cols - 1) / num_cols;

    int row, col;
    for (row = 0; row < num_rows; row++) {
        for (col = 0; col < num_cols; col++) {
            int idx = col * num_rows + row;
            if (idx >= count) continue;

            print_color(names[idx], color[idx]);

            int is_last_in_row = (col == num_cols - 1) || (idx + num_rows >= count);
            if (!is_last_in_row) {
                int pad = col_width - disp_len[idx];
                int k;
                for (k = 0; k < pad; k++) print(" ");
            }
        }
        print("\n");
    }
}

/* Load a directory's children into the shared entry arrays (sorted).
 * Returns the entry count, or -1 if the path is not a directory. */
static int ls_load_dir(const char* path_arg, int opt_a) {
    uint32_t size = 0;
    if (fs_list_directory(path_arg, ls_buf, &size) != 0)
        return -1;

    int count = 0;
    int i = 0;

    /* -a: show . and .. first (they sort to the top anyway) */
    if (opt_a) {
        names[count][0] = '.'; names[count][1] = 0;               is_dir[count] = 1; count++;
        names[count][0] = '.'; names[count][1] = '.'; names[count][2] = 0; is_dir[count] = 1; count++;
    }

    while (ls_buf[i] && count < LS_MAX_ENTRIES) {
        int start = i;
        while (ls_buf[i] && ls_buf[i] != '\n') i++;
        int end = i;
        if (ls_buf[i] == '\n') i++;

        if (end > start) {
            int dir_flag = 0;
            int name_end = end;
            if (ls_buf[name_end - 1] == '/') {
                dir_flag = 1;
                name_end--;
            }
            int len = name_end - start;
            if (len >= LS_NAME_LEN) len = LS_NAME_LEN - 1;

            /* dotfiles are hidden unless -a */
            if (!opt_a && ls_buf[start] == '.') continue;

            int j;
            for (j = 0; j < len; j++) names[count][j] = ls_buf[start + j];
            names[count][len] = 0;
            is_dir[count] = dir_flag;
            count++;
        }
    }

    /* sort by name (insertion sort, names are short) */
    for (i = 1; i < count; i++) {
        char tmp_name[LS_NAME_LEN];
        int tmp_dir = is_dir[i];
        int k;
        for (k = 0; k < LS_NAME_LEN; k++) tmp_name[k] = names[i][k];

        int j = i - 1;
        while (j >= 0 && ls_name_cmp(names[j], tmp_name) > 0) {
            for (k = 0; k < LS_NAME_LEN; k++) names[j + 1][k] = names[j][k];
            is_dir[j + 1] = is_dir[j];
            j--;
        }
        for (k = 0; k < LS_NAME_LEN; k++) names[j + 1][k] = tmp_name[k];
        is_dir[j + 1] = tmp_dir;
    }

    /* full path of every entry, for size/program/script checks */
    for (i = 0; i < count; i++)
        ls_build_path(path_arg, names[i], epath[i], sizeof(epath[i]));

    return count;
}

void main(char* args) {
    static char paths[LS_MAX_OPERANDS][256];
    int npaths = 0;
    int opt_l = 0, opt_a = 0, opt_h = 0;

    /* option parsing: leading "-lah"-style words, then the operands */
    if (args && args[0]) {
        char* p = args;
        char arg[256];
        while (next_arg(&p, arg, sizeof(arg))) {
            if (arg[0] == '-' && arg[1] && arg[1] != '-') {
                int j;
                for (j = 1; arg[j]; j++) {
                    if      (arg[j] == 'l') opt_l = 1;
                    else if (arg[j] == 'a') opt_a = 1;
                    else if (arg[j] == 'h') opt_h = 1;
                    else {
                        print("ls: invalid option -- '");
                        console_putc(arg[j]);
                        print("'\nusage: ls [-lah] [file...]\n");
                        return;
                    }
                }
                continue;
            }
            if (npaths < LS_MAX_OPERANDS) {
                int n = 0;
                while (arg[n] && n < 255) { paths[npaths][n] = arg[n]; n++; }
                paths[npaths][n] = 0;
                npaths++;
            }
        }
    }

    if (npaths == 0) {
        int count = ls_load_dir("", opt_a);
        if (count < 0) {
            print(fs_errno == 13 ? "ls: cannot open directory '.': Permission denied\n"
                                 : "ls: .: No such file or directory\n");
            return;
        }
        if (count > 0) ls_print_entries(count, opt_l, opt_h, opt_a);
        return;
    }

    if (npaths == 1) {
        const char* pa = paths[0];
        if (fs_directory_exists(pa)) {
            int count = ls_load_dir(pa, opt_a);
            if (count < 0) {
                if (fs_errno == 13) {
                    print("ls: cannot open directory '");
                    print(pa);
                    print("': Permission denied\n");
                } else {
                    print("ls: ");
                    print(pa);
                    print(": No such file or directory\n");
                }
                return;
            }
            if (count > 0) ls_print_entries(count, opt_l, opt_h, opt_a);
            return;
        }
        if (fs_file_exists(pa)) {
            /* a lone file operand prints as its name, like real ls */
            if (opt_l) {
                char sz[12];
                ls_fmt_size(fs_get_file_size(pa), opt_h, sz);
                print("- ");
                print(sz);
                print("  ");
                print_color(pa, ls_is_green(pa) ? COLOR_LIGHT_GREEN : COLOR_WHITE);
                print("\n");
            } else {
                print_color(pa, ls_is_green(pa) ? COLOR_LIGHT_GREEN : COLOR_WHITE);
                print("\n");
            }
            return;
        }
        print("ls: ");
        print(pa);
        print(": No such file or directory\n");
        return;
    }

    /* multiple operands: missing ones are reported, file operands are
     * listed together in one grid, and directories each get a
     * "path:" header - the same shape as coreutils ls. */
    static char fpaths[LS_MAX_OPERANDS][256];
    static char dpaths[LS_MAX_OPERANDS][256];
    int nfiles = 0, ndirs = 0, i;

    for (i = 0; i < npaths; i++) {
        if (fs_directory_exists(paths[i])) {
            int n = 0;
            while (paths[i][n] && n < 255) { dpaths[ndirs][n] = paths[i][n]; n++; }
            dpaths[ndirs][n] = 0; ndirs++;
        } else if (fs_file_exists(paths[i])) {
            int n = 0;
            while (paths[i][n] && n < 255) { fpaths[nfiles][n] = paths[i][n]; n++; }
            fpaths[nfiles][n] = 0; nfiles++;
        } else {
            print("ls: cannot access '");
            print(paths[i]);
            print("': No such file or directory\n");
        }
    }

    int use_headers = (nfiles + ndirs) > 1;
    int printed = 0;

    if (nfiles > 0) {
        for (i = 0; i < nfiles; i++) {
            int n = 0;
            while (fpaths[i][n] && n < LS_NAME_LEN - 1) { names[i][n] = fpaths[i][n]; n++; }
            names[i][n] = 0;
            is_dir[i] = 0;
            ls_build_path("", fpaths[i], epath[i], sizeof(epath[i]));
            if (!epath[i][0]) { epath[i][0] = '.'; epath[i][1] = 0; }
        }
        ls_print_entries(nfiles, opt_l, opt_h, opt_a);
        printed = 1;
    }

    for (i = 0; i < ndirs; i++) {
        int count;
        if (printed) print("\n");
        if (use_headers) { print(dpaths[i]); print(":\n"); }
        count = ls_load_dir(dpaths[i], opt_a);
        if (count > 0) ls_print_entries(count, opt_l, opt_h, opt_a);
        else if (count < 0) {
            if (fs_errno == 13) {
                print("ls: cannot open directory '");
                print(dpaths[i]);
                print("': Permission denied\n");
            } else {
                print("ls: ");
                print(dpaths[i]);
                print(": No such file or directory\n");
            }
        }
        printed = 1;
    }
}
