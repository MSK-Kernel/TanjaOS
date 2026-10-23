#include "../include/fs.h"

// Hook into store.c. It's always linked in; it no-ops internally if no
// disk-backed Storefile is active. Declared extern here (rather than
// pulled in via a header) to avoid fs.h depending on store.h.
extern void store_autosave(void);
extern void boot_log(const char* msg);

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
/* Per-file capacity. The largest shipped home/ files (the Torah books
 * up to ~200KB; the tcc compiler at ~281KB) must fit in one slot, or
 * fs_seed_home silently skips them and datareset can never restore them.
 * 320KB covers them with headroom. NOTE: fs.c BSS and the Storefile buffer in store.c
 * scale with this (see fs_store_size / STORE_BUF_SECTORS). */
#define MAX_FILE_DATA (4u * 1048576)
#define FS_PATH_CAP 256

/* Keep the filesystem metadata small on low-RAM machines.
 *
 * The old implementation reserved MAX_F * MAX_FILE_DATA bytes in .bss
 * (about 30 MiB) even when almost every file was empty. That made the
 * kernel need a huge amount of RAM just to boot simple commands.
 *
 * Files now live in a compact 8 MiB first-fit pool and are allocated only
 * when they actually contain data. MAX_FILE_DATA is still the per-file
 * limit, so normal large files (including TCC source/header files) keep
 * working without reserving 30+ MiB up front.
 */
#define FS_DATA_POOL_SIZE (16u * 1024u * 1024u)

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

/* ownership & permissions
 * Every file and directory carries a UNIX-style owner uid and a
 * 9-bit mode (rwx for owner/group/other; TanjaOS has no groups
 * yet, so the group bits are stored but not enforced). The
 * current effective uid lives in fs_cur_uid (0 = root = bypass).
 * root bypasses all checks; the owner uses the owner bits and
 * everyone else the other bits. Login sets fs_cur_uid; sudo
 * switches it to 0 for one command. */
static int fowner[MAX_F];
static int fmode[MAX_F];
static int downer[MAX_D];
static int dmode[MAX_D];

int fs_cur_uid = 0;   /* 0 = root: full access (boot, login sets it) */

/* Set by the FS layer when an operation is refused by the permission
 * system. Commands report it themselves, GNU-style:
 * rc == -2 -> "Permission denied"
 * fs_errno keeps 13 (EACCES) so callers can use strerror() too. */
int fs_errno = 0;

static void fs_deny(const char* path) {
    (void)path;
    fs_errno = 13;
}

static int file_index(const char* abs) {
    int i;
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], abs)) return i;
    return -1;
}
static int dir_index(const char* abs) {
    int i;
    for (i = 0; i < MAX_D; i++)
        if (dused[i] && strcmp_safe(dname[i], abs)) return i;
    return -1;
}

/* 1 if the current user has `bit' on this entry (mode/owner pair) */
static int perm_allows(int mode, int owner, int bit) {
    if (fs_cur_uid == 0) return 1;
    if (fs_cur_uid == owner) return (mode & (bit >> 0)) != 0;
    return (mode & (bit >> 6)) != 0;   /* other = shifted owner bits */
}

/* split "/a/b/c" into parent "/a/b" and name "c" */
static void split_parent(const char* abs, char* parent, int cap) {
    int len = 0;
    while (abs[len]) len++;
    while (len > 1 && abs[len - 1] == '/') len--;
    int cut = len;
    while (cut > 0 && abs[cut - 1] != '/') cut--;
    if (cut == 0) { parent[0] = '/'; parent[1] = 0; return; }
    int pl = cut - 1; if (pl < 1) pl = 1;   /* parent of "/x" is "/" */
    if (pl >= cap) pl = cap - 1;
    int i;
    for (i = 0; i < pl; i++) parent[i] = abs[i];
    parent[pl] = 0;
}

