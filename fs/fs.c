#include "../include/fs.h"

// Hook into store.c. It's always linked in; it no-ops internally if no
// disk-backed Storefile is active. Declared extern here (rather than
// pulled in via a header) to avoid fs.h depending on store.h.
extern void store_autosave(void);

static int fs_batch_depth = 0;

static void fs_commit_change(void) {
    if (fs_batch_depth == 0)
        store_autosave();
}

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
    while (n < 1023 && s[n]) n++;
    return n;
}

#define MAX_F 96
#define MAX_D 64
/* Per-file capacity.  The largest shipped home/ files (the Torah books
 * up to ~200KB; the tcc compiler at ~281KB) must fit in one slot, or
 * fs_seed_home silently skips them and datareset can never restore them.
 * 320KB covers them with headroom.  NOTE: fs.c BSS and the Storefile buffer in store.c
 * scale with this (see fs_store_size / STORE_BUF_SECTORS). */
#define MAX_FILE_DATA 327680
#define FS_PATH_CAP 256

/*
 * Keep the filesystem metadata small on low-RAM machines.
 *
 * The old implementation reserved MAX_F * MAX_FILE_DATA bytes in .bss
 * (about 30 MiB) even when almost every file was empty.  That made the
 * kernel need a huge amount of RAM just to boot simple commands.
 *
 * Files now live in a compact 8 MiB first-fit pool and are allocated only
 * when they actually contain data.  MAX_FILE_DATA is still the per-file
 * limit, so normal large files (including TCC source/header files) keep
 * working without reserving 30+ MiB up front.
 */
#define FS_DATA_POOL_SIZE (8u * 1024u * 1024u)

typedef struct fs_pool_block {
    uint32_t size;       /* payload bytes in this block */
    uint32_t free;       /* 1 = free, 0 = allocated */
} fs_pool_block_t;

int fs_delete_directory_recursive(const char *path);

static char fname[MAX_F][FS_PATH_CAP];
static char* fdata[MAX_F];
static uint8_t fpool[FS_DATA_POOL_SIZE];
static int fsize[MAX_F];
static int fused[MAX_F];

static char dname[MAX_D][FS_PATH_CAP];
static int dused[MAX_D];

static char cwd[256];

/* Small private allocator for file contents.  It never calls libc malloc,
 * so the filesystem remains usable before the program heap exists. */
static void fpool_init(void) {
    fs_pool_block_t* b = (fs_pool_block_t*)fpool;
    b->size = FS_DATA_POOL_SIZE - sizeof(fs_pool_block_t);
    b->free = 1;
}

static void fpool_coalesce(void) {
    uint32_t off = 0;
    while (off + sizeof(fs_pool_block_t) <= FS_DATA_POOL_SIZE) {
        fs_pool_block_t* b = (fs_pool_block_t*)(fpool + off);
        if (b->size > FS_DATA_POOL_SIZE - off - sizeof(fs_pool_block_t))
            break;

        uint32_t next_off = off + sizeof(fs_pool_block_t) + b->size;
        if (b->free && next_off + sizeof(fs_pool_block_t) <= FS_DATA_POOL_SIZE) {
            fs_pool_block_t* n = (fs_pool_block_t*)(fpool + next_off);
            if (n->size <= FS_DATA_POOL_SIZE - next_off - sizeof(fs_pool_block_t) &&
                n->free) {
                b->size += sizeof(fs_pool_block_t) + n->size;
                continue;
            }
        }
        if (next_off >= FS_DATA_POOL_SIZE) break;
        off = next_off;
    }
}

