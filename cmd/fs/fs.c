#include "../include/fs.h"

static void strcpy_safe(char* d, const char* s, int max) {
    if (!d || !s) return;
    int i;
    for (i = 0; i < max - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

static int strcmp_safe(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int strlen_safe(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (n < 200 && s[n]) n++;
    return n;
}

#define MAX_F 32
#define MAX_D 16

static char fname[MAX_F][64];
static char fdata[MAX_F][1024];
static int fsize[MAX_F];
static int fused[MAX_F];

static char dname[MAX_D][64];
static int dused[MAX_D];

static char cwd[256];

void fs_init(void) {
    int i;
    for (i = 0; i < MAX_F; i++) { fused[i] = 0; fsize[i] = 0; fname[i][0] = 0; }
    for (i = 0; i < MAX_D; i++) { dused[i] = 0; dname[i][0] = 0; }
    dused[0] = 1;
    strcpy_safe(dname[0], "/", 64);
    strcpy_safe(cwd, "/", 256);
}

static void abs_path(const char* name, char* out) {
    if (!name || !out) return;
    if (name[0] == '/') {
        strcpy_safe(out, name, 256);
    } else {
        strcpy_safe(out, cwd, 256);
        int len = strlen_safe(out);
        if (len > 0 && out[len-1] != '/' && len < 255) {
            out[len] = '/';
            out[len+1] = 0;
            len++;
        }
        strcpy_safe(out + len, name, 256 - len);
    }
}

int fs_create_directory(const char* path) {
    if (!path || !path[0]) return -1;
    char full[256];
    abs_path(path, full);
    if (strcmp_safe(full, "/")) return -1;
    int i;
    // Check if directory already exists
    for (i = 0; i < MAX_D; i++)
        if (dused[i] && strcmp_safe(dname[i], full)) return -1;
    // Check if a file with same name exists
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full)) return -1;
    for (i = 1; i < MAX_D; i++) {
        if (!dused[i]) {
            strcpy_safe(dname[i], full, 64);
            dused[i] = 1;
            return 0;
        }
    }
    return -1;
}

int fs_directory_exists(const char* path) {
    if (!path || !path[0]) return 0;
    if (strcmp_safe(path, "/")) return 1;
    char full[256];
    abs_path(path, full);
    int i;
    for (i = 0; i < MAX_D; i++)
        if (dused[i] && strcmp_safe(dname[i], full)) return 1;
    return 0;
}

int fs_delete_directory(const char* path) {
    if (!path || !path[0]) return -1;
    if (strcmp_safe(path, "/")) return -1;
    char full[256];
    abs_path(path, full);
    int dirlen = strlen_safe(full);
    int i, j;
    
    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) continue;
        int flen = strlen_safe(fname[i]);
        if (flen <= dirlen) continue;
        int match = 1;
        for (j = 0; j < dirlen; j++) {
            if (fname[i][j] != full[j]) { match = 0; break; }
        }
        if (match && fname[i][dirlen] == '/') {
            const char* rest = fname[i] + dirlen + 1;
            int slash = 0;
            for (j = 0; rest[j]; j++) if (rest[j] == '/') { slash = 1; break; }
            if (!slash) return -2;
        }
    }
    
    for (i = 0; i < MAX_D; i++) {
        if (!dused[i] || strcmp_safe(dname[i], full)) continue;
        int dlen = strlen_safe(dname[i]);
        if (dlen <= dirlen) continue;
        int match = 1;
        for (j = 0; j < dirlen; j++) {
            if (dname[i][j] != full[j]) { match = 0; break; }
        }
        if (match && dname[i][dirlen] == '/') {
            const char* rest = dname[i] + dirlen + 1;
            int slash = 0;
            for (j = 0; rest[j]; j++) if (rest[j] == '/') { slash = 1; break; }
            if (!slash) return -2;
        }
    }
    
    for (i = 0; i < MAX_D; i++) {
        if (dused[i] && strcmp_safe(dname[i], full)) {
            dused[i] = 0;
            dname[i][0] = 0;
            return 0;
        }
    }
    return -1;
}

int fs_create_file(const char* path) {
    if (!path || !path[0]) return -1;
    char full[256];
    abs_path(path, full);
    int i;
    // Check if file already exists
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full)) return 0;
    // Check if a directory with same name exists
    for (i = 0; i < MAX_D; i++)
        if (dused[i] && strcmp_safe(dname[i], full)) return -1;
    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) {
            strcpy_safe(fname[i], full, 64);
            fsize[i] = 0;
            fdata[i][0] = 0;
            fused[i] = 1;
            return 0;
        }
    }
    return -1;
}

int fs_file_exists(const char* path) {
    if (!path || !path[0]) return 0;
    char full[256];
    abs_path(path, full);
    int i;
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full)) return 1;
    return 0;
}

