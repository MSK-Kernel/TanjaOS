#include "cmd.h"

static int mv_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void mv_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void mv_join(char* out, const char* base, const char* name) {
    int len = mv_strlen(base);
    if (len == 0) { out[0] = '/'; len = 1; out[1] = 0; }
    else mv_strcpy(out, base, 256);
    if (out[len - 1] != '/') {
        if (len < 254) { out[len] = '/'; len++; out[len] = 0; }
    }
    int nlen = mv_strlen(name);
    for (int k = 0; k < nlen && len < 255; k++) out[len++] = name[k];
    out[len] = 0;
}

static const char* mv_basename(const char* path) {
    const char* base = path;
    const char* q = path;
    while (*q) {
        if (*q == '/' && *(q + 1)) base = q + 1;
        q++;
    }
    return base;
}

static int mv_copy_file(const char* src, const char* dst) {
    extern int fs_read_file(const char* path, char* buffer, uint32_t* size);
    extern int fs_write_file(const char* path, const char* data, uint32_t size);
    extern int fs_create_file(const char* path);
    extern int fs_file_exists(const char* path);

    char buffer[4096];
    uint32_t size = 0;

    if (fs_read_file(src, buffer, &size) != 0) return -1;
    if (!fs_file_exists(dst)) fs_create_file(dst);
    return fs_write_file(dst, buffer, size);
}

static int mv_copy_dir(const char* src, const char* dst) {
    extern int fs_create_directory(const char* path);
    extern int fs_directory_exists(const char* path);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);

    if (!fs_directory_exists(dst)) {
        if (fs_create_directory(dst) != 0) return -1;
    }

    char buf[4096];
    uint32_t size = 0;
    if (fs_list_directory(src, buf, &size) != 0) return -1;

    int i = 0;
    while (i < (int)size) {
        char entry[128];
        int j = 0;
        while (i < (int)size && buf[i] != '\n' && j < 127) { entry[j++] = buf[i++]; }
        entry[j] = 0;
        if (i < (int)size && buf[i] == '\n') i++;
        if (j == 0) continue;

        int is_dir = 0;
        if (entry[j - 1] == '/') { entry[j - 1] = 0; is_dir = 1; }

        char child_src[256];
        char child_dst[256];
        mv_join(child_src, src, entry);
        mv_join(child_dst, dst, entry);

        if (is_dir) {
            if (mv_copy_dir(child_src, child_dst) != 0) return -1;
        } else {
            if (mv_copy_file(child_src, child_dst) != 0) return -1;
        }
    }
    return 0;
}

// Deletes everything inside src (files + subdirs, depth-first), then src itself.
static int mv_remove_tree(const char* src) {
    extern int fs_delete_file(const char* path);
    extern int fs_delete_directory(const char* path);
    extern int fs_list_directory(const char* path, char* buffer, uint32_t* size);

    char buf[4096];
    uint32_t size = 0;
    if (fs_list_directory(src, buf, &size) != 0) return -1;

    int i = 0;
    while (i < (int)size) {
        char entry[128];
        int j = 0;
        while (i < (int)size && buf[i] != '\n' && j < 127) { entry[j++] = buf[i++]; }
        entry[j] = 0;
        if (i < (int)size && buf[i] == '\n') i++;
        if (j == 0) continue;

        int is_dir = 0;
        if (entry[j - 1] == '/') { entry[j - 1] = 0; is_dir = 1; }

        char child[256];
        mv_join(child, src, entry);

        if (is_dir) {
            if (mv_remove_tree(child) != 0) return -1;
        } else {
            fs_delete_file(child);
        }
    }

    return fs_delete_directory(src);
}

void cmd_mv(char* args) {
    extern void print(const char* s);
    extern int fs_file_exists(const char* path);
    extern int fs_directory_exists(const char* path);
    extern int fs_delete_file(const char* path);

    if (!args || !*args) {
        print("Usage: mv <source> <destination>\n");
        return;
    }

    while (*args == ' ') args++;

    // Trim trailing whitespace/newline
    char* end = args;
    while (*end) end++;
    end--;
    while (end > args && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = 0;
        end--;
    }

    // Split into source and destination on first space
    char* src = args;
    char* p = args;
    while (*p && *p != ' ') p++;

    if (!*p) {
        print("Usage: mv <source> <destination>\n");
        return;
    }

    *p = 0;
    p++;
    while (*p == ' ') p++;
    char* dst = p;

    if (!*dst) {
        print("Usage: mv <source> <destination>\n");
        return;
    }

    // Strip a single trailing slash from dst for existence checks
    char dst_trimmed[256];
    mv_strcpy(dst_trimmed, dst, 256);
    int dlen = mv_strlen(dst_trimmed);
    if (dlen > 1 && dst_trimmed[dlen - 1] == '/') dst_trimmed[dlen - 1] = 0;

    if (fs_directory_exists(src)) {
        char final_dst[256];
        if (fs_directory_exists(dst_trimmed)) {
            mv_join(final_dst, dst_trimmed, mv_basename(src));
        } else {
            mv_strcpy(final_dst, dst_trimmed, 256);
        }
        if (mv_copy_dir(src, final_dst) != 0) {
            print("mv: cannot move directory '");
            print(src);
            print("'\n");
            return;
        }
        mv_remove_tree(src);
        return;
    }

    if (!fs_file_exists(src)) {
        print("mv: cannot stat '");
        print(src);
        print("': No such file or directory\n");
        return;
    }

    char real_dst[256];
    if (fs_directory_exists(dst_trimmed)) {
        mv_join(real_dst, dst_trimmed, mv_basename(src));
    } else {
        mv_strcpy(real_dst, dst_trimmed, 256);
    }

    if (mv_copy_file(src, real_dst) != 0) {
        print("mv: cannot create regular file '");
        print(real_dst);
        print("'\n");
        return;
    }

    fs_delete_file(src);
}
