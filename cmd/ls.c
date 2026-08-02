#include "cmd.h"

#define LS_MAX_ENTRIES 64
#define LS_NAME_LEN    64
#define LS_TERM_WIDTH  80  // matches VGA_WIDTH in kernel.c
#define LS_COL_SPACING 2   // gap between columns

// Case-sensitive byte-order compare, like a plain strcmp.
static int ls_name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        a++; b++;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;
}

// Does any line in the file start with a word that matches a
// registered command name? If so, treat it as a script.
static int ls_looks_like_script(const char* path) {
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);

    char fbuf[4096];
    uint32_t fsize = 0;

    if (fs_read_file(path, fbuf, &fsize) != 0) return 0;

    uint32_t k = 0;
    while (k < fsize) {
        while (k < fsize && (fbuf[k] == ' ' || fbuf[k] == '\t' ||
               fbuf[k] == '\n' || fbuf[k] == '\r')) k++;
        if (k >= fsize) break;

        char word[32];
        int w = 0;
        while (k < fsize && fbuf[k] != ' ' && fbuf[k] != '\t' &&
               fbuf[k] != '\n' && fbuf[k] != '\r' && w < 31) {
            word[w++] = fbuf[k++];
        }
        word[w] = 0;

        if (cmd_exists(word)) return 1;

        while (k < fsize && fbuf[k] != '\n') k++;
    }
    return 0;
}

// Build the full path to `child` under `base_args` (whatever the
// user passed to ls, or the cwd if nothing was passed).
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

void cmd_ls(char* args) {
    extern void print(const char* s);
    extern void print_color(const char* s, uint16_t color);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);

    char buffer[4096];
    uint32_t size = 0;

    if (fs_list_directory(args, buffer, &size) != 0 || size == 0) return;

    char names[LS_MAX_ENTRIES][LS_NAME_LEN];
    int is_dir[LS_MAX_ENTRIES];
    int count = 0;

    // Parse fs_list_directory's output (folders first, then files,
    // each "\n"-terminated, folders additionally "/"-terminated)
    // into a flat list of (name, is_dir) entries.
    int i = 0;
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

            int j;
            for (j = 0; j < len; j++) names[count][j] = buffer[start + j];
            names[count][len] = 0;
            is_dir[count] = dir_flag;
            count++;
        }
    }

    // Sort ALL entries together, alphabetically, folders and files
    // interleaved - the way plain `ls` sorts on real Linux (as
    // opposed to grouping directories first).
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

    // Precompute each entry's display length (name + trailing '/' for
    // dirs) and color (dir / script / normal file), once, up front.
    int disp_len[LS_MAX_ENTRIES];
    uint16_t color[LS_MAX_ENTRIES];
    int max_len = 0;

    for (i = 0; i < count; i++) {
        int len = 0;
        while (names[i][len]) len++;

        if (is_dir[i]) {
            disp_len[i] = len;
            color[i] = COLOR_DIR;
        } else {
            char path[300];
            ls_build_path(args, names[i], path, sizeof(path));
            disp_len[i] = len;
            color[i] = ls_looks_like_script(path) ? COLOR_LIGHT_GREEN : COLOR_WHITE;
        }

        if (disp_len[i] > max_len) max_len = disp_len[i];
    }

    // Lay entries out in columns, filling down each column before
    // moving to the next (the same layout plain `ls` uses on a
    // terminal), instead of one entry per line.
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