static void* fpool_alloc(uint32_t bytes) {
    if (bytes == 0) return 0;
    bytes = (bytes + 3u) & ~3u;

    uint32_t off = 0;
    while (off + sizeof(fs_pool_block_t) <= FS_DATA_POOL_SIZE) {
        fs_pool_block_t* b = (fs_pool_block_t*)(fpool + off);
        if (b->size > FS_DATA_POOL_SIZE - off - sizeof(fs_pool_block_t))
            return 0;

        if (b->free && b->size >= bytes) {
            uint32_t extra = b->size - bytes;
            if (extra >= sizeof(fs_pool_block_t) + 16u) {
                uint32_t next_off = off + sizeof(fs_pool_block_t) + bytes;
                fs_pool_block_t* n = (fs_pool_block_t*)(fpool + next_off);
                n->size = extra - sizeof(fs_pool_block_t);
                n->free = 1;
                b->size = bytes;
            }
            b->free = 0;
            return (void*)(b + 1);
        }

        if (off + sizeof(fs_pool_block_t) + b->size >= FS_DATA_POOL_SIZE)
            break;
        off += sizeof(fs_pool_block_t) + b->size;
    }
    return 0;
}

static void fpool_free(void* ptr) {
    if (!ptr) return;
    if ((uint8_t*)ptr < fpool + sizeof(fs_pool_block_t) ||
        (uint8_t*)ptr >= fpool + FS_DATA_POOL_SIZE)
        return;

    fs_pool_block_t* b = ((fs_pool_block_t*)ptr) - 1;
    b->free = 1;
    fpool_coalesce();
}

static int fdata_resize(int idx, uint32_t size) {
    if (idx < 0 || idx >= MAX_F) return -1;
    if (size > MAX_FILE_DATA - 1) return -2;

    if (size == 0) {
        fpool_free(fdata[idx]);
        fdata[idx] = 0;
        fsize[idx] = 0;
        return 0;
    }

    char* p = (char*)fpool_alloc(size + 1);
    if (!p) return -3;

    if (fdata[idx] && fsize[idx] > 0) {
        uint32_t copy = (uint32_t)fsize[idx];
        if (copy > size) copy = size;
        uint32_t j;
        for (j = 0; j < copy; j++) p[j] = fdata[idx][j];
    }
    p[size] = 0;

    fpool_free(fdata[idx]);
    fdata[idx] = p;
    fsize[idx] = (int)size;
    return 0;
}



/* Built-in source-tree home/ archive.  The build embeds home/ as a tar
 * archive; this keeps the user-facing interface as simple as an ordinary
 * directory while allowing arbitrary files and nested directories to ship
 * in the boot image. */
extern const unsigned char _binary_home_tar_start[];
extern const unsigned char _binary_home_tar_end[];

static uint32_t tar_octal(const unsigned char* p, int n) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (p[i] >= '0' && p[i] <= '7') v = (v << 3) + (uint32_t)(p[i] - '0');
    }
    return v;
}

static void home_copy_path(char* out, const unsigned char* name, int n) {
    int i = 0, j = 0;
    out[0] = '/'; j = 1;
    while (i < n && name[i] && j < 255) {
        char c = (char)name[i++];
        if (i == 1 && c == '.') continue;
        if (i == 2 && c == '/') continue;
        out[j++] = c;
    }
    if (j > 1 && out[j - 1] == '/') j--;
    out[j] = 0;
}

