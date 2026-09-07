#include "bin.h"

#define LS_MAX_ENTRIES 64
#define LS_NAME_LEN    64
#define LS_TERM_WIDTH  80
#define LS_COL_SPACING 2
#define LS_SCRIPT_SCAN_CAP 4096
extern uint32_t fs_get_file_size(const char* path);
extern int fs_read_file_range(const char* path, uint32_t offset, char* buffer, uint32_t capacity, uint32_t* size);
extern int fs_read_file_prefix(const char* path, char* buffer, uint32_t capacity, uint32_t* size);

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
        "cat","c","cd","clear","cp","datareset","echo","editor","exec",
        "grep","help","ls","mkdir","mv","printf","pwd","read","reboot",
        "rm","rmdir","sleep","sync","touch","uptime","hostname","exit"
    };
    int i;
    for (i = 0; i < (int)(sizeof(commands) / sizeof(commands[0])); i++)
        if (ls_name_cmp(name, commands[i]) == 0) return 1;
    return 0;
}

static int ls_file_is_tjbin(const char* path) {
    extern int fs_read_file_prefix(const char* path, char* buffer,
                                   uint32_t capacity, uint32_t* size);
    char hdr[12];
    uint32_t got = 0;
    if (fs_read_file_prefix(path, hdr, sizeof(hdr), &got) != 0)
        return 0;
    return got >= 4 && hdr[0] == 'T' && hdr[1] == 'J' &&
           hdr[2] == 'B' && hdr[3] == 'N';
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

void cmd_ls(char* args) {
    extern void print(const char* s);
    extern void print_color(const char* s, uint16_t color);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);

    static char buffer[16384];
    static char names[LS_MAX_ENTRIES][LS_NAME_LEN];
    static int is_dir[LS_MAX_ENTRIES];
    static int disp_len[LS_MAX_ENTRIES];
    static uint16_t color[LS_MAX_ENTRIES];
    uint32_t size = 0;

    if (fs_list_directory(args, buffer, &size) != 0 || size == 0) return;

    int count = 0;
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

    for (i = 0; i < count; i++) {
        int len = 0;
        while (names[i][len]) len++;

        if (is_dir[i]) {
            disp_len[i] = len;
            color[i] = COLOR_DIR;
        } else {
            static char path[300];
            ls_build_path(args, names[i], path, sizeof(path));
            disp_len[i] = len;
            /* Compiled TJBN programs and shell scripts are both green. */
            color[i] = (ls_file_is_tjbin(path) || ls_looks_like_script(path))
                       ? COLOR_LIGHT_GREEN : COLOR_WHITE;
        }

        if (disp_len[i] > max_len) max_len = disp_len[i];
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
