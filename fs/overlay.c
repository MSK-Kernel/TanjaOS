#include "../include/overlay.h"
#include "../include/iso9660.h"
#include "../include/ramfs.h"
#include "../include/vfs.h"

/* TanjaOS overlay root
 *
 * When the machine was booted from an ISO (a CD/DVD, or a hybrid ISO on a
 * disk), the files on that ISO show up in the root directory next to the
 * normal TanjaOS tree: /boot, /boot/grub/grub.cfg, /boot.catalog, ...
 *
 *   upper layer : ramfs  - writable, this is the normal TanjaOS root
 *   lower layer : the ISO, read-only (fs/iso9660.c)
 *
 * Reads see the upper layer first, then the ISO. The first time something
 * on the ISO is written, changed, chmod'ed or appended to, it is copied up
 * into ramfs (with its parent directories) and the edit lands on the copy.
 * Deleting an ISO file records a "whiteout" in RAM that hides it.
 *
 * Nothing is ever written to the ISO. On a live CD the ramfs is not
 * persisted, so every edit is gone after a reboot - on an installed system
 * with a Storefile the edited copies are saved like any other file (the
 * unmodified ISO files and the whiteouts are not).
 *
 * ramfs has a small fixed number of file/directory slots, which is why the
 * ISO is NOT copied in wholesale: only what is edited takes a slot.
 */

extern int fs_cur_uid;
extern int fs_errno;
extern void ramfs_abs_path(const char* in, char* out);

static const struct vfs_ops* U;            /* upper layer ops (ramfs) */
static int ovl_active;

/* string helpers                                                    */

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static int seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* p == w, or p lies below w */
static int path_under(const char* p, const char* w) {
    int i = 0;
    while (w[i] && p[i] == w[i]) i++;
    if (w[i]) return 0;
    return p[i] == 0 || p[i] == '/' || (i == 1 && w[0] == '/');
}

/* "/a/b/c" -> "/a/b" ; "/a" -> "/" */
static void parent_of(const char* p, char* out, int cap) {
    int len = slen(p), cut;
    while (len > 1 && p[len - 1] == '/') len--;
    cut = len;
    while (cut > 1 && p[cut - 1] != '/') cut--;
    if (cut <= 1) { out[0] = '/'; out[1] = 0; return; }
    if (cut - 1 >= cap) cut = cap;
    {
        int i;
        for (i = 0; i < cut - 1; i++) out[i] = p[i];
        out[cut - 1] = 0;
    }
}

/* whiteouts: ISO paths hidden by a delete                           */

#define WH_MAX  1024
#define WH_POOL 49152
static char     wh_pool[WH_POOL];
static uint16_t wh_off[WH_MAX];
static int      wh_n;
static uint32_t wh_used;
static int wh_loaded;
#define WH_FILE "/.tanja-overlay-whiteouts"

/* Whiteouts are filesystem state, not transient overlay state. Keep them in
 * ramfs so the normal Storefile serializer carries deletions across boots. */
static void wh_load(void) {
    static char saved[WH_POOL];
    uint32_t size = sizeof(saved) - 1;
    uint32_t pos = 0;
    if (wh_loaded || !U) return;
    wh_loaded = 1;
    if (!U->file_exists(0, WH_FILE) || !U->read_file) return;
    if (U->read_file(0, WH_FILE, saved, &size) != 0) return;
    if (size >= sizeof(saved)) size = sizeof(saved) - 1;
    saved[size] = 0;
    while (pos < size && wh_n < WH_MAX) {
        uint32_t start = pos;
        while (pos < size && saved[pos] != '\n') pos++;
        if (pos > start && pos - start < VFS_PATH_MAX &&
            wh_used + (pos - start) + 1 <= WH_POOL) {
            uint32_t len = pos - start;
            wh_off[wh_n++] = (uint16_t)wh_used;
            for (uint32_t k = 0; k < len; k++) wh_pool[wh_used + k] = saved[start + k];
            wh_pool[wh_used + len] = 0;
            wh_used += len + 1;
        }
        if (pos < size) pos++;
    }
}