void fs_seed_home(void) {
    const unsigned char* p = _binary_home_tar_start;
    const unsigned char* end = _binary_home_tar_end;

    /* The source-tree home/ directory is the contents of TanjaOS's
       starting home directory, not a runtime /home directory.  Thus: 
       home/foo -> /foo and home/projects/x -> /projects/x. */
    while (p + 512 <= end) {
        const unsigned char* h = p;
        int empty = 1, i;
        for (i = 0; i < 512; i++) if (h[i]) { empty = 0; break; }
        if (empty) break;

        uint32_t size = tar_octal(h + 124, 12);
        char path[256];
        home_copy_path(path, h, 100);
        if (!path[1]) { p += 512; continue; }

        char type = (char)h[156];
        if (type == '5') {
            if (!fs_directory_exists(path)) fs_create_directory(path);
        } else if (type == '0' || type == '\0') {
            /* home/ supplies defaults only; never overwrite a persistent file. */
            if (!fs_file_exists(path) && size <= MAX_FILE_DATA - 1) {
                if (fs_create_file(path) == 0)
                    fs_write_file(path, (const char*)(p + 512), size);
            }
        }

        uint32_t blocks = (size + 511) / 512;
        if (p + 512 + blocks * 512 > end) break;
        p += 512 + blocks * 512;
    }

    /* The user's home directory (where logins start, shown as ~).
       home.tar cannot reliably carry empty directories, so /home
       is created here - after every init/seed, including datareset's
       factory restore. */
    if (!fs_directory_exists("/home"))
        fs_create_directory("/home");

    /* Standard user folders under ~, for the same reason as /home
       above: home.tar cannot reliably carry empty directories, so they
       are created here after every init/seed, including datareset's
       factory restore. */
    {
        static const char* const home_subdirs[] = {
            "Documents", "Programs", "Projects", "Trash", "Scripts"
        };
        int i;
        for (i = 0; i < 5; i++) {
            char p[FS_PATH_CAP];
            int len = strlen_safe("/home/");
            strcpy_safe(p, "/home/", sizeof(p));
            strcpy_safe(p + len, home_subdirs[i], (int)sizeof(p) - len);
            if (!fs_directory_exists(p))
                fs_create_directory(p);
        }
    }
}

void fs_init(void) {
    int i;
    fpool_init();
    for (i = 0; i < MAX_F; i++) { fused[i] = 0; fsize[i] = 0; fdata[i] = 0; fname[i][0] = 0; }
    for (i = 0; i < MAX_D; i++) { dused[i] = 0; dname[i][0] = 0; }
    dused[0] = 1;
    strcpy_safe(dname[0], "/", FS_PATH_CAP);
    strcpy_safe(cwd, "/", 256);
}

/* Build a canonical absolute path.  This is deliberately done inside the
 * filesystem layer so EVERY command gets . and .. support automatically:
 *   ../file       -> parent of cwd
 *   ../../file    -> two parents up
 *   ./file        -> file in cwd
 *   /a/../b       -> /b
 *   /../../x      -> /x (cannot go above root)
 * Repeated slashes are also collapsed. */
static void abs_path(const char* name, char* out) {
    if (!name || !out) return;

    char combined[512];
    int pos = 0, i = 0;

    if (name[0] == '/') {
        while (name[i] && pos < (int)sizeof(combined) - 1)
            combined[pos++] = name[i++];
    } else {
        int j = 0;
        while (cwd[j] && pos < (int)sizeof(combined) - 1)
            combined[pos++] = cwd[j++];
        if (pos == 0) combined[pos++] = '/';
        if (pos > 1 && combined[pos - 1] != '/') combined[pos++] = '/';
        i = 0;
        while (name[i] && pos < (int)sizeof(combined) - 1)
            combined[pos++] = name[i++];
    }
    combined[pos] = 0;

    /* Store component start offsets.  MAX_PATH_DEPTH is deliberately
       small because FS_PATH_CAP is only 256 bytes anyway. */
    int starts[64];
    int depth = 0;
    int n = 0;
    out[0] = '/';
    out[1] = 0;

    while (n < pos) {
        while (n < pos && combined[n] == '/') n++;
        if (n >= pos) break;

        int start = n;
        while (n < pos && combined[n] != '/') n++;
        int len = n - start;

        /* Skip '.' components. */
        if (len == 1 && combined[start] == '.')
            continue;

        /* '..' removes one component, but never escapes /. */
        if (len == 2 && combined[start] == '.' && combined[start + 1] == '.') {
            if (depth > 0) {
                /* starts[] points just past the separator slash, so cut
                 * one earlier to drop it too (keep the lone root "/").
                 * Without this "/a/b/.." became "/a/" and the next
                 * component then produced "/a//x". */
                int cut = starts[depth - 1];
                if (cut > 1) cut--;
                out[cut] = 0;
                depth--;
            }
            continue;
        }

        if (depth >= 64) continue;
        int olen = strlen_safe(out);
        if (olen > 1) {
            if (olen < 255) { out[olen++] = '/'; out[olen] = 0; }
        }
        starts[depth++] = olen;
        int k;
        for (k = 0; k < len && olen < 255; k++)
            out[olen++] = combined[start + k];
        out[olen] = 0;
    }

    if (out[0] == 0) strcpy_safe(out, "/", 256);
}

