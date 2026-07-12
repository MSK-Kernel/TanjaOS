#include "../include/fs.h"

// External print function
extern void print(const char* s);

// Tiny string functions
static void safe_copy(char* dest, const char* src, int max) {
    if (!dest || !src || max <= 0) return;
    int i;
    for (i = 0; i < max - 1 && src[i] != 0; i++) {
        dest[i] = src[i];
    }
    dest[i] = 0;
}

static int safe_compare(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return (*a == *b) ? 1 : 0;
}

static int safe_length(const char* s) {
    if (!s) return 0;
    int len = 0;
    while (len < 256 && s[len] != 0) len++;
    return len;
}

static int starts_with(const char* str, const char* prefix) {
    if (!str || !prefix) return 0;
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

#define SMALL_MAX_FILES 32
#define SMALL_MAX_DIRS 16

static char file_names[SMALL_MAX_FILES][64];
static char file_data[SMALL_MAX_FILES][1024];
static int file_sizes[SMALL_MAX_FILES];
static int file_used[SMALL_MAX_FILES];

static char dir_names[SMALL_MAX_DIRS][64];
static int dir_used[SMALL_MAX_DIRS];

static char current_path[256] = "/";

void fs_init(void) {
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        file_used[i] = 0;
        file_sizes[i] = 0;
        file_names[i][0] = 0;
        file_data[i][0] = 0;
    }
    
    for (int i = 0; i < SMALL_MAX_DIRS; i++) {
        dir_used[i] = 0;
        dir_names[i][0] = 0;
    }
    
    safe_copy(dir_names[0], "/", 64);
    dir_used[0] = 1;
    safe_copy(current_path, "/", 256);
}

static void build_full_path(const char* name, char* full_path, int max) {
    if (!name || !full_path) return;
    
    if (name[0] == '/') {
        safe_copy(full_path, name, max);
    } else {
        safe_copy(full_path, current_path, max);
        int len = safe_length(full_path);
        
        if (len > 0 && full_path[len-1] != '/' && len < max - 1) {
            full_path[len] = '/';
            len++;
            full_path[len] = 0;
        }
        
        if (len < max - 1) {
            safe_copy(full_path + len, name, max - len);
        }
    }
}

int fs_create_directory(const char* path) {
    if (!path || path[0] == 0) return -1;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    if (safe_compare(full_path, "/")) return -1;
    
    for (int i = 0; i < SMALL_MAX_DIRS; i++) {
        if (dir_used[i] && safe_compare(dir_names[i], full_path)) {
            return -1;
        }
    }
    
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (file_used[i] && safe_compare(file_names[i], full_path)) {
            return -1;
        }
    }
    
    for (int i = 1; i < SMALL_MAX_DIRS; i++) {
        if (!dir_used[i]) {
            safe_copy(dir_names[i], full_path, 64);
            dir_used[i] = 1;
            return 0;
        }
    }
    
    return -1;
}

int fs_directory_exists(const char* path) {
    if (!path) return 0;
    if (safe_compare(path, "/")) return 1;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    for (int i = 0; i < SMALL_MAX_DIRS; i++) {
        if (dir_used[i] && safe_compare(dir_names[i], full_path)) {
            return 1;
        }
    }
    return 0;
}

int fs_delete_directory(const char* path) {
    if (!path) return -1;
    if (safe_compare(path, "/")) return -1;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    int dir_len = safe_length(full_path);
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (file_used[i]) {
            if (starts_with(file_names[i], full_path)) {
                int file_len = safe_length(file_names[i]);
                if (file_len > dir_len && file_names[i][dir_len] == '/') {
                    return -2;
                }
            }
        }
    }
    
    for (int i = 1; i < SMALL_MAX_DIRS; i++) {
        if (dir_used[i] && !safe_compare(dir_names[i], full_path)) {
            if (starts_with(dir_names[i], full_path)) {
                int d_len = safe_length(dir_names[i]);
                if (d_len > dir_len && dir_names[i][dir_len] == '/') {
                    return -2;
                }
            }
        }
    }
    
    for (int i = 1; i < SMALL_MAX_DIRS; i++) {
        if (dir_used[i] && safe_compare(dir_names[i], full_path)) {
            dir_used[i] = 0;
            dir_names[i][0] = 0;
            return 0;
        }
    }
    
    return -1;
}

int fs_create_file(const char* path) {
    if (!path || path[0] == 0) return -1;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (file_used[i] && safe_compare(file_names[i], full_path)) {
            return 0;
        }
    }
    
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (!file_used[i]) {
            safe_copy(file_names[i], full_path, 64);
            file_sizes[i] = 0;
            file_data[i][0] = 0;
            file_used[i] = 1;
            return 0;
        }
    }
    
    return -1;
}

int fs_file_exists(const char* path) {
    if (!path) return 0;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (file_used[i] && safe_compare(file_names[i], full_path)) {
            return 1;
        }
    }
    return 0;
}

int fs_delete_file(const char* path) {
    if (!path) return -1;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (file_used[i] && safe_compare(file_names[i], full_path)) {
            file_used[i] = 0;
            file_names[i][0] = 0;
            file_sizes[i] = 0;
            return 0;
        }
    }
    
    return -1;
}