static int wh_save(void) {
    static char saved[WH_POOL];
    uint32_t pos = 0;
    int i;
    if (!U || !U->write_file || !U->create_file) return -1;
    for (i = 0; i < wh_n; i++) {
        const char* p = wh_pool + wh_off[i];
        int k = 0;
        while (p[k] && pos + 2 < sizeof(saved)) saved[pos++] = p[k++];
        if (pos + 1 >= sizeof(saved)) return -1;
        saved[pos++] = '\n';
    }
    saved[pos] = 0;
    if (!U->file_exists(0, WH_FILE) && U->create_file(0, WH_FILE) != 0) return -1;
    return U->write_file(0, WH_FILE, saved, pos);
}

static int is_blocked(const char* p) {
    int i;
    wh_load();
    for (i = 0; i < wh_n; i++)
        if (path_under(p, wh_pool + wh_off[i])) return 1;
    return 0;
}

static int wh_add(const char* p) {
    int len = slen(p), i, nn = 0;
    uint32_t used = 0;
    wh_load();
    if (is_blocked(p)) return 0;

    /* a whiteout on a directory covers everything below it: drop the
     * now-redundant entries (in place - writes never overtake reads) */
    for (i = 0; i < wh_n; i++) {
        const char* e = wh_pool + wh_off[i];
        int el = slen(e);
        if (el > len && path_under(e, p)) continue;
        {
            uint16_t no = (uint16_t)used;
            int k;
            for (k = 0; k <= el; k++) wh_pool[used + k] = e[k];
            wh_off[nn++] = no;
            used += el + 1;
        }
    }
    wh_n = nn;
    wh_used = used;

    if (wh_n >= WH_MAX || wh_used + len + 1 > WH_POOL) return -1;
    wh_off[wh_n++] = (uint16_t)wh_used;
    scopy(wh_pool + wh_used, p, len + 1);
    wh_used += len + 1;
    return wh_save();
}

/* layer queries                                                     */

static int u_file(const char* p) { return U->file_exists(0, p); }
static int u_dir(const char* p)  { return U->dir_exists(0, p); }

/* 0 = visible on the ISO */
static int l_stat(const char* p, struct iso_node* n) {
    if (!iso_available() || is_blocked(p)) return -1;
    return iso_lookup(p, n);
}

static int l_perm(const struct iso_node* n, int bit) {
    if (fs_cur_uid == 0) return 1;
    return (n->mode & (bit >> 6)) != 0;       /* ISO files belong to root */
}

static int o_file_exists(void* sb, const char* p) {
    struct iso_node n;
    (void)sb;
    if (u_file(p)) return 1;
    if (u_dir(p)) return 0;
    return l_stat(p, &n) == 0 && !n.is_dir;
}

static int o_dir_exists(void* sb, const char* p) {
    struct iso_node n;
    (void)sb;
    if (u_dir(p)) return 1;
    if (u_file(p)) return 0;
    return l_stat(p, &n) == 0 && n.is_dir;
}

/* copy-up                                                           */

/* Make sure every parent directory of p that exists on the ISO also exists
 * in ramfs. Done as root: it is bookkeeping, not the user's own write. */
static int ensure_parents(const char* p) {
    char dirs[32][VFS_PATH_MAX > 128 ? 128 : VFS_PATH_MAX];
    char cur[256];
    int depth = 0, i, rc = 0, saved;

    parent_of(p, cur, sizeof(cur));
    while (!(cur[0] == '/' && cur[1] == 0) && depth < 32) {
        scopy(dirs[depth++], cur, sizeof(dirs[0]));
        {
            char up[256];
            parent_of(cur, up, sizeof(up));
            scopy(cur, up, sizeof(cur));
        }
    }

    saved = fs_cur_uid;
    fs_cur_uid = 0;
    for (i = depth - 1; i >= 0; i--) {
        struct iso_node n;
        if (u_dir(dirs[i])) continue;
        if (l_stat(dirs[i], &n) != 0 || !n.is_dir) break;   /* missing: U fails */
        rc = U->create_dir(0, dirs[i]);
        if (rc != 0) break;
        U->chmod(0, dirs[i], n.mode);
    }
    fs_cur_uid = saved;
    return rc;
}