static int parent_exists(const char* path) {
    char full[256];
    abs_path(path, full);

    char parent[256];
    strcpy_safe(parent, full, 256);

    int len = strlen_safe(parent);

    if (len <= 1)
        return 1; // root

    while (len > 0 && parent[len - 1] != '/')
        len--;

    if (len <= 1)
        return 1;

    parent[len - 1] = 0;

    return fs_directory_exists(parent);
}

int fs_create_directory(const char* path) {
    if (!path || !path[0]) return -1;
    char full[256];
    abs_path(path, full);
    if (!parent_exists(path))
    return -1;
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
            strcpy_safe(dname[i], full, FS_PATH_CAP);
            dused[i] = 1;
            fs_commit_change();
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

/* Return 1 when a directory exists but has no direct child files or
 * directories. Missing directories return 0 so callers can distinguish
 * "not present" from an actually empty directory. */
int fs_directory_is_empty(const char* path) {
    if (!path || !path[0]) return 0;

    char full[FS_PATH_CAP];
    abs_path(path, full);
    if (!fs_directory_exists(full)) return 0;

    int llen = strlen_safe(full);
    int i, j;

    for (i = 1; i < MAX_D; i++) {
        if (!dused[i]) continue;
        int dlen = strlen_safe(dname[i]);
        if (dlen <= llen) continue;
        int match = 1;
        for (j = 0; j < llen; j++) {
            if (dname[i][j] != full[j]) { match = 0; break; }
        }
        if (!match) continue;
        if (full[0] == '/' && full[1] == 0) {
            if (dname[i][0] == '/' && dname[i][1] != 0) return 0;
        } else if (dname[i][llen] == '/') {
            const char* child = dname[i] + llen + 1;
            if (*child) { int slash = 0; for (j = 0; child[j]; j++) if (child[j] == '/') { slash = 1; break; } if (!slash) return 0; }
        }
    }

    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) continue;
        int flen = strlen_safe(fname[i]);
        if (flen <= llen) continue;
        int match = 1;
        for (j = 0; j < llen; j++) {
            if (fname[i][j] != full[j]) { match = 0; break; }
        }
        if (!match) continue;
        if (full[0] == '/' && full[1] == 0) {
            if (fname[i][0] == '/' && fname[i][1] != 0) return 0;
        } else if (fname[i][llen] == '/') {
            const char* child = fname[i] + llen + 1;
            if (*child) { int slash = 0; for (j = 0; child[j]; j++) if (child[j] == '/') { slash = 1; break; } if (!slash) return 0; }
        }
    }

    return 1;
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
            fs_commit_change();
            return 0;
        }
    }
    return -1;
}