int fs_delete_file(const char* path) {
    if (!path || !path[0]) return -1;
    char full[256];
    abs_path(path, full);
    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && strcmp_safe(fname[i], full)) {
            fused[i] = 0;
            fname[i][0] = 0;
            fsize[i] = 0;
            return 0;
        }
    }
    return -1;
}

int fs_write_file(const char* path, const char* data, uint32_t size) {
    if (!path || !data) return -1;
    char full[256];
    abs_path(path, full);
    int idx = -1, i;
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full)) { idx = i; break; }
    if (idx == -1) {
        fs_create_file(path);
        for (i = 0; i < MAX_F; i++)
            if (fused[i] && strcmp_safe(fname[i], full)) { idx = i; break; }
        if (idx == -1) return -1;
    }
    if (size > 1000) size = 1000;
    for (i = 0; i < (int)size; i++) fdata[idx][i] = data[i];
    fdata[idx][size] = 0;
    fsize[idx] = size;
    return 0;
}

int fs_read_file(const char* path, char* buffer, uint32_t* size) {
    if (!path || !buffer || !size) return -1;
    *size = 0; buffer[0] = 0;
    char full[256];
    abs_path(path, full);
    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && strcmp_safe(fname[i], full)) {
            int sz = fsize[i];
            if (sz > 1000) sz = 1000;
            int j;
            for (j = 0; j < sz; j++) buffer[j] = fdata[i][j];
            buffer[sz] = 0;
            *size = sz;
            return 0;
        }
    }
    return -1;
}

int fs_list_directory(const char* path, char* buf, uint32_t* size) {
    if (!buf || !size) return -1;
    *size = 0; buf[0] = 0;
    
    char lp[256];
    if (path && path[0]) abs_path(path, lp);
    else strcpy_safe(lp, cwd, 256);
    
    int llen = strlen_safe(lp);
    int pos = 0, i, j;
    
    // List subdirectories
    for (i = 1; i < MAX_D && pos < 4000; i++) {
        if (!dused[i]) continue;
        int dlen = strlen_safe(dname[i]);
        if (dlen <= llen) continue;
        int match = 1;
        for (j = 0; j < llen; j++) {
            if (dname[i][j] != lp[j]) { match = 0; break; }
        }
        if (!match) continue;
        const char* child;
        if (llen == 1 && lp[0] == '/') {
            child = dname[i] + 1;
        } else {
            if (dname[i][llen] != '/') continue;
            child = dname[i] + llen + 1;
        }
        if (child[0] == 0) continue;
        int slash = 0;
        for (j = 0; child[j]; j++) if (child[j] == '/') { slash = 1; break; }
        if (slash) continue;
        int clen = strlen_safe(child);
        for (j = 0; j < clen; j++) buf[pos++] = child[j];
        buf[pos++] = '/';
        buf[pos++] = '\n';
    }
    
    // List files
    for (i = 0; i < MAX_F && pos < 4000; i++) {
        if (!fused[i]) continue;
        int flen = strlen_safe(fname[i]);
        if (flen <= llen) continue;
        int match = 1;
        for (j = 0; j < llen; j++) {
            if (fname[i][j] != lp[j]) { match = 0; break; }
        }
        if (!match) continue;
        const char* child;
        if (llen == 1 && lp[0] == '/') {
            child = fname[i] + 1;
        } else {
            if (fname[i][llen] != '/') continue;
            child = fname[i] + llen + 1;
        }
        if (child[0] == 0) continue;
        int slash = 0;
        for (j = 0; child[j]; j++) if (child[j] == '/') { slash = 1; break; }
        if (slash) continue;
        int clen = strlen_safe(child);
        for (j = 0; j < clen; j++) buf[pos++] = child[j];
        buf[pos++] = '\n';
    }
    
    buf[pos] = 0;
    *size = pos;
    return 0;
}

int fs_change_directory(const char* path) {
    if (!path || !path[0] || strcmp_safe(path, "/")) {
        strcpy_safe(cwd, "/", 256);
        return 0;
    }
    if (strcmp_safe(path, "..")) {
        if (strcmp_safe(cwd, "/")) return 0;
        int len = strlen_safe(cwd);
        int i;
        for (i = len - 1; i >= 0; i--) {
            if (cwd[i] == '/') {
                if (i == 0) cwd[1] = 0;
                else cwd[i] = 0;
                return 0;
            }
        }
    }
    char full[256];
    abs_path(path, full);
    int i;
    for (i = 0; i < MAX_D; i++) {
        if (dused[i] && strcmp_safe(dname[i], full)) {
            strcpy_safe(cwd, full, 256);
            return 0;
        }
    }
    return -1;
}

void fs_get_current_path(char* path) {
    if (path) strcpy_safe(path, cwd, 256);
}

int find_in_directory(int a, const char* b, fs_type_t c) { (void)a;(void)b;(void)c; return -1; }
int parse_path(const char* a, char* b, char* c) { (void)a; if(b)b[0]=0; if(c)c[0]=0; return 0; }
