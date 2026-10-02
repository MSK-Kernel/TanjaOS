#include <stdint.h>
#include <stddef.h>
#include "tanja.h"
#include "utf8.h"

#define LS_MAX_ENTRIES 64
#define LS_NAME_LEN    64
#define LS_TERM_WIDTH  80
#define LS_COL_SPACING 2
#define LS_SCRIPT_SCAN_CAP 4096

static int ls_name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        a++; b++;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;
}

static int ls_is_tanja_command(const char* name) {
    static const char* const commands[] = {
        "cat","cd","clear","cp","datareset","echo","editor","exec",
        "grep","help","ls","mkdir","mv","printf","pwd","read","reboot",
        "rm","rmdir","sleep","sync","touch","uptime","hostname","exit"
    };
    int i;
    for (i = 0; i < (int)(sizeof(commands) / sizeof(commands[0])); i++)
        if (ls_name_cmp(name, commands[i]) == 0) return 1;
    return 0;
}

/* ELF32 object files (the real command programs in /Programs) are
 * shown green, like compiled binaries always were. */
static int ls_file_is_program(const char* path) {
    char hdr[8];
    uint32_t got = 0;
    if (fs_read_file_prefix(path, hdr, sizeof(hdr), &got) != 0)
        return 0;
    if (got >= 4 && hdr[0] == 0x7F && hdr[1] == 'E' &&
        hdr[2] == 'L' && hdr[3] == 'F')
        return 1;
    return 0;
}

static int ls_looks_like_script(const char* path) {
    static char chunk[256];
    uint32_t total = fs_get_file_size(path);
    if (total > LS_SCRIPT_SCAN_CAP) total = LS_SCRIPT_SCAN_CAP;
    uint32_t off = 0;
    int at_line_start = 1;
    int in_word = 0;
    char word[64];
    int wlen = 0;

    while (off < total) {
        uint32_t got = 0;
        if (fs_read_file_range(path, off, chunk, sizeof(chunk), &got) != 0)
            return 0;
        if (got == 0) break;

        for (uint32_t i = 0; i < got; i++) {
            char c = chunk[i];

            if (at_line_start) {
                if (c == ' ' || c == '\t' || c == '\r') continue;
                if (c == '\n') continue;
                if (c == '#') {
                    at_line_start = 0;
                    in_word = 0;
                    wlen = 0;
                    continue;
                }
                at_line_start = 0;
                in_word = 1;
                wlen = 0;
            }

            if (in_word) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                    word[wlen] = 0;
                    if (ls_is_tanja_command(word)) return 1;
                    in_word = 0;
                    wlen = 0;
                    if (c == '\n') at_line_start = 1;
                } else if (wlen < (int)sizeof(word) - 1) {
                    word[wlen++] = c;
                }
            } else if (c == '\n') {
                at_line_start = 1;
            }
        }
        off += got;
    }

    if (in_word) {
        word[wlen] = 0;
        if (ls_is_tanja_command(word)) return 1;
    }
    return 0;
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

void main(char* args) {

    static char buffer[16384];
    static char names[LS_MAX_ENTRIES][LS_NAME_LEN];
    static int is_dir[LS_MAX_ENTRIES];
    static int disp_len[LS_MAX_ENTRIES];
    static uint16_t color[LS_MAX_ENTRIES];
    static char sizestr[LS_MAX_ENTRIES][12];
    static char path_arg[256];
    uint32_t size = 0;
    int opt_l = 0, opt_a = 0, opt_h = 0;

    /* ---- option parsing ----------------------------------------------
     * Leading "-lah"-style words are options (any order, combinable).
     * Whatever is left is the path, taken verbatim so names containing
     * spaces keep working like they did before. */
    path_arg[0] = 0;
    if (args) {
        const char* p = args;
        for (;;) {
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] != '-' || p[1] == 0 || p[1] == ' ') break;
            p++;                                   /* skip '-' */
            while (*p && *p != ' ' && *p != '\t') {
                if      (*p == 'l') opt_l = 1;
                else if (*p == 'a') opt_a = 1;
                else if (*p == 'h') opt_h = 1;
                else {
                    print("ls: invalid option -- '");
                    putc(*p);
                    print("'\nusage: ls [-lah] [path]\n");
                    return;
                }
                p++;
            }
        }
        {
            int n = 0;
            while (*p && n < (int)sizeof(path_arg) - 1) path_arg[n++] = *p++;
            while (n > 0 && (path_arg[n - 1] == ' ' || path_arg[n - 1] == '\t' ||
                             path_arg[n - 1] == '\n' || path_arg[n - 1] == '\r'))
                n--;
            path_arg[n] = 0;
        }
    }

    if (fs_list_directory(path_arg, buffer, &size) != 0) {
        print("ls: ");
        if (path_arg[0]) print(path_arg);
        else print(".");
        print(": No such file or directory\n");
        return;
    }

    int count = 0;
    int i = 0;

    /* -a: show . and .. first (they sort to the top anyway) */
    if (opt_a) {
        names[count][0] = '.'; names[count][1] = 0;               is_dir[count++] = 1;
        names[count][0] = '.'; names[count][1] = '.'; names[count][2] = 0; is_dir[count++] = 1;
    }

    while (buffer[i] && count < LS_MAX_ENTRIES) {
        int start = i;
        while (buffer[i] && buffer[i] != '\n') i++;
        int end = i;
        if (buffer[i] == '\n') i++;

        if (end > start) {
            int dir_flag = 0;
            int name_end = end;
            if (buffer[name_end - 1] == '/') {
                dir_flag = 1;
                name_end--;
            }
            int len = name_end - start;
            if (len >= LS_NAME_LEN) len = LS_NAME_LEN - 1;

            /* dotfiles are hidden unless -a */
            if (!opt_a && buffer[start] == '.') continue;

            int j;
            for (j = 0; j < len; j++) names[count][j] = buffer[start + j];
            names[count][len] = 0;
            is_dir[count] = dir_flag;
            count++;
        }
    }

    if (count == 0) return;

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

    int max_len = 0;
    int max_size_len = 1;

    for (i = 0; i < count; i++) {
        int len = 0;
        while (names[i][len]) len++;

        if (is_dir[i]) {
            disp_len[i] = len;
            color[i] = COLOR_DIR;
            if (opt_l) {
                static char dpath[300];
                ls_build_path(path_arg, names[i], dpath, sizeof(dpath));
                /* buffer was already parsed into names[], safe to reuse */
                itoa(ls_count_entries(dpath, buffer, opt_a), sizestr[i], 10);
            }
        } else {
            static char path[300];
            ls_build_path(path_arg, names[i], path, sizeof(path));
            disp_len[i] = len;
            /* ELF32 programs and shell scripts are both green. */
            color[i] = (ls_file_is_program(path) || ls_looks_like_script(path))
                       ? COLOR_LIGHT_GREEN : COLOR_WHITE;
            if (opt_l)
                ls_fmt_size(fs_get_file_size(path), opt_h, sizestr[i]);
        }

        if (disp_len[i] > max_len) max_len = disp_len[i];
        {
            int sl = 0;
            while (sizestr[i][sl]) sl++;
            if (sl > max_size_len) max_size_len = sl;
        }
    }

    /* ---- -l: one entry per line:  "d  3  name" (3 = items inside) / "-  1.5K  name" */
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