static int copy_up_file(const char* p, const struct iso_node* n) {
    static char chunk[32768];
    uint32_t off = 0;
    int rc, saved;

    rc = ensure_parents(p);
    if (rc != 0) return rc;

    saved = fs_cur_uid;
    fs_cur_uid = 0;
    rc = U->create_file(0, p);
    while (rc == 0 && off < n->size) {
        uint32_t got = 0;
        if (iso_read_range(n, off, chunk, sizeof(chunk), &got) != 0 || got == 0) {
            rc = -1;
            break;
        }
        rc = U->append_file(0, p, chunk, got);
        off += got;
    }
    if (rc == 0) U->chmod(0, p, n->mode);
    else if (U->file_exists(0, p)) U->delete_file(0, p);
    fs_cur_uid = saved;
    return rc;
}

/* Called before a write that bypasses the VFS ops (fs_write_at,
 * fs_prealloc_file): an ISO file is copied up, a new path gets its parent
 * directories. Takes an absolute path. 0 = ok. */
int ovl_prepare_write(const char* abs) {
    struct iso_node n;
    if (!ovl_active || !abs) return 0;
    if (u_file(abs)) return 0;
    if (l_stat(abs, &n) == 0 && !n.is_dir) {
        if (!l_perm(&n, 0200)) { fs_errno = 13; return -2; }
        return copy_up_file(abs, &n);
    }
    return ensure_parents(abs);
}

/* permission to add/remove entries in p's directory when that directory is
 * only on the ISO (ramfs checks it itself once the directory is copied up) */
static int parent_writable(const char* p) {
    char par[256];
    struct iso_node n;
    parent_of(p, par, sizeof(par));
    if (u_dir(par)) return 1;
    if (l_stat(par, &n) == 0 && n.is_dir && !l_perm(&n, 0200)) {
        fs_errno = 13;
        return 0;
    }
    return 1;
}

/* vfs ops                                                           */

static int o_create_file(void* sb, const char* p) {
    int rc;
    (void)sb;
    fs_errno = 0;
    if (o_file_exists(0, p)) return 0;
    if (o_dir_exists(0, p)) return -1;
    if (!parent_writable(p)) return -2;
    rc = ensure_parents(p);
    if (rc != 0) return rc;
    return U->create_file(0, p);
}

static int o_create_dir(void* sb, const char* p) {
    int rc;
    (void)sb;
    fs_errno = 0;
    if (o_dir_exists(0, p)) return 0;
    if (o_file_exists(0, p)) return -1;
    if (!parent_writable(p)) return -2;
    rc = ensure_parents(p);
    if (rc != 0) return rc;
    return U->create_dir(0, p);
}

/* Prepare to replace the content of p completely (no need to copy it). */
static int prepare_replace(const char* p) {
    struct iso_node n;
    int rc, saved;
    if (u_file(p)) return 0;
    if (l_stat(p, &n) == 0 && !u_dir(p)) {
        if (n.is_dir) return -1;
        if (!l_perm(&n, 0200)) { fs_errno = 13; return -2; }
        rc = ensure_parents(p);
        if (rc != 0) return rc;
        saved = fs_cur_uid;
        fs_cur_uid = 0;
        rc = U->create_file(0, p);
        if (rc == 0) U->chmod(0, p, n.mode);
        fs_cur_uid = saved;
        return rc;
    }
    if (!parent_writable(p)) return -2;
    return ensure_parents(p);
}

static int o_write_file(void* sb, const char* p, const char* data, uint32_t size) {
    int rc;
    (void)sb;
    fs_errno = 0;
    rc = prepare_replace(p);
    if (rc != 0) return rc;
    return U->write_file(0, p, data, size);
}

static int o_append_file(void* sb, const char* p, const char* data, uint32_t len) {
    int rc;
    (void)sb;
    fs_errno = 0;
    rc = ovl_prepare_write(p);
    if (rc != 0) return rc;
    if (!u_file(p) && !parent_writable(p)) return -2;
    return U->append_file(0, p, data, len);
}

static int o_read_file(void* sb, const char* p, char* buf, uint32_t* size) {
    struct iso_node n;
    uint32_t cap, got = 0;
    (void)sb;
    fs_errno = 0;
    if (!buf || !size) return -1;
    if (u_file(p)) return U->read_file(0, p, buf, size);
    if (l_stat(p, &n) != 0 || n.is_dir) return -1;
    if (!l_perm(&n, 0400)) { fs_errno = 13; return -2; }
    cap = *size;                               /* caller: max payload bytes */
    if (iso_read_range(&n, 0, buf, cap, &got) != 0) { buf[0] = 0; *size = 0; return -1; }
    buf[got] = 0;
    *size = got;
    return 0;
}