int fs_delete_directory_recursive(const char* path) {
    if (!path || !path[0]) return -1;
    if (!fs_directory_exists(path)) return -1;

    /*
     * rm -rf used to call store_autosave() once per child.  Batch the whole
     * tree deletion into one compact Storefile write.
     */
    fs_batch_depth++;

    char buf[16384];
    uint32_t size = 0;
    if (fs_list_directory(path, buf, &size) != 0) {
        fs_batch_depth--;
        return -1;
    }

    int result = 0;
    int i = 0;
    while (i < (int)size) {
        char entry[256];
        int j = 0;
        while (i < (int)size && buf[i] != '\n' && j < 255)
            entry[j++] = buf[i++];
        entry[j] = 0;
        if (i < (int)size && buf[i] == '\n') i++;
        if (j == 0) continue;

        int is_dir = 0;
        if (entry[j - 1] == '/') {
            entry[j - 1] = 0;
            is_dir = 1;
        }

        char child[256];
        int plen = strlen_safe(path);
        int clen = 0;
        if (strcmp_safe(path, "/")) {
            child[0] = '/'; child[1] = 0;
            clen = 1;
        }
        if (plen > 0 && !(plen == 1 && path[0] == '/')) {
            int k;
            for (k = 0; k < plen && clen < 255; k++) child[clen++] = path[k];
        }
        if (clen > 1 && child[clen - 1] != '/' && clen < 255) child[clen++] = '/';
        else if (clen == 0) child[clen++] = '/';

        int k;
        for (k = 0; entry[k] && clen < 255; k++) child[clen++] = entry[k];
        child[clen] = 0;

        if (is_dir) {
            if (fs_delete_directory_recursive(child) != 0) {
                result = -1;
                break;
            }
        } else {
            if (fs_delete_file(child) != 0) {
                result = -1;
                break;
            }
        }
    }

    if (result == 0 && fs_delete_directory(path) != 0)
        result = -1;

    fs_batch_depth--;
    if (fs_batch_depth == 0)
        store_autosave();

    return result;
}

int fs_create_file(const char* path) {
    if (!path || !path[0]) return -1;
    char full[256];
    abs_path(path, full);
    if (!parent_exists(path))
    return -1;
    int i;
    // Check if file already exists
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full)) return 0;
    // Check if a directory with same name exists
    for (i = 0; i < MAX_D; i++)
        if (dused[i] && strcmp_safe(dname[i], full)) return -1;
    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) {
            strcpy_safe(fname[i], full, FS_PATH_CAP);
            fsize[i] = 0;
            fdata[i] = 0;
            fused[i] = 1;
            fs_commit_change();
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
            fpool_free(fdata[i]);
            fdata[i] = 0;
            fsize[i] = 0;
            fs_commit_change();
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
    if (fs_create_file(path) != 0)
        return -1;
        for (i = 0; i < MAX_F; i++)
            if (fused[i] && strcmp_safe(fname[i], full)) { idx = i; break; }
        if (idx == -1) return -1;
    }
    if (size > MAX_FILE_DATA - 1) return -2;
    if (fdata_resize(idx, size) != 0) return -3;
    for (i = 0; i < (int)size; i++) fdata[idx][i] = data[i];
    fdata[idx][size] = 0;
    fs_commit_change();
    return 0;
}

int fs_read_file(const char* path, char* buffer, uint32_t* size) {
    /* Callers pass their buffer capacity in *size.  Never write more
       than that: the old implementation copied the WHOLE file into the
       caller's buffer and smashed the stack of any command holding a
       small buffer (grep's 4KB buffer crashed on the ~200KB Torah
       files).  Reads at most capacity-1 bytes, reports the actual
       byte count in *size. */
    if (!path || !buffer || !size) return -1;
    return fs_read_file_prefix(path, buffer, *size + 1, size);
}

int fs_read_file_prefix(const char* path, char* buffer, uint32_t capacity, uint32_t* size) {
    if (!path || !buffer || !size || capacity == 0) return -1;
    *size = 0;
    buffer[0] = 0;

    char full[256];
    abs_path(path, full);

    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && strcmp_safe(fname[i], full)) {
            uint32_t sz = (uint32_t)fsize[i];
            if (sz > capacity - 1) sz = capacity - 1;
            uint32_t j;
            for (j = 0; j < sz; j++) buffer[j] = fdata[i][j];
            buffer[sz] = 0;
            *size = sz;
            return 0;
        }
    }
    return -1;
}

int fs_read_file_range(const char* path, uint32_t offset, char* buffer,
                       uint32_t capacity, uint32_t* size) {
    if (!path || !buffer || !size || capacity == 0) return -1;
    *size = 0;
    char full[FS_PATH_CAP];
    abs_path(path, full);

    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && strcmp_safe(fname[i], full)) {
            uint32_t total = (uint32_t)fsize[i];
            if (offset >= total) {
                buffer[0] = 0;
                return 0;
            }

            uint32_t n = total - offset;
            if (n > capacity) n = capacity;

            uint32_t j;
            for (j = 0; j < n; j++)
                buffer[j] = fdata[i][offset + j];

            *size = n;
            return 0;
        }
    }
    return -1;
}