/* write access to the PARENT directory of abs (for create/delete) */
static int may_modify_parent(const char* abs) {
    char parent[FS_PATH_CAP];
    split_parent(abs, parent, sizeof(parent));
    int d = dir_index(parent);
    if (d < 0) return 1;                 /* no dir entry: nothing to check */
    if (perm_allows(dmode[d], downer[d], 0200)) return 1;
    fs_deny(parent);
    return 0;
}

static char cwd[256];

/* Small private allocator for file contents. It never calls libc malloc,
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



/* Built-in source-tree home/ archive. The build embeds home/ as a tar
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
            if (!fs_file_exists(path)) {
                if (size > MAX_FILE_DATA - 1) {
                    /* be explicit instead of silently vanishing at boot */
                    boot_log("home: file too large, skipped: ");
                    boot_log(path);
                } else if (fs_create_file(path) == 0) {
                    fs_write_file(path, (const char*)(p + 512), size);
                    /* Command binaries under /bin are executable
 * programs - seed them 0755 like a real system's
 * /bin, so ls colors them by their +x bit. */
                    if (path[0] == '/' && path[1] == 'b' &&
                        path[2] == 'i' && path[3] == 'n' &&
                        (path[4] == '/' || path[4] == 0))
                        fs_chmod(path, 0755);
                }
            }
        }

        uint32_t blocks = (size + 511) / 512;
        if (p + 512 + blocks * 512 > end) break;
        p += 512 + blocks * 512;
    }

    /* /home holds the per-user home directories created by useradd
       (and by the setup wizard).  home.tar cannot reliably carry
       empty directories, so /home is created here - after every
       init/seed, including datareset's factory restore.  Root owns
       it; users get their own /home/<name> when their account is
       created. */
    if (!fs_directory_exists("/home"))
        fs_create_directory("/home");

    /* root's home: /root, private (0700), with the standard user
       folders inside it.  root exists from the moment the system
       boots, so this happens on every init/seed. */
    fs_create_home_tree("/root", 0);
}

void fs_init(void) {
    int i;
    fpool_init();
    for (i = 0; i < MAX_F; i++) { fused[i] = 0; fsize[i] = 0; fdata[i] = 0; fname[i][0] = 0;
                                  fowner[i] = 0; fmode[i] = 0755; }
    for (i = 0; i < MAX_D; i++) { dused[i] = 0; dname[i][0] = 0;
                                  downer[i] = 0; dmode[i] = 0755; }
    dused[0] = 1;
    strcpy_safe(dname[0], "/", FS_PATH_CAP);
    strcpy_safe(cwd, "/", 256);
}