static int o_read_file_range(void* sb, const char* p, uint32_t offset,
                             char* buf, uint32_t capacity, uint32_t* size) {
    struct iso_node n;
    (void)sb;
    fs_errno = 0;
    if (!buf || !size || capacity == 0) return -1;
    if (u_file(p)) return U->read_file_range(0, p, offset, buf, capacity, size);
    *size = 0;
    if (l_stat(p, &n) != 0 || n.is_dir) return -1;
    if (!l_perm(&n, 0400)) { fs_errno = 13; return -2; }
    if (iso_read_range(&n, offset, buf, capacity, size) != 0) return -1;
    return 0;
}

static uint32_t o_get_file_size(void* sb, const char* p) {
    struct iso_node n;
    (void)sb;
    if (u_file(p)) return U->get_file_size(0, p);
    if (l_stat(p, &n) == 0 && !n.is_dir && !n.is_link) return n.size;
    return 0;
}

/* ---- merged directory listing ---------------------------------- */

#define LIST_LIMIT 16000

/* is `name` (nl bytes, no slash) one of the entries in a listing? */
static int name_in(const char* list, uint32_t len, const char* name, int nl) {
    uint32_t i = 0;
    while (i < len) {
        uint32_t s = i, e;
        while (i < len && list[i] != '\n') i++;
        e = i;
        if (i < len) i++;
        if (e > s && list[e - 1] == '/') e--;
        if ((int)(e - s) == nl) {
            int k = 0;
            while (k < nl && list[s + k] == name[k]) k++;
            if (k == nl) return 1;
        }
    }
    return 0;
}

static int merged_list(const char* p, char* out, uint32_t cap, uint32_t* size) {
    static char ub[16384], lb[16384];
    uint32_t us = 0, ls = 0, pos = 0;
    int have_l = 0, pass;

    *size = 0;
    out[0] = 0;
    if (!o_dir_exists(0, p)) return -1;

    ub[0] = 0;
    if (u_dir(p)) U->list_dir(0, p, ub, &us);
    if (iso_available() && !is_blocked(p) && !u_file(p))
        have_l = (iso_list(p, lb, sizeof(lb), &ls) == 0);

    for (pass = 0; pass < 2; pass++) {          /* 0 = directories, 1 = files */
        int src;
        for (src = 0; src < 2; src++) {         /* 0 = ramfs, 1 = ISO */
            const char* lst = src ? lb : ub;
            uint32_t len = src ? ls : us;
            uint32_t i = 0;
            if (src && !have_l) continue;
            while (i < len) {
                uint32_t s = i, e;
                int is_dir, nl;
                while (i < len && lst[i] != '\n') i++;
                e = i;
                if (i < len) i++;
                if (e == s) continue;
                is_dir = (lst[e - 1] == '/');
                if (is_dir != (pass == 0)) continue;
                nl = (int)(e - s) - (is_dir ? 1 : 0);
                if (nl <= 0) continue;
                if (src) {
                    /* hidden by a delete, or shadowed by a ramfs entry? */
                    char full[300];
                    int m = 0, k;
                    for (k = 0; p[k] && m < 250; k++) full[m++] = p[k];
                    if (!(m == 1 && full[0] == '/')) full[m++] = '/';
                    for (k = 0; k < nl && m < 298; k++) full[m++] = lst[s + k];
                    full[m] = 0;
                    if (is_blocked(full)) continue;
                    if (name_in(ub, us, lst + s, nl)) continue;
                }
                if (pos + (uint32_t)(e - s) + 2 > cap) goto done;
                if (pos) out[pos++] = '\n';
                {
                    uint32_t k;
                    for (k = s; k < e; k++) out[pos++] = lst[k];
                }
                out[pos] = 0;
            }
        }
    }
done:
    *size = pos;
    return 0;
}

static int o_list_dir(void* sb, const char* p, char* out, uint32_t* size) {
    (void)sb;
    if (!p || !out || !size) return -1;
    return merged_list(p, out, LIST_LIMIT, size);
}