uint32_t fs_get_file_size(const char* path) {
    if (!path || !path[0]) return 0;
    char full[FS_PATH_CAP];
    abs_path(path, full);

    int i;
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full))
            return (uint32_t)fsize[i];

    return 0;
}


int fs_list_directory(const char* path, char* buf, uint32_t* size) {
    if (!buf || !size) return -1;
    *size = 0; buf[0] = 0;
    
    char lp[256];
    if (path && path[0]) abs_path(path, lp);
    else strcpy_safe(lp, cwd, 256);
    
    /* A missing path is an error, not an empty directory.  Previously
     * ls could silently succeed because this function simply found zero
     * matching children and returned an empty listing. */
    if (!fs_directory_exists(lp))
        return -1;

    int llen = strlen_safe(lp);
    int pos = 0, i, j;
    
    // List subdirectories. The caller supplies a large buffer; never
    // write past it. MAX_D/MAX_F bound the number of entries.
    for (i = 1; i < MAX_D && pos < 16380; i++) {
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
        if (pos + clen + 3 >= 16384) continue;
        for (j = 0; j < clen; j++) buf[pos++] = child[j];
        buf[pos++] = '/';
        buf[pos++] = '\n';
    }
    
    // List files
    for (i = 0; i < MAX_F && pos < 16380; i++) {
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
        if (pos + clen + 2 >= 16384) continue;
        for (j = 0; j < clen; j++) buf[pos++] = child[j];
        buf[pos++] = '\n';
    }
    
    buf[pos] = 0;
    *size = pos;
    return 0;
}

int fs_change_directory(const char* path) {
    if (!path || !path[0]) {
        strcpy_safe(cwd, "/", 256);
        return 0;
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

// ============================================================
// STOREFILE SERIALIZATION
//
// Packs the static fname/fdata/fsize/fused/dname/dused/cwd tables into
// a flat blob (and back). Layout, in order:
//   4 bytes  magic "TJFS"
//   1 byte   version
//   3 bytes  reserved/padding
//   MAX_D    bytes   dused[]
//   MAX_D*64 bytes   dname[][64]
//   MAX_F    bytes   fused[]
//   MAX_F*4  bytes   fsize[]   (little-endian)
//   MAX_F*64 bytes   fname[][64]
//   an 8 MiB on-demand file-data pool
//   256      bytes   cwd
// ============================================================

#define STORE_VERSION 4

/*
 * Storefile format v4 is intentionally sparse.  The old format reserved
 * MAX_FILE_DATA bytes for EVERY file slot, so a single mkdir/rm caused a
 * ~31 MB serialize + disk write even when the filesystem only contained a
 * few KB.  v4 keeps the fixed directory/file metadata but stores only the
 * bytes belonging to files that are actually in use.
 *
 * Layout:
 *   8       magic/version
 *   MAX_D   directory used flags
 *   MAX_D*256 directory names
 *   MAX_F   file used flags
 *   MAX_F*4 file sizes
 *   MAX_F*256 file names
 *   sum(fsize[i]) file data for used files, in slot order
 *   256     cwd
 */
uint32_t fs_store_size(void) {
    return 8
        + MAX_D
        + (MAX_D * FS_PATH_CAP)
        + MAX_F
        + (MAX_F * 4)
        + (MAX_F * FS_PATH_CAP)
        + FS_DATA_POOL_SIZE
        + 256;
}

uint32_t fs_serialized_size(void) {
    uint32_t need = 8
        + MAX_D
        + (MAX_D * FS_PATH_CAP)
        + MAX_F
        + (MAX_F * 4)
        + (MAX_F * FS_PATH_CAP)
        + 256;
    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && fsize[i] > 0)
            need += (uint32_t)fsize[i];
    }
    return need;
}