/* Build a canonical absolute path. This is deliberately done inside the
 * filesystem layer so EVERY command gets . and .. support automatically:
 * ../file -> parent of cwd
 * ../../file -> two parents up
 * ./file -> file in cwd
 * /a/../b -> /b
 * /../../x -> /x (cannot go above root)
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

    /* Store component start offsets. MAX_PATH_DEPTH is deliberately
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
    fs_errno = 0;
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
    if (!may_modify_parent(full)) return -2;
    for (i = 1; i < MAX_D; i++) {
        if (!dused[i]) {
            strcpy_safe(dname[i], full, FS_PATH_CAP);
            dused[i] = 1;
            downer[i] = fs_cur_uid;
            dmode[i] = 0755;
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
    fs_errno = 0;
    char full[FS_PATH_CAP];
    if (path && path[0]) {
        abs_path(path, full);
        if (!may_modify_parent(full)) return -2;
    }
    if (!path || !path[0]) return -1;
    if (strcmp_safe(path, "/")) return -1;
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
    fs_errno = 0;
    if (path && path[0]) {
        char full[FS_PATH_CAP];
        abs_path(path, full);
        if (!may_modify_parent(full)) return -2;
    }
    if (!path || !path[0]) return -1;
    if (!fs_directory_exists(path)) return -1;

    /* rm -rf used to call store_autosave() once per child. Batch the whole
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
    fs_errno = 0;
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
    if (!may_modify_parent(full)) return -2;
    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) {
            strcpy_safe(fname[i], full, FS_PATH_CAP);
            fsize[i] = 0;
            fdata[i] = 0;
            fused[i] = 1;
            fowner[i] = fs_cur_uid;
            fmode[i] = 0644;
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
    fs_errno = 0;
    if (!path || !path[0]) return -1;
    char full[256];
    abs_path(path, full);
    if (!may_modify_parent(full)) return -2;
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
    fs_errno = 0;
    if (!path || !data) return -1;
    char full[256];
    abs_path(path, full);
    int idx = -1, i;
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full)) { idx = i; break; }
    if (idx != -1) {
        /* existing file: need write permission on the file itself */
        if (!perm_allows(fmode[idx], fowner[idx], 0200)) {
            fs_deny(full);
            return -2;
        }
    } else {
        /* creation goes through fs_create_file, which checks the
 * parent directory for write access */
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

/* Append bytes to a file (create if missing). Lets commands copy
 * arbitrarily large files in small chunks without a whole-file buffer:
 * fdata_resize reallocates with the old bytes preserved, then the new
 * chunk lands at the old end. Write permission is checked like
 * fs_write_file (parent dir w for create, file w for append). */
int fs_append_file(const char* path, const char* data, uint32_t len)
{
    fs_errno = 0;
    if (!path || !path[0] || !data || len == 0) return -1;
    {
        char full[FS_PATH_CAP];
        int i, idx = -1;
        uint32_t oldsz;
        abs_path(path, full);

        for (i = 0; i < MAX_F; i++)
            if (fused[i] && strcmp_safe(fname[i], full)) { idx = i; break; }

        if (idx < 0) {
            int cr = fs_create_file(path);
            if (cr != 0) return cr;
            for (i = 0; i < MAX_F; i++)
                if (fused[i] && strcmp_safe(fname[i], full)) { idx = i; break; }
            if (idx < 0) return -1;
            oldsz = 0;
        } else {
            if (!perm_allows(fmode[idx], fowner[idx], 0200)) {
                fs_deny(full);
                return -2;
            }
            oldsz = (uint32_t)(fsize[idx] < 0 ? 0 : fsize[idx]);
        }

        if (oldsz + len > MAX_FILE_DATA - 1) return -3;   /* full pool */

        if (fdata_resize(idx, oldsz + len) != 0) return -4;
        {
            uint32_t k;
            for (k = 0; k < len; k++)
                fdata[idx][oldsz + k] = data[k];
        }
        fdata[idx][oldsz + len] = 0;
        return 0;
    }
}

int fs_read_file(const char* path, char* buffer, uint32_t* size) {
    fs_errno = 0;
    /* Callers pass their buffer capacity in *size. Never write more
       than that: the old implementation copied the WHOLE file into the
       caller's buffer and smashed the stack of any command holding a
       small buffer (grep's 4KB buffer crashed on the ~200KB Torah
       files).  Reads at most capacity-1 bytes, reports the actual
       byte count in *size. */
    if (!path || !buffer || !size) return -1;
    return fs_read_file_prefix(path, buffer, *size + 1, size);
}

int fs_read_file_prefix(const char* path, char* buffer, uint32_t capacity, uint32_t* size) {
    fs_errno = 0;
    if (!path || !buffer || !size || capacity == 0) return -1;
    *size = 0;
    buffer[0] = 0;

    char full[256];
    abs_path(path, full);

    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && strcmp_safe(fname[i], full)) {
            if (!perm_allows(fmode[i], fowner[i], 0400)) {
                fs_deny(full);
                return -2;
            }
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
    fs_errno = 0;
    if (!path || !buffer || !size || capacity == 0) return -1;
    *size = 0;
    char full[FS_PATH_CAP];
    abs_path(path, full);

    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && strcmp_safe(fname[i], full)) {
            if (!perm_allows(fmode[i], fowner[i], 0400)) {
                fs_deny(full);
                return -2;
            }
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
    fs_errno = 0;
    if (!path || !path[0]) return 0;
    char full[FS_PATH_CAP];
    abs_path(path, full);

    int i;
    for (i = 0; i < MAX_F; i++)
        if (fused[i] && strcmp_safe(fname[i], full)) {
            if (!perm_allows(fmode[i], fowner[i], 0400)) {
                fs_deny(full);
                return -2;
            }
            return (uint32_t)fsize[i];
        }

    return 0;
}


int fs_list_directory(const char* path, char* buf, uint32_t* size) {
    fs_errno = 0;
    if (!buf || !size) return -1;
    *size = 0; buf[0] = 0;
    
    char lp[256];
    if (path && path[0]) abs_path(path, lp);
    else strcpy_safe(lp, cwd, 256);
    
    /* A missing path is an error, not an empty directory. Previously
 * ls could silently succeed because this function simply found zero
 * matching children and returned an empty listing. */
    if (!fs_directory_exists(lp))
        return -1;

    /* directory listings need read permission on the directory */
    {
        int d = dir_index(lp);
        if (d >= 0 && !perm_allows(dmode[d], downer[d], 0400)) {
            fs_deny(lp);
            return -2;
        }
    }

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
    fs_errno = 0;
    if (!path || !path[0]) {
        strcpy_safe(cwd, "/", 256);
        return 0;
    }

    char full[256];
    abs_path(path, full);

    int i;
    for (i = 0; i < MAX_D; i++) {
        if (dused[i] && strcmp_safe(dname[i], full)) {
            if (!perm_allows(dmode[i], downer[i], 0100)) {
                fs_deny(full);
                return -2;
            }
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

// STOREFILE SERIALIZATION
//
// Packs the static fname/fdata/fsize/fused/dname/dused/cwd tables into
// a flat blob (and back). Layout, in order:
// 4 bytes magic "TJFS"
// 1 byte version
// 3 bytes reserved/padding
// MAX_D bytes dused[]
// MAX_D*64 bytes dname[][64]
// MAX_F bytes fused[]
// MAX_F*4 bytes fsize[] (little-endian)
// MAX_F*64 bytes fname[][64]
// an 8 MiB on-demand file-data pool
// 256 bytes cwd

#define STORE_VERSION 4

/* Storefile format v4 is intentionally sparse. The old format reserved
 * MAX_FILE_DATA bytes for EVERY file slot, so a single mkdir/rm caused a
 * ~31 MB serialize + disk write even when the filesystem only contained a
 * few KB. v4 keeps the fixed directory/file metadata but stores only the
 * bytes belonging to files that are actually in use.
 *
 * Layout:
 * 8 magic/version
 * MAX_D directory used flags
 * MAX_D*256 directory names
 * MAX_F file used flags
 * MAX_F*4 file sizes
 * MAX_F*256 file names
 * sum(fsize[i]) file data for used files, in slot order
 * 256 cwd
 */
/* ownership API
 * chmod: owner or root. chown: root only. The kernel's user-
 * management builtins call these; they return 0 on success. */
int fs_chmod(const char* path, int mode) {
    if (!path || !path[0]) return -1;
    mode &= 0777;
    char full[FS_PATH_CAP];
    abs_path(path, full);
    int f = file_index(full);
    if (f >= 0) {
        if (fs_cur_uid != 0 && fs_cur_uid != fowner[f]) return -1;  /* owner or root */
        fmode[f] = mode;
        fs_commit_change();
        return 0;
    }
    int d = dir_index(full);
    if (d >= 0) {
        if (fs_cur_uid != 0 && fs_cur_uid != downer[d]) return -1;
        dmode[d] = mode;
        fs_commit_change();
        return 0;
    }
    return -1;
}

int fs_chown(const char* path, int uid) {
    if (!path || !path[0]) return -1;
    if (fs_cur_uid != 0) return -1;       /* only root may chown */
    char full[FS_PATH_CAP];
    abs_path(path, full);
    int f = file_index(full);
    if (f >= 0) { fowner[f] = uid; fs_commit_change(); return 0; }
    int d = dir_index(full);
    if (d >= 0) { downer[d] = uid; fs_commit_change(); return 0; }
    return -1;
}

int fs_get_owner(const char* path) {
    if (!path || !path[0]) return -1;
    char full[FS_PATH_CAP];
    abs_path(path, full);
    int f = file_index(full);
    if (f >= 0) return fowner[f];
    int d = dir_index(full);
    if (d >= 0) return downer[d];
    return -1;
}

int fs_get_mode(const char* path) {
    if (!path || !path[0]) return -1;
    char full[FS_PATH_CAP];
    abs_path(path, full);
    int f = file_index(full);
    if (f >= 0) return fmode[f];
    int d = dir_index(full);
    if (d >= 0) return dmode[d];
    return -1;
}

/* Create a user's home directory tree: the directory itself (owned
 * by the user, mode 0700) plus the standard subfolders inside it.
 * Used for /root at boot and for every useradd. */
int fs_create_home_tree(const char* base, int uid) {
    static const char* const subdirs[] = {
        "Documents", "Programs", "Projects", "Trash", "Scripts"
    };
    int i;
    int saved = fs_cur_uid;
    fs_cur_uid = 0;                       /* create as root */
    if (!fs_directory_exists(base))
        if (fs_create_directory(base) != 0) { fs_cur_uid = saved; return -1; }
    {
        char p[FS_PATH_CAP];
        int bl = strlen_safe(base);
        if (base[bl - 1] == '/') bl--;
        for (i = 0; i < 5; i++) {
            int len = bl, j;
            for (j = 0; j < 256 && base[j]; j++) p[j] = base[j];
            if (base[0] != '/') { p[0] = '/'; len = 1;
                for (j = 0; j < bl; j++) p[1 + j] = base[j]; len = 1 + bl; }
            if (p[len - 1] != '/') p[len++] = '/';
            for (j = 0; subdirs[i][j]; j++) p[len++] = subdirs[i][j];
            p[len] = 0;
            if (!fs_directory_exists(p))
                fs_create_directory(p);
            /* chown to the user, private (0700) */
            fs_chown(p, uid);
            fs_chmod(p, 0700);
        }
    }
    fs_chown(base, uid);
    fs_chmod(base, 0700);
    fs_cur_uid = saved;
    return 0;
}

/* 1 if abs path `child` lives under directory `dir` (or equals it) */
static int path_under(const char* child, const char* dir) {
    int dl = strlen_safe(dir);
    if (dl && dir[dl - 1] == '/') dl--;
    int i;
    for (i = 0; i < dl; i++) if (child[i] != dir[i]) return 0;
    if (child[dl] == 0) return 1;
    if (dl > 1 && child[dl] == '/') return 1;
    if (dl == 1 && dir[0] == '/' && child[0] == '/') return 1;
    return 0;
}

/* chmod applied to `path` and everything underneath it (chmod -R).
 * Each entry still obeys the owner-or-root rule; entries the caller
 * may not change are skipped. Returns the number of entries
 * changed, or -1 if the target itself does not exist. */
int fs_chmod_recursive(const char* path, int mode) {
    if (!path || !path[0]) return -1;
    mode &= 0777;
    char full[FS_PATH_CAP];
    abs_path(path, full);
    int base = fs_chmod(full, mode);   /* the target itself */
    if (base != 0) return -1;
    int count = 1, i;
    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) continue;
        if (!path_under(fname[i], full)) continue;
        if (fs_cur_uid == 0 || fs_cur_uid == fowner[i]) {
            fmode[i] = mode;
            count++;
        }
    }
    for (i = 0; i < MAX_D; i++) {
        if (!dused[i]) continue;
        if (!path_under(dname[i], full)) continue;
        if (fs_cur_uid == 0 || fs_cur_uid == downer[i]) {
            dmode[i] = mode;
            count++;
        }
    }
    fs_commit_change();
    return count;
}

/* chown -R: root only; the target and everything under it becomes
 * owned by `uid`. */
int fs_chown_recursive(const char* path, int uid) {
    if (!path || !path[0]) return -1;
    if (fs_cur_uid != 0) return -1;
    char full[FS_PATH_CAP];
    abs_path(path, full);
    int base = fs_chown(full, uid);
    if (base != 0) return -1;
    int count = 1, i;
    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) continue;
        if (path_under(fname[i], full)) { fowner[i] = uid; count++; }
    }
    for (i = 0; i < MAX_D; i++) {
        if (!dused[i]) continue;
        if (path_under(dname[i], full)) { downer[i] = uid; count++; }
    }
    fs_commit_change();
    return count;
}