static int o_dir_is_empty(void* sb, const char* p) {
    char tmp[64];
    uint32_t sz = 0;
    (void)sb;
    if (!o_dir_exists(0, p)) return 0;
    /* a name longer than the probe buffer is still "not empty": the list
     * stops adding at the first entry that does not fit, so also ask the
     * layers directly */
    if (u_dir(p) && !U->dir_is_empty(0, p)) return 0;
    if (merged_list(p, tmp, sizeof(tmp), &sz) != 0) return 0;
    if (sz > 0) return 0;
    if (iso_available() && !is_blocked(p) && !u_file(p) && !iso_dir_is_empty(p)) {
        /* children exist on the ISO but none fit the probe buffer, or all
         * are whited out - take the full listing to know which */
        static char big[16384];
        uint32_t bs = 0;
        if (merged_list(p, big, sizeof(big), &bs) != 0) return 0;
        return bs == 0;
    }
    return 1;
}

/* ---- delete / attributes ---------------------------------------- */

static int o_delete_file(void* sb, const char* p) {
    struct iso_node n;
    int on_iso;
    (void)sb;
    fs_errno = 0;
    on_iso = (l_stat(p, &n) == 0 && !n.is_dir);
    if (u_file(p)) {
        int rc = U->delete_file(0, p);
        if (rc != 0) return rc;
        if (on_iso && wh_add(p) != 0) return -3;
        return 0;
    }
    if (on_iso) {
        if (!parent_writable(p)) return -2;
        return wh_add(p) == 0 ? 0 : -3;
    }
    return -1;
}

static int o_delete_dir(void* sb, const char* p) {
    struct iso_node n;
    (void)sb;
    fs_errno = 0;
    if (!p || !p[0] || seq(p, "/")) return -1;
    if (!o_dir_exists(0, p)) return -1;
    if (!o_dir_is_empty(0, p)) return -2;
    if (!parent_writable(p)) return -2;
    if (u_dir(p)) {
        int rc = U->delete_dir(0, p);
        if (rc != 0) return rc;
    }
    if (l_stat(p, &n) == 0 && n.is_dir && wh_add(p) != 0) return -3;
    return 0;
}

static int copy_up_dir(const char* p, const struct iso_node* n) {
    int rc, saved;
    rc = ensure_parents(p);
    if (rc != 0) return rc;
    saved = fs_cur_uid;
    fs_cur_uid = 0;
    rc = U->create_dir(0, p);
    if (rc == 0) U->chmod(0, p, n->mode);
    fs_cur_uid = saved;
    return rc;
}

/* 0 = p now exists in ramfs (it was already there, or was copied up) */
static int copy_up_any(const char* p) {
    struct iso_node n;
    if (u_file(p) || u_dir(p)) return 0;
    if (l_stat(p, &n) != 0) return -1;
    return n.is_dir ? copy_up_dir(p, &n) : copy_up_file(p, &n);
}

static int o_chmod(void* sb, const char* p, int mode) {
    (void)sb;
    if (!p || !p[0]) return -1;
    if (!u_file(p) && !u_dir(p)) {
        if (fs_cur_uid != 0) return -1;         /* ISO files belong to root */
        if (copy_up_any(p) != 0) return -1;
    }
    return U->chmod(0, p, mode);
}

static int o_chown(void* sb, const char* p, int uid) {
    (void)sb;
    if (!p || !p[0] || fs_cur_uid != 0) return -1;
    if (!u_file(p) && !u_dir(p) && copy_up_any(p) != 0) return -1;
    return U->chown(0, p, uid);
}

static int o_get_owner(void* sb, const char* p) {
    struct iso_node n;
    (void)sb;
    if (u_file(p) || u_dir(p)) return U->get_owner(0, p);
    return l_stat(p, &n) == 0 ? 0 : -1;
}

static int o_get_mode(void* sb, const char* p) {
    struct iso_node n;
    (void)sb;
    if (u_file(p) || u_dir(p)) return U->get_mode(0, p);
    return l_stat(p, &n) == 0 ? (int)n.mode : -1;
}

static struct vfs_ops ovl_ops;

/* Remove the RAM copy of p WITHOUT hiding the ISO original: afterwards the
 * file on the ISO (if there is one) shows through again. This is what
 * `pkg remove` wants for a package that replaced an ISO file - a plain
 * delete would whiteout the original instead. 0 = ok (also when there was
 * no RAM copy), -1 = nothing there at all. */