int fs_write_file(const char* path, const char* data, uint32_t size) {
    if (!path || !data) return -1;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    int file_idx = -1;
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (file_used[i] && safe_compare(file_names[i], full_path)) {
            file_idx = i;
            break;
        }
    }
    
    if (file_idx == -1) {
        if (fs_create_file(path) != 0) return -1;
        for (int i = 0; i < SMALL_MAX_FILES; i++) {
            if (file_used[i] && safe_compare(file_names[i], full_path)) {
                file_idx = i;
                break;
            }
        }
        if (file_idx == -1) return -1;
    }
    
    if (size > 1000) size = 1000;
    
    for (uint32_t i = 0; i < size; i++) {
        file_data[file_idx][i] = data[i];
    }
    file_data[file_idx][size] = 0;
    file_sizes[file_idx] = size;
    
    return 0;
}

int fs_read_file(const char* path, char* buffer, uint32_t* size) {
    if (!path || !buffer || !size) return -1;
    
    *size = 0;
    buffer[0] = 0;
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    for (int i = 0; i < SMALL_MAX_FILES; i++) {
        if (file_used[i] && safe_compare(file_names[i], full_path)) {
            int read_size = file_sizes[i];
            if (read_size > 1000) read_size = 1000;
            
            for (int j = 0; j < read_size; j++) {
                buffer[j] = file_data[i][j];
            }
            buffer[read_size] = 0;
            *size = read_size;
            return 0;
        }
    }
    
    return -1;
}

int fs_list_directory(const char* path, char* buffer, uint32_t* size) {
    if (!buffer || !size) return -1;
    
    *size = 0;
    buffer[0] = 0;
    
    char list_path[256];
    if (path && path[0] != 0) {
        build_full_path(path, list_path, 256);
    } else {
        safe_copy(list_path, current_path, 256);
    }
    
    int list_len = safe_length(list_path);
    int pos = 0;
    
    for (int i = 1; i < SMALL_MAX_DIRS; i++) {
        if (dir_used[i] && pos < 4000) {
            int dir_len = safe_length(dir_names[i]);
            if (starts_with(dir_names[i], list_path) && dir_len > list_len) {
                const char* child = dir_names[i] + list_len;
                if (child[0] == '/') child++;
                
                int is_immediate = 1;
                for (int j = 0; child[j]; j++) {
                    if (child[j] == '/') {
                        is_immediate = 0;
                        break;
                    }
                }
                
                if (is_immediate) {
                    int child_len = safe_length(child);
                    if (pos + child_len + 2 < 4096) {
                        safe_copy(buffer + pos, child, 64);
                        pos += child_len;
                        buffer[pos++] = '/';
                        buffer[pos++] = '\n';
                    }
                }
            }
        }
    }
    
    for (int i = 0; i < SMALL_MAX_FILES && pos < 4000; i++) {
        if (file_used[i]) {
            int file_len = safe_length(file_names[i]);
            if (starts_with(file_names[i], list_path) && file_len > list_len) {
                const char* child = file_names[i] + list_len;
                if (child[0] == '/') child++;
                
                int is_immediate = 1;
                for (int j = 0; child[j]; j++) {
                    if (child[j] == '/') {
                        is_immediate = 0;
                        break;
                    }
                }
                
                if (is_immediate) {
                    int child_len = safe_length(child);
                    if (pos + child_len + 1 < 4096) {
                        safe_copy(buffer + pos, child, 64);
                        pos += child_len;
                        buffer[pos++] = '\n';
                    }
                }
            }
        }
    }
    
    buffer[pos] = 0;
    *size = pos;
    
    return 0;
}

int fs_change_directory(const char* path) {
    if (!path || path[0] == 0 || safe_compare(path, "/")) {
        safe_copy(current_path, "/", 256);
        return 0;
    }
    
    if (safe_compare(path, "..")) {
        if (safe_compare(current_path, "/")) return 0;
        
        int len = safe_length(current_path);
        for (int i = len - 1; i >= 0; i--) {
            if (current_path[i] == '/') {
                if (i == 0) {
                    current_path[1] = 0;
                } else {
                    current_path[i] = 0;
                }
                return 0;
            }
        }
        return 0;
    }
    
    char full_path[256];
    build_full_path(path, full_path, 256);
    
    if (safe_compare(full_path, "/")) {
        safe_copy(current_path, "/", 256);
        return 0;
    }
    
    for (int i = 0; i < SMALL_MAX_DIRS; i++) {
        if (dir_used[i] && safe_compare(dir_names[i], full_path)) {
            safe_copy(current_path, full_path, 256);
            return 0;
        }
    }
    
    return -1;
}

void fs_get_current_path(char* path) {
    if (!path) return;
    safe_copy(path, current_path, 256);
}

int find_in_directory(int dir_index, const char* name, fs_type_t type) {
    (void)dir_index;
    (void)name;
    (void)type;
    return -1;
}

int parse_path(const char* path, char* parent_path, char* name) {
    (void)path;
    if (parent_path) parent_path[0] = 0;
    if (name) name[0] = 0;
    return 0;
}