/* hand every file in the filesystem owned by `old_uid` over to
 * `new_uid` (used by usermod -u, like real usermod re-owning the
 * user's files). Returns how many entries changed. */
int fs_reassign_uid(int old_uid, int new_uid) {
    int count = 0, i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && fowner[i] == old_uid) { fowner[i] = new_uid; count++; }
    }
    for (i = 0; i < MAX_D; i++) {
        if (dused[i] && downer[i] == old_uid) { downer[i] = new_uid; count++; }
    }
    if (count) fs_commit_change();
    return count;
}

uint32_t fs_store_size(void) {
    return 8
        + MAX_D
        + (MAX_D * FS_PATH_CAP)
        + MAX_F
        + (MAX_F * 4)
        + (MAX_F * FS_PATH_CAP)
        + FS_DATA_POOL_SIZE
        + 256
        + (MAX_D * 6)      /* downer(4) + dmode(2) */
        + (MAX_F * 6);     /* fowner(4) + fmode(2) */
}

uint32_t fs_serialized_size(void) {
    uint32_t need = 8
        + MAX_D
        + (MAX_D * FS_PATH_CAP)
        + MAX_F
        + (MAX_F * 4)
        + (MAX_F * FS_PATH_CAP)
        + 256
        + (MAX_D * 6)
        + (MAX_F * 6);
    int i;
    for (i = 0; i < MAX_F; i++) {
        if (fused[i] && fsize[i] > 0)
            need += (uint32_t)fsize[i];
    }
    return need;
}

/* Streaming serializer: emits the v4 Storefile image piece by piece
 * through a caller sink. The store layer streams these bytes straight
 * to disk in small chunks, so no full-image RAM buffer is needed any
 * more. The layout is IDENTICAL to the historical buffer-based format
 * (fs_serialize below is a thin wrapper). */
typedef void (*fs_emit_fn)(void* ctx, const uint8_t* data, uint32_t len);

static void fs_emit_le32(void* ctx, fs_emit_fn emit, uint32_t v)
{
    uint8_t t[4];
    t[0] = (uint8_t)(v & 0xFF);
    t[1] = (uint8_t)((v >> 8) & 0xFF);
    t[2] = (uint8_t)((v >> 16) & 0xFF);
    t[3] = (uint8_t)((v >> 24) & 0xFF);
    emit(ctx, t, 4);
}

int fs_serialize_sink(void* ctx, fs_emit_fn emit)
{
    uint8_t hdr[8];
    int i;

    hdr[0] = 'T'; hdr[1] = 'J'; hdr[2] = 'F'; hdr[3] = 'S';
    hdr[4] = STORE_VERSION; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    emit(ctx, hdr, 8);

    for (i = 0; i < MAX_D; i++) {
        uint8_t b = (uint8_t)dused[i];
        emit(ctx, &b, 1);
    }
    for (i = 0; i < MAX_D; i++)
        emit(ctx, (const uint8_t*)dname[i], FS_PATH_CAP);

    for (i = 0; i < MAX_F; i++) {
        uint8_t b = (uint8_t)fused[i];
        emit(ctx, &b, 1);
    }
    for (i = 0; i < MAX_F; i++) {
        uint32_t sz = (fsize[i] < 0) ? 0u : (uint32_t)fsize[i];
        fs_emit_le32(ctx, emit, sz);
    }
    for (i = 0; i < MAX_F; i++)
        emit(ctx, (const uint8_t*)fname[i], FS_PATH_CAP);

    for (i = 0; i < MAX_F; i++) {
        if (!fused[i] || fsize[i] <= 0) continue;
        emit(ctx, (const uint8_t*)fdata[i], (uint32_t)fsize[i]);
    }

    emit(ctx, (const uint8_t*)cwd, 256);

    /* ownership metadata (appended; older images lack it - see the
 * deserialize note). Modes exceed one byte, 2 bytes LE. */
    for (i = 0; i < MAX_D; i++) {
        fs_emit_le32(ctx, emit, (uint32_t)downer[i]);
        {
            uint8_t t[2];
            t[0] = (uint8_t)(dmode[i] & 0xFF);
            t[1] = (uint8_t)((dmode[i] >> 8) & 0xFF);
            emit(ctx, t, 2);
        }
    }
    for (i = 0; i < MAX_F; i++) {
        fs_emit_le32(ctx, emit, (uint32_t)fowner[i]);
        {
            uint8_t t[2];
            t[0] = (uint8_t)(fmode[i] & 0xFF);
            t[1] = (uint8_t)((fmode[i] >> 8) & 0xFF);
            emit(ctx, t, 2);
        }
    }
    return 0;
}

/* buffer wrapper: same bytes as the historical implementation */
typedef struct { uint8_t* p; } fs_buf_sink_ctx;
static void fs_buf_sink(void* vctx, const uint8_t* data, uint32_t len)
{
    fs_buf_sink_ctx* ctx = (fs_buf_sink_ctx*)vctx;
    uint32_t k;
    for (k = 0; k < len; k++) *ctx->p++ = data[k];
}

int fs_serialize(uint8_t* buf, uint32_t buf_size) {
    if (!buf) return -1;
    uint32_t need = fs_serialized_size();
    if (buf_size < need) return -1;
    fs_buf_sink_ctx ctx = { buf };
    fs_serialize_sink(&ctx, fs_buf_sink);
    return (int)need;
}

/* Streaming deserializer: pulls the image through a caller fill
 * function. fill returns -1 when the image ends (older images lack
 * the ownership section, so a short read there is not an error). */
typedef int (*fs_fill_fn)(void* ctx, uint8_t* dst, uint32_t len);

int fs_deserialize_fill(void* ctx, fs_fill_fn fill)
{
    uint8_t hdr[8];
    uint8_t tmp4[4];
    uint8_t tmp2[2];
    uint8_t used[MAX_F > MAX_D ? MAX_F : MAX_D];
    int i, j;

    if (!fill) return -1;
    if (fill(ctx, hdr, 8) != 0) return -1;
    if (hdr[0] != 'T' || hdr[1] != 'J' || hdr[2] != 'F' || hdr[3] != 'S') return -1;
    if (hdr[4] != STORE_VERSION) return -1;

    /* Rebuild file storage from scratch. */
    for (i = 0; i < MAX_F; i++) {
        fpool_free(fdata[i]);
        fdata[i] = 0;
        fsize[i] = 0;
    }
    fpool_init();

    if (fill(ctx, used, MAX_D) != 0) return -1;
    for (i = 0; i < MAX_D; i++) dused[i] = used[i];

    for (i = 0; i < MAX_D; i++)
        if (fill(ctx, (uint8_t*)dname[i], FS_PATH_CAP) != 0) return -1;

    if (fill(ctx, used, MAX_F) != 0) return -1;
    for (i = 0; i < MAX_F; i++) fused[i] = used[i];

    for (i = 0; i < MAX_F; i++) {
        if (fill(ctx, tmp4, 4) != 0) return -1;
        {
            uint32_t sz = (uint32_t)tmp4[0] | ((uint32_t)tmp4[1] << 8)
                        | ((uint32_t)tmp4[2] << 16) | ((uint32_t)tmp4[3] << 24);
            if (sz >= MAX_FILE_DATA) return -1;
            fsize[i] = (int)sz;
        }
    }

    for (i = 0; i < MAX_F; i++)
        if (fill(ctx, (uint8_t*)fname[i], FS_PATH_CAP) != 0) return -1;

    for (i = 0; i < MAX_F; i++) {
        if (!fused[i]) {
            fsize[i] = 0;
            continue;
        }
        if (fdata_resize(i, (uint32_t)fsize[i]) != 0) return -1;
        if (fill(ctx, (uint8_t*)fdata[i], (uint32_t)fsize[i]) != 0) return -1;
        fdata[i][fsize[i]] = 0;
    }

    if (fill(ctx, (uint8_t*)cwd, 256) != 0) return -1;

    /* ownership metadata: appended section. Older Storefiles lack
 * it entirely; those files become root-owned 0755 (safe
 * default: only root could have created them anyway, since the
 * permission system did not exist yet). */
    for (i = 0; i < MAX_F; i++) { fowner[i] = 0; fmode[i] = 0755; }
    for (i = 0; i < MAX_D; i++) { downer[i] = 0; dmode[i] = 0755; }
    {
        int have_owners = 1;
        for (i = 0; i < MAX_D && have_owners; i++) {
            if (fill(ctx, tmp4, 4) != 0) { have_owners = 0; break; }
            downer[i] = (int)((uint32_t)tmp4[0] | ((uint32_t)tmp4[1] << 8)
                     | ((uint32_t)tmp4[2] << 16) | ((uint32_t)tmp4[3] << 24));
            if (fill(ctx, tmp2, 2) != 0) { have_owners = 0; break; }
            dmode[i] = (int)tmp2[0] | ((int)tmp2[1] << 8);
        }
        for (i = 0; i < MAX_F && have_owners; i++) {
            if (fill(ctx, tmp4, 4) != 0) { have_owners = 0; break; }
            fowner[i] = (int)((uint32_t)tmp4[0] | ((uint32_t)tmp4[1] << 8)
                     | ((uint32_t)tmp4[2] << 16) | ((uint32_t)tmp4[3] << 24));
            if (fill(ctx, tmp2, 2) != 0) { have_owners = 0; break; }
            fmode[i] = (int)tmp2[0] | ((int)tmp2[1] << 8);
        }
    }
    return 0;
}

/* buffer wrapper: identical semantics to the historical reader */
typedef struct { const uint8_t* p; const uint8_t* end; } fs_buf_fill_ctx;
static int fs_buf_fill(void* vctx, uint8_t* dst, uint32_t len)
{
    fs_buf_fill_ctx* ctx = (fs_buf_fill_ctx*)vctx;
    if ((uint32_t)(ctx->end - ctx->p) < len) return -1;
    {
        uint32_t k;
        for (k = 0; k < len; k++) dst[k] = ctx->p[k];
    }
    ctx->p += len;
    return 0;
}

int fs_deserialize(const uint8_t* buf, uint32_t buf_size) {
    fs_buf_fill_ctx ctx;
    if (!buf) return -1;
    ctx.p = buf; ctx.end = buf + buf_size;
    return fs_deserialize_fill(&ctx, fs_buf_fill);
}

/* Zero-copy read access: pointer straight into the file-data pool.
 * The pool never moves existing allocations, so the pointer stays
 * valid while the file is not rewritten. Read permission is checked
 * exactly like fs_read_file; NULL + fs_errno=13 on denial. */
const char* fs_file_data_ptr(const char* path, uint32_t* size_out)
{
    fs_errno = 0;
    if (!path || !path[0]) return 0;
    {
        char full[256];
        int i;
        abs_path(path, full);
        for (i = 0; i < MAX_F; i++) {
            if (fused[i] && strcmp_safe(fname[i], full)) {
                if (!perm_allows(fmode[i], fowner[i], 0400)) {
                    fs_deny(full);
                    return 0;
                }
                if (size_out)
                    *size_out = (uint32_t)(fsize[i] < 0 ? 0 : fsize[i]);
                return fdata[i];
            }
        }
    }
    return 0;
}