int ovl_revert(const char* abs) {
    struct iso_node n;
    if (!ovl_active || !abs) return -1;
    if (u_file(abs)) return U->delete_file(0, abs);
    return l_stat(abs, &n) == 0 && !n.is_dir ? 0 : -1;
}

/* ---- zero-copy view of an ISO file ------------------------------- */

/* exec and the Linux layer ask for a direct pointer to a file's bytes.
 * ramfs files have one; ISO files get a small RAM cache instead. Files
 * that are too big (or when memory is short) return NULL and the callers
 * fall back to reading through the normal file API. */
extern void* malloc(unsigned int n);
extern void free(void* p);

#define PTR_SLOTS    4
#define PTR_MAX_FILE (2u * 1024u * 1024u)
static struct { char path[128]; char* data; uint32_t size; } ptr_cache[PTR_SLOTS];
static int ptr_next;

int ovl_is_iso_only(const char* abs) {
    struct iso_node n;
    if (!ovl_active || !abs) return 0;
    if (u_file(abs) || u_dir(abs)) return 0;
    return l_stat(abs, &n) == 0 && !n.is_dir;
}

const char* ovl_iso_data_ptr(const char* abs, uint32_t* size_out) {
    struct iso_node n;
    int i;
    if (!ovl_is_iso_only(abs) || l_stat(abs, &n) != 0) return 0;
    if (!l_perm(&n, 0400)) { fs_errno = 13; return 0; }
    if (size_out) *size_out = n.is_link ? 0 : n.size;
    if (n.is_link || n.size == 0 || n.size > PTR_MAX_FILE || slen(abs) >= 128) return 0;

    for (i = 0; i < PTR_SLOTS; i++)
        if (ptr_cache[i].data && seq(ptr_cache[i].path, abs)) return ptr_cache[i].data;

    {
        char* d = (char*)malloc(n.size + 1);
        uint32_t got = 0;
        if (!d) return 0;
        if (iso_read_range(&n, 0, d, n.size, &got) != 0 || got != n.size) {
            free(d);
            return 0;
        }
        d[n.size] = 0;
        i = ptr_next;
        ptr_next = (ptr_next + 1) % PTR_SLOTS;
        if (ptr_cache[i].data) free(ptr_cache[i].data);
        scopy(ptr_cache[i].path, abs, 128);
        ptr_cache[i].data = d;
        ptr_cache[i].size = n.size;
        return d;
    }
}

/* ---- setup ------------------------------------------------------ */

int ovl_active_now(void) { return ovl_active; }

/* Look for an ISO; if there is one, put the overlay under the root mount.
 * Called from fs_init() after the block devices were probed. */
int ovl_init(void) {
    int i;
    ovl_active = 0;
    wh_n = 0;
    wh_used = 0;
    wh_loaded = 0;
    for (i = 0; i < PTR_SLOTS; i++) {
        if (ptr_cache[i].data) free(ptr_cache[i].data);
        ptr_cache[i].data = 0;
        ptr_cache[i].path[0] = 0;
    }
    ptr_next = 0;

    if (!iso_probe()) return 0;

    U = ramfs_get_ops();
    ovl_ops = *U;
    ovl_ops.name = "overlay";
    ovl_ops.create_file  = o_create_file;
    ovl_ops.create_dir   = o_create_dir;
    ovl_ops.delete_file  = o_delete_file;
    ovl_ops.delete_dir   = o_delete_dir;
    ovl_ops.write_file   = o_write_file;
    ovl_ops.read_file    = o_read_file;
    ovl_ops.read_file_range = o_read_file_range;
    ovl_ops.append_file  = o_append_file;
    ovl_ops.get_file_size = o_get_file_size;
    ovl_ops.file_exists  = o_file_exists;
    ovl_ops.dir_exists   = o_dir_exists;
    ovl_ops.dir_is_empty = o_dir_is_empty;
    ovl_ops.list_dir     = o_list_dir;
    ovl_ops.chmod        = o_chmod;
    ovl_ops.chown        = o_chown;
    ovl_ops.get_owner    = o_get_owner;
    ovl_ops.get_mode     = o_get_mode;

    vfs_replace_root_ops(&ovl_ops, ramfs_get_sb());
    ovl_active = 1;
    return 1;
}