int fs_serialize(uint8_t* buf, uint32_t buf_size) {
    if (!buf) return -1;
    uint32_t need = fs_serialized_size();
    if (buf_size < need) return -1;

    uint32_t p = 0;
    int i, j;

    buf[p++] = 'T'; buf[p++] = 'J'; buf[p++] = 'F'; buf[p++] = 'S';
    buf[p++] = STORE_VERSION; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;

    for (i = 0; i < MAX_D; i++) buf[p++] = (uint8_t)dused[i];
    for (i = 0; i < MAX_D; i++)
        for (j = 0; j < FS_PATH_CAP; j++) buf[p++] = (uint8_t)dname[i][j];

    for (i = 0; i < MAX_F; i++) buf[p++] = (uint8_t)fused[i];
    for (i = 0; i < MAX_F; i++) {
        uint32_t sz = (fsize[i] < 0) ? 0u : (uint32_t)fsize[i];
        buf[p++] = (uint8_t)(sz & 0xFF);
        buf[p++] = (uint8_t)((sz >> 8) & 0xFF);
        buf[p++] = (uint8_t)((sz >> 16) & 0xFF);
        buf[p++] = (uint8_t)((sz >> 24) & 0xFF);
    }

    for (i = 0; i < MAX_F; i++)
        for (j = 0; j < FS_PATH_CAP; j++) buf[p++] = (uint8_t)fname[i][j];

    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) continue;
        for (j = 0; j < fsize[i]; j++)
            buf[p++] = (uint8_t)fdata[i][j];
    }

    for (j = 0; j < 256; j++) buf[p++] = (uint8_t)cwd[j];

    return (int)p;
}

int fs_deserialize(const uint8_t* buf, uint32_t buf_size) {
    if (!buf) return -1;
    if (buf_size < 8) return -1;
    if (buf[0] != 'T' || buf[1] != 'J' || buf[2] != 'F' || buf[3] != 'S') return -1;
    if (buf[4] != STORE_VERSION) return -1;

    uint32_t p = 8;
    int i, j;

    /* Rebuild file storage from scratch. */
    for (i = 0; i < MAX_F; i++) {
        fpool_free(fdata[i]);
        fdata[i] = 0;
        fsize[i] = 0;
    }
    fpool_init();

    if (buf_size < p + MAX_D + (MAX_D * FS_PATH_CAP) +
                   MAX_F + (MAX_F * 4) + (MAX_F * FS_PATH_CAP) + 256)
        return -1;

    for (i = 0; i < MAX_D; i++) dused[i] = buf[p++];
    for (i = 0; i < MAX_D; i++)
        for (j = 0; j < FS_PATH_CAP; j++) dname[i][j] = (char)buf[p++];

    for (i = 0; i < MAX_F; i++) fused[i] = buf[p++];
    for (i = 0; i < MAX_F; i++) {
        uint32_t sz = (uint32_t)buf[p]
                    | ((uint32_t)buf[p + 1] << 8)
                    | ((uint32_t)buf[p + 2] << 16)
                    | ((uint32_t)buf[p + 3] << 24);
        if (sz >= MAX_FILE_DATA) return -1;
        fsize[i] = (int)sz;
        p += 4;
    }

    for (i = 0; i < MAX_F; i++)
        for (j = 0; j < FS_PATH_CAP; j++) fname[i][j] = (char)buf[p++];

    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) {
            fsize[i] = 0;
            continue;
        }
        if (p + (uint32_t)fsize[i] > buf_size) return -1;
        if (fdata_resize(i, (uint32_t)fsize[i]) != 0) return -1;
        for (j = 0; j < fsize[i]; j++)
            fdata[i][j] = (char)buf[p++];
        fdata[i][fsize[i]] = 0;
    }

    if (p + 256 > buf_size) return -1;
    for (j = 0; j < 256; j++) cwd[j] = (char)buf[p++];

    return 0;
}