/* btrfs, read-only.
 *
 * Single-device volumes (profiles single / DUP / RAID1 on one device), the
 * default subvolume (other subvolumes and snapshots show up as directories
 * you can enter), inline and regular extents, sparse files, symlinks.
 * Metadata and data checksums are not verified. Compressed (zlib / lzo /
 * zstd) and encrypted extents are not supported: reading such a file fails
 * and a message says why. RAID0/5/6/10 volumes are refused.
 */
#include <stdint.h>
#include <stddef.h>
#include "../include/vfs.h"
#include "../include/blkio.h"

extern void* malloc(size_t n);
extern void free(void* p);
extern void print(const char* s);

#define SB_OFFSET   0x10000ull
#define SB_MAGIC    0x4D5F53665248425Full         /* "_BHRfS_M" */

#define KEY_INODE_ITEM   1
#define KEY_DIR_ITEM     84
#define KEY_DIR_INDEX    96
#define KEY_EXTENT_DATA  108
#define KEY_ROOT_ITEM    132
#define KEY_CHUNK_ITEM   228

#define OBJ_ROOT_TREE_DIR 6ull
#define OBJ_FS_TREE       5ull
#define OBJ_FIRST_CHUNK   256ull

#define CHUNK_RAID0   0x08ull
#define CHUNK_RAID1   0x10ull
#define CHUNK_DUP     0x20ull
#define CHUNK_RAID10  0x40ull
#define CHUNK_RAID5   0x80ull
#define CHUNK_RAID6   0x100ull

#define HDR_SIZE   101u
#define ITEM_SIZE  25u
#define PTR_SIZE   33u
#define MAXLVL     8

#define S_IFMT   0170000u
#define S_IFDIR  0040000u
#define S_IFLNK  0120000u

#define LIST_LIMIT 16000u
#define MAX_SYMLINK_DEPTH 8

struct key { uint64_t oid; uint8_t type; uint64_t off; };

struct chunk { uint64_t logical, length, phys, type; };

struct btrfs {
    struct vfs_bdev* dev;
    uint32_t nodesize, sectorsize;
    uint64_t devid;
    uint64_t root_tree, chunk_tree;
    struct chunk* chunks;
    uint32_t nchunks, cchunks;
    uint8_t* cdata;
    uint64_t* clog;                 /* logical address held by each cache slot */
    uint32_t ncache;
    uint64_t def_tree_root;         /* default subvolume */
    uint64_t def_dirid;
    int warned_compress;
};

struct path { uint64_t node[MAXLVL]; int slot[MAXLVL]; int lvl; };

struct bnode { uint64_t tree_root, ino; };

struct inode { uint32_t mode, uid; uint64_t size; };

/* ---- logical -> physical ---- */

static int map_logical(struct btrfs* b, uint64_t logical, uint64_t* phys)
{
    uint32_t i;
    for (i = 0; i < b->nchunks; i++) {
        struct chunk* c = &b->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length) {
            *phys = c->phys + (logical - c->logical);
            return 0;
        }
    }
    return -1;
}

static int add_chunk(struct btrfs* b, uint64_t logical, uint64_t length, uint64_t phys, uint64_t type)
{
    uint32_t i;
    for (i = 0; i < b->nchunks; i++) if (b->chunks[i].logical == logical) return 0;
    if (b->nchunks == b->cchunks) {
        uint32_t ncap = b->cchunks ? b->cchunks * 2 : 64;
        struct chunk* n = (struct chunk*)malloc(ncap * sizeof *n);
        if (!n) return -1;
        if (b->chunks) { rfs_copy(n, b->chunks, b->nchunks * sizeof *n); free(b->chunks); }
        b->chunks = n;
        b->cchunks = ncap;
    }
    b->chunks[b->nchunks].logical = logical;
    b->chunks[b->nchunks].length = length;
    b->chunks[b->nchunks].phys = phys;
    b->chunks[b->nchunks].type = type;
    b->nchunks++;
    return 0;
}

/* parse one btrfs_chunk (at p, key offset = logical start); returns its size or 0 */
static uint32_t parse_chunk(struct btrfs* b, uint64_t logical, const uint8_t* p, uint32_t avail)
{
    uint64_t length = rd64(p), type = rd64(p + 24);
    uint32_t nstripes = rd16(p + 44), i, need = 48 + nstripes * 32;
    uint64_t phys = 0;
    int found = 0;
    if (nstripes == 0 || need > avail) return 0;
    if (type & (CHUNK_RAID0 | CHUNK_RAID10 | CHUNK_RAID5 | CHUNK_RAID6)) return need | 0x80000000u;
    for (i = 0; i < nstripes && !found; i++) {
        const uint8_t* s = p + 48 + i * 32;
        if (rd64(s) == b->devid) { phys = rd64(s + 8); found = 1; }
    }
    if (!found) return need;                      /* lives on another device: not reachable from here */
    if (add_chunk(b, logical, length, phys, type) != 0) return 0;
    return need;
}

/* ---- tree nodes ---- */

static const uint8_t* getnode(struct btrfs* b, uint64_t logical)
{
    uint32_t slot = (uint32_t)((logical / b->nodesize) % b->ncache);
    uint8_t* data = b->cdata + (uint64_t)slot * b->nodesize;
    uint64_t phys;
    if (b->clog[slot] == logical) return data;
    b->clog[slot] = 0;
    if (map_logical(b, logical, &phys) != 0) return 0;
    if (blk_read(b->dev, phys, data, b->nodesize) != 0) return 0;
    if (rd64(data + 48) != logical) return 0;                 /* header's own address must match */
    b->clog[slot] = logical;
    return data;
}

static void get_key(const uint8_t* p, struct key* k) { k->oid = rd64(p); k->type = p[8]; k->off = rd64(p + 9); }

static int key_cmp(const struct key* a, const struct key* b)
{
    if (a->oid != b->oid) return a->oid < b->oid ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return 0;
}

/* Position the path at the first item with key >= target. 0 = ok (path valid, possibly
 * one past the last item of its leaf: use path_fix), -1 = error. */
static int search_ge(struct btrfs* b, uint64_t root, const struct key* target, struct path* p)
{
    uint64_t cur = root;
    int lvl = 0;
    for (;;) {
        const uint8_t* n = getnode(b, cur);
        uint32_t nr, i;
        uint8_t level;
        if (!n || lvl >= MAXLVL) return -1;
        nr = rd32(n + 96);
        level = n[100];
        p->node[lvl] = cur;
        if (level == 0) {
            for (i = 0; i < nr; i++) {
                struct key k;
                get_key(n + HDR_SIZE + i * ITEM_SIZE, &k);
                if (key_cmp(&k, target) >= 0) break;
            }
            p->slot[lvl] = (int)i;
            p->lvl = lvl;
            return 0;
        }
        {
            uint32_t chosen = 0;
            for (i = 0; i < nr; i++) {
                struct key k;
                get_key(n + HDR_SIZE + i * PTR_SIZE, &k);
                if (key_cmp(&k, target) <= 0) chosen = i; else break;
            }
            p->slot[lvl] = (int)chosen;
            cur = rd64(n + HDR_SIZE + chosen * PTR_SIZE + 17);
        }
        lvl++;
    }
}

/* descend leftmost / rightmost from the node at level `from` (already in p) to a leaf */
static int descend(struct btrfs* b, struct path* p, int from, int rightmost)
{
    int lvl = from;
    for (;;) {
        const uint8_t* n = getnode(b, p->node[lvl]);
        uint32_t nr;
        if (!n) return -1;
        nr = rd32(n + 96);
        if (n[100] == 0) {
            p->lvl = lvl;
            p->slot[lvl] = rightmost ? (nr ? (int)nr - 1 : 0) : 0;
            return 0;
        }
        if (nr == 0 || lvl + 1 >= MAXLVL) return -1;
        p->slot[lvl] = rightmost ? (int)nr - 1 : 0;
        p->node[lvl + 1] = rd64(n + HDR_SIZE + (uint32_t)p->slot[lvl] * PTR_SIZE + 17);
        lvl++;
    }
}

/* the item the path points at; if the slot is one past the end of the leaf, move on to the
 * next leaf. Returns 0 with *k and the item data, 1 = no more items, -1 = error. The data
 * pointer is only valid until the next getnode(). */
static int path_item(struct btrfs* b, struct path* p, struct key* k, const uint8_t** data, uint32_t* size)
{
    for (;;) {
        const uint8_t* n = getnode(b, p->node[p->lvl]);
        uint32_t nr;
        if (!n) return -1;
        nr = rd32(n + 96);
        if ((uint32_t)p->slot[p->lvl] < nr) {
            const uint8_t* it = n + HDR_SIZE + (uint32_t)p->slot[p->lvl] * ITEM_SIZE;
            get_key(it, k);
            *data = n + HDR_SIZE + rd32(it + 17);
            *size = rd32(it + 21);
            return 0;
        }
        /* past the end: go up to an ancestor that has a next slot */
        {
            int l = p->lvl - 1;
            while (l >= 0) {
                const uint8_t* a = getnode(b, p->node[l]);
                if (!a) return -1;
                if ((uint32_t)p->slot[l] + 1 < rd32(a + 96)) break;
                l--;
            }
            if (l < 0) return 1;
            p->slot[l]++;
            {
                const uint8_t* a = getnode(b, p->node[l]);
                if (!a) return -1;
                p->node[l + 1] = rd64(a + HDR_SIZE + (uint32_t)p->slot[l] * PTR_SIZE + 17);
            }
            if (descend(b, p, l + 1, 0) != 0) return -1;
        }
    }
}

static void path_advance(struct path* p) { p->slot[p->lvl]++; }

/* move to the previous item (0 ok, 1 = none, -1 error) */
static int path_prev(struct btrfs* b, struct path* p)
{
    if (p->slot[p->lvl] > 0) { p->slot[p->lvl]--; return 0; }
    {
        int l = p->lvl - 1;
        while (l >= 0 && p->slot[l] == 0) l--;
        if (l < 0) return 1;
        p->slot[l]--;
        {
            const uint8_t* a = getnode(b, p->node[l]);
            if (!a) return -1;
            p->node[l + 1] = rd64(a + HDR_SIZE + (uint32_t)p->slot[l] * PTR_SIZE + 17);
        }
        return descend(b, p, l + 1, 1);
    }
}

/* ---- crc32c (the name hash of directory items) ---- */

static uint32_t crc32c_raw(uint32_t crc, const uint8_t* p, uint32_t n)
{
    uint32_t i, j;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (j = 0; j < 8; j++) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc;
}

/* ---- inode / directory helpers ---- */

static int read_inode(struct btrfs* b, const struct bnode* n, struct inode* in)
{
    struct key t, k;
    struct path p;
    const uint8_t* d;
    uint32_t sz;
    t.oid = n->ino; t.type = KEY_INODE_ITEM; t.off = 0;
    if (search_ge(b, n->tree_root, &t, &p) != 0) return -1;
    if (path_item(b, &p, &k, &d, &sz) != 0) return -1;
    if (k.oid != n->ino || k.type != KEY_INODE_ITEM || sz < 56) return -1;
    in->size = rd64(d + 16);
    in->uid = rd32(d + 44);
    in->mode = rd32(d + 52);
    return 0;
}

/* parent directory of a directory (via its INODE_REF item); a subvolume's root is its own parent */
static int parent_of(struct btrfs* b, const struct bnode* n, struct bnode* parent)
{
    struct key t, k;
    struct path p;
    const uint8_t* d;
    uint32_t sz;
    *parent = *n;
    t.oid = n->ino; t.type = 12 /* INODE_REF */; t.off = 0;
    if (search_ge(b, n->tree_root, &t, &p) != 0) return -1;
    if (path_item(b, &p, &k, &d, &sz) != 0) return 0;
    if (k.oid == n->ino && k.type == 12 && k.off != n->ino) parent->ino = k.off;
    return 0;
}

/* tree root of subvolume `id` (from the root tree), and its root directory inode */
static int subvol_root(struct btrfs* b, uint64_t id, uint64_t* root, uint64_t* dirid)
{
    struct key t, k;
    struct path p;
    const uint8_t* d;
    uint32_t sz;
    int found = 0, rc;
    t.oid = id; t.type = KEY_ROOT_ITEM; t.off = 0;
    if (search_ge(b, b->root_tree, &t, &p) != 0) return -1;
    for (;;) {
        rc = path_item(b, &p, &k, &d, &sz);
        if (rc < 0) return -1;
        if (rc > 0 || k.oid != id || k.type != KEY_ROOT_ITEM) break;
        if (sz >= 184) { *root = rd64(d + 176); *dirid = rd64(d + 168); found = 1; }   /* the latest one wins */
        path_advance(&p);
    }
    return found ? 0 : -1;
}

struct dirent_info { uint64_t oid; uint8_t ktype; uint8_t ftype; uint32_t nl; char name[256]; };

/* parse the dir_item(s) in an item; calls f for each */
typedef int (*dent_cb)(void* ctx, const struct dirent_info* e);

static int parse_dir_item(const uint8_t* d, uint32_t size, dent_cb cb, void* ctx)
{
    uint32_t off = 0;
    static struct dirent_info e;
    while (off + 30 <= size) {
        uint32_t dl = rd16(d + off + 25), nl = rd16(d + off + 27), i;
        if (off + 30 + nl + dl > size || nl > 255) break;
        e.oid = rd64(d + off);
        e.ktype = d[off + 8];
        e.ftype = d[off + 29];
        e.nl = nl;
        for (i = 0; i < nl; i++) e.name[i] = (char)d[off + 30 + i];
        e.name[nl] = 0;
        if (cb(ctx, &e)) return 1;
        off += 30 + nl + dl;
    }
    return 0;
}

struct find_ctx { const char* name; uint32_t nl; struct dirent_info* out; int found; };

static int find_cb(void* ctx, const struct dirent_info* e)
{
    struct find_ctx* f = (struct find_ctx*)ctx;
    uint32_t i;
    if (e->nl != f->nl) return 0;
    for (i = 0; i < e->nl; i++) if (e->name[i] != f->name[i]) return 0;
    *f->out = *e;
    f->found = 1;
    return 1;
}

/* look a name up in a directory via its DIR_ITEM hash key */
static int dir_find(struct btrfs* b, const struct bnode* dir, const char* name, uint32_t nl,
                    struct dirent_info* out)
{
    struct key t, k;
    struct path p;
    const uint8_t* d;
    uint32_t sz;
    struct find_ctx f;
    int rc;
    t.oid = dir->ino; t.type = KEY_DIR_ITEM;
    t.off = crc32c_raw(0xFFFFFFFEu, (const uint8_t*)name, nl);
    if (search_ge(b, dir->tree_root, &t, &p) != 0) return -1;
    rc = path_item(b, &p, &k, &d, &sz);
    if (rc != 0 || key_cmp(&k, &t) != 0) return -1;
    f.name = name; f.nl = nl; f.out = out; f.found = 0;
    parse_dir_item(d, sz, find_cb, &f);
    return f.found ? 0 : -1;
}

/* iterate a directory in index order; cb non-zero stops */
static int dir_iter(struct btrfs* b, const struct bnode* dir, dent_cb cb, void* ctx)
{
    struct key t, k;
    struct path p;
    const uint8_t* d;
    uint32_t sz;
    int rc;
    t.oid = dir->ino; t.type = KEY_DIR_INDEX; t.off = 0;
    if (search_ge(b, dir->tree_root, &t, &p) != 0) return -1;
    for (;;) {
        rc = path_item(b, &p, &k, &d, &sz);
        if (rc < 0) return -1;
        if (rc > 0 || k.oid != dir->ino || k.type != KEY_DIR_INDEX) return 0;
        if (parse_dir_item(d, sz, cb, ctx)) return 1;
        path_advance(&p);
    }
}

/* ---- file data ---- */

static int64_t read_data(struct btrfs* b, const struct bnode* n, uint64_t fsize, uint64_t off,
                         uint8_t* buf, uint64_t len)
{
    struct key t, k;
    struct path p;
    const uint8_t* d;
    uint32_t sz;
    uint64_t pos, end;
    int rc;

    if (off >= fsize) return 0;
    if (len > fsize - off) len = fsize - off;
    pos = off;
    end = off + len;

    t.oid = n->ino; t.type = KEY_EXTENT_DATA; t.off = pos;
    if (search_ge(b, n->tree_root, &t, &p) != 0) return -1;
    /* the extent covering pos starts at or before it: step back one item */
    {
        int pr = path_prev(b, &p);
        if (pr < 0) return -1;
        if (pr > 0) { /* no earlier item: start from the first */
            if (search_ge(b, n->tree_root, &t, &p) != 0) return -1;
        }
    }

    while (pos < end) {
        uint64_t fo, elen, take;
        uint8_t comp, enc, etype;
        rc = path_item(b, &p, &k, &d, &sz);
        if (rc < 0) return -1;
        if (rc > 0 || k.oid != n->ino || k.type != KEY_EXTENT_DATA) {
            if (rc == 0 && (k.oid < n->ino || (k.oid == n->ino && k.type < KEY_EXTENT_DATA))) { path_advance(&p); continue; }
            rfs_zero(buf + (pos - off), (uint32_t)(end - pos));          /* sparse tail */
            return (int64_t)len;
        }
        fo = k.off;
        if (sz < 21) return -1;
        comp = d[16]; enc = d[17]; etype = d[20];
        if (comp != 0 || enc != 0) {
            if (!b->warned_compress) {
                b->warned_compress = 1;
                print("btrfs: this file uses ");
                print(enc ? "encryption" : comp == 1 ? "zlib compression" : comp == 2 ? "lzo compression" : "zstd compression");
                print(", which is not supported (read-only driver)\n");
            }
            return -1;
        }
        if (etype == 0) {                                                 /* inline */
            elen = sz - 21;
            if (elen > rd64(d + 8)) elen = rd64(d + 8);
        } else {
            if (sz < 53) return -1;
            elen = rd64(d + 45);
        }
        if (fo + elen <= pos) { path_advance(&p); continue; }             /* ends before pos */
        if (fo > pos) {                                                   /* hole before this extent */
            uint64_t z = fo - pos;
            if (z > end - pos) z = end - pos;
            rfs_zero(buf + (pos - off), (uint32_t)z);
            pos += z;
            if (pos >= end) break;
        }
        take = fo + elen - pos;
        if (take > end - pos) take = end - pos;
        if (etype == 0) {
            rfs_copy(buf + (pos - off), d + 21 + (pos - fo), (uint32_t)take);
        } else {
            uint64_t disk = rd64(d + 21), eoff = rd64(d + 37), phys;
            if (etype == 2 || disk == 0) {                                /* prealloc / hole */
                rfs_zero(buf + (pos - off), (uint32_t)take);
            } else {
                if (map_logical(b, disk + eoff + (pos - fo), &phys) != 0) return -1;
                if (blk_read(b->dev, phys, buf + (pos - off), (uint32_t)take) != 0) return -1;
            }
        }
        pos += take;
        path_advance(&p);
    }
    return (int64_t)len;
}

/* ---- path resolution ---- */

static int resolve(struct btrfs* b, const struct bnode* start, const char* path, int follow_last,
                   struct bnode* out, struct inode* oin, int depth);

static int read_symlink(struct btrfs* b, const struct bnode* n, const struct inode* in, char* buf, uint32_t cap)
{
    int64_t got;
    if (in->size == 0 || in->size >= cap) return -1;
    got = read_data(b, n, in->size, 0, (uint8_t*)buf, in->size);
    if (got < 0) return -1;
    buf[got] = 0;
    return 0;
}

static int resolve(struct btrfs* b, const struct bnode* start, const char* path, int follow_last,
                   struct bnode* out, struct inode* oin, int depth)
{
    struct bnode cur, dir;
    struct inode in;
    const char* p = path;

    if (depth > MAX_SYMLINK_DEPTH) return -1;
    cur = *start;
    if (*p == '/') { cur.tree_root = b->def_tree_root; cur.ino = b->def_dirid; }
    dir = cur;
    if (read_inode(b, &cur, &in) != 0) return -1;

    for (;;) {
        char comp[256];
        uint32_t n = 0;
        struct dirent_info de;
        struct bnode child;
        struct inode cin;
        int last;

        while (*p == '/') p++;
        if (!*p) break;
        while (*p && *p != '/' && n < 255) comp[n++] = *p++;
        if (*p && *p != '/') return -1;
        comp[n] = 0;
        while (*p == '/') p++;
        last = (*p == 0);
        if (n == 1 && comp[0] == '.') continue;
        if ((in.mode & S_IFMT) != S_IFDIR) return -1;
        if (n == 2 && comp[0] == '.' && comp[1] == '.') {          /* btrfs directories have no ".." entry */
            struct bnode up;
            if (parent_of(b, &cur, &up) != 0 || read_inode(b, &up, &in) != 0) return -1;
            cur = up;
            continue;
        }
        if (dir_find(b, &cur, comp, n, &de) != 0) return -1;

        child.tree_root = cur.tree_root;
        child.ino = de.oid;
        if (de.ktype == KEY_ROOT_ITEM) {                       /* a subvolume: enter its tree */
            uint64_t root, dirid;
            if (subvol_root(b, de.oid, &root, &dirid) != 0) return -1;
            child.tree_root = root;
            child.ino = dirid;
        }
        if (read_inode(b, &child, &cin) != 0) return -1;

        if ((cin.mode & S_IFMT) == S_IFLNK && (!last || follow_last)) {
            char target[512], joined[768];
            uint32_t tl, rl, k;
            if (read_symlink(b, &child, &cin, target, sizeof target) != 0) return -1;
            tl = rfs_len(target); rl = rfs_len(p);
            if (tl + 1 + rl + 1 > sizeof joined) return -1;
            for (k = 0; k < tl; k++) joined[k] = target[k];
            joined[tl] = '/';
            for (k = 0; k < rl; k++) joined[tl + 1 + k] = p[k];
            joined[tl + 1 + rl] = 0;
            return resolve(b, &cur, joined, follow_last, out, oin, depth + 1);
        }
        dir = cur;
        cur = child;
        in = cin;
    }
    (void)dir;
    *out = cur;
    if (oin) *oin = in;
    return 0;
}

static int lookup(struct btrfs* b, const char* path, struct bnode* n, struct inode* in)
{
    struct bnode root;
    root.tree_root = b->def_tree_root;
    root.ino = b->def_dirid;
    return resolve(b, &root, path, 1, n, in, 0);
}

/* ---- VFS operations ---- */

static int b_file_exists(void* sb, const char* p) {
    struct bnode n; struct inode in;
    if (!sb || lookup((struct btrfs*)sb, p, &n, &in) != 0) return 0;
    return (in.mode & S_IFMT) != S_IFDIR;
}

static int b_dir_exists(void* sb, const char* p) {
    struct bnode n; struct inode in;
    if (!sb || lookup((struct btrfs*)sb, p, &n, &in) != 0) return 0;
    return (in.mode & S_IFMT) == S_IFDIR;
}

static uint32_t b_get_file_size(void* sb, const char* p) {
    struct bnode n; struct inode in;
    if (!sb || lookup((struct btrfs*)sb, p, &n, &in) != 0 || (in.mode & S_IFMT) == S_IFDIR) return 0;
    return in.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)in.size;
}

static int b_read_file_range(void* sb, const char* p, uint32_t offset, char* buf, uint32_t cap, uint32_t* size)
{
    struct bnode n; struct inode in;
    int64_t got;
    if (!sb || !buf || !size) return -1;
    *size = 0;
    if (lookup((struct btrfs*)sb, p, &n, &in) != 0 || (in.mode & S_IFMT) == S_IFDIR) return -1;
    got = read_data((struct btrfs*)sb, &n, in.size, offset, (uint8_t*)buf, cap);
    if (got < 0) return -1;
    *size = (uint32_t)got;
    return 0;
}

static int b_read_file(void* sb, const char* p, char* buf, uint32_t* size)
{
    uint32_t cap, got = 0;
    if (!buf || !size) return -1;
    cap = *size;
    if (cap == 0) { buf[0] = 0; return b_file_exists(sb, p) ? 0 : -1; }
    if (b_read_file_range(sb, p, 0, buf, cap, &got) != 0) { buf[0] = 0; *size = 0; return -1; }
    buf[got] = 0;
    *size = got;
    return 0;
}

struct list_ctx { char* out; uint32_t pos; };

static int list_cb(void* ctx, const struct dirent_info* e)
{
    struct list_ctx* l = (struct list_ctx*)ctx;
    uint32_t i;
    int is_dir = (e->ftype == 2) || e->ktype == KEY_ROOT_ITEM;
    if (l->pos + e->nl + 3 > LIST_LIMIT) return 1;
    if (l->pos) l->out[l->pos++] = '\n';
    for (i = 0; i < e->nl; i++) l->out[l->pos++] = e->name[i] == '\n' ? '?' : e->name[i];
    if (is_dir) l->out[l->pos++] = '/';
    l->out[l->pos] = 0;
    return 0;
}

static int b_list_dir(void* sb, const char* p, char* out, uint32_t* size)
{
    struct btrfs* b = (struct btrfs*)sb;
    struct bnode n; struct inode in;
    struct list_ctx l;
    if (!b || !out || !size) return -1;
    *size = 0; out[0] = 0;
    if (lookup(b, p, &n, &in) != 0 || (in.mode & S_IFMT) != S_IFDIR) return -1;
    l.out = out; l.pos = 0;
    if (dir_iter(b, &n, list_cb, &l) < 0) return -1;
    *size = l.pos;
    return 0;
}

static int b_dir_is_empty(void* sb, const char* p) {
    static char tmp[LIST_LIMIT + 8];
    uint32_t sz = 0;
    if (!b_dir_exists(sb, p)) return 0;
    if (b_list_dir(sb, p, tmp, &sz) != 0) return 0;
    return sz == 0;
}

static int b_get_mode(void* sb, const char* p) {
    struct bnode n; struct inode in;
    if (!sb || lookup((struct btrfs*)sb, p, &n, &in) != 0) return 0644;
    return (int)(in.mode & 07777);
}

static int b_get_owner(void* sb, const char* p) {
    struct bnode n; struct inode in;
    if (!sb || lookup((struct btrfs*)sb, p, &n, &in) != 0) return 0;
    return (int)in.uid;
}

static int b_ro1(void* sb, const char* p) { (void)sb; (void)p; return VFS_EACCES; }
static int b_ro_w(void* sb, const char* p, const char* d, uint32_t n) { (void)sb; (void)p; (void)d; (void)n; return VFS_EACCES; }
static int b_ro_chmod(void* sb, const char* p, int v) { (void)sb; (void)p; (void)v; return VFS_EACCES; }
static int b_ro_at(void* sb, const char* p, uint32_t o, const char* d, uint32_t n) { (void)sb; (void)p; (void)o; (void)d; (void)n; return VFS_EACCES; }
static int b_ro_prealloc(void* sb, const char* p, uint32_t n) { (void)sb; (void)p; (void)n; return VFS_EACCES; }

/* ---- probe / mount ---- */

int btrfs_probe(struct vfs_bdev* dev)
{
    uint8_t m[8];
    if (!dev || blk_read(dev, SB_OFFSET + 0x40, m, 8) != 0) return 0;
    return rd64(m) == SB_MAGIC;
}

static void drop(struct btrfs* b)
{
    if (!b) return;
    free(b->chunks); free(b->cdata); free(b->clog); free(b);
}

static int b_mount(void** sbp, struct vfs_bdev* dev, const char* opts)
{
    static uint8_t sb[4096];
    struct btrfs* b;
    uint32_t arr_size, off;
    (void)opts;

    if (!dev || !dev->present || !dev->read) return VFS_EINVAL;
    if (blk_read(dev, SB_OFFSET, sb, sizeof sb) != 0) return VFS_ERR;
    if (rd64(sb + 0x40) != SB_MAGIC) return VFS_ENOENT;

    b = (struct btrfs*)malloc(sizeof *b);
    if (!b) return VFS_ENOSPC;
    rfs_zero(b, sizeof *b);
    b->dev = dev;
    b->root_tree = rd64(sb + 0x50);
    b->chunk_tree = rd64(sb + 0x58);
    b->sectorsize = rd32(sb + 0x90);
    b->nodesize = rd32(sb + 0x94);
    b->devid = rd64(sb + 0xC9);
    if (b->nodesize < 4096 || b->nodesize > 65536 || (b->nodesize & (b->nodesize - 1))) { drop(b); return VFS_EINVAL; }

    b->ncache = 32;
    b->cdata = (uint8_t*)malloc((size_t)b->ncache * b->nodesize);
    b->clog = (uint64_t*)malloc(b->ncache * sizeof(uint64_t));
    if (!b->cdata || !b->clog) { drop(b); return VFS_ENOSPC; }
    rfs_zero(b->clog, b->ncache * sizeof(uint64_t));

    /* bootstrap: the system chunks embedded in the superblock */
    arr_size = rd32(sb + 0xA0);
    if (arr_size > 2048) { drop(b); return VFS_EINVAL; }
    off = 0;
    while (off + 17 + 48 <= arr_size) {
        const uint8_t* e = sb + 0x32B + off;
        uint32_t cs = parse_chunk(b, rd64(e + 9), e + 17, arr_size - off - 17);
        if (cs == 0) break;
        if (cs & 0x80000000u) { print("btrfs: RAID0/5/6/10 volumes are not supported\n"); drop(b); return VFS_EINVAL; }
        off += 17 + cs;
    }
    if (b->nchunks == 0) { drop(b); return VFS_ERR; }

    /* the full chunk tree */
    {
        struct key t, k;
        struct path p;
        const uint8_t* d;
        uint32_t sz;
        int rc;
        t.oid = OBJ_FIRST_CHUNK; t.type = KEY_CHUNK_ITEM; t.off = 0;
        if (search_ge(b, b->chunk_tree, &t, &p) != 0) { drop(b); return VFS_ERR; }
        for (;;) {
            rc = path_item(b, &p, &k, &d, &sz);
            if (rc < 0) { drop(b); return VFS_ERR; }
            if (rc > 0 || k.type != KEY_CHUNK_ITEM) break;
            {
                uint32_t cs = parse_chunk(b, k.off, d, sz);
                if (cs & 0x80000000u) { print("btrfs: RAID0/5/6/10 volumes are not supported\n"); drop(b); return VFS_EINVAL; }
            }
            path_advance(&p);
        }
    }

    /* default subvolume (usually FS_TREE = 5) */
    {
        struct key t, k;
        struct path p;
        const uint8_t* d;
        uint32_t sz;
        uint64_t subvol = OBJ_FS_TREE;
        int rc;
        t.oid = OBJ_ROOT_TREE_DIR; t.type = KEY_DIR_ITEM; t.off = 0;
        if (search_ge(b, b->root_tree, &t, &p) == 0) {
            for (;;) {
                rc = path_item(b, &p, &k, &d, &sz);
                if (rc != 0 || k.oid != OBJ_ROOT_TREE_DIR || k.type != KEY_DIR_ITEM) break;
                if (sz >= 38 && rd16(d + 27) == 7 && d[30] == 'd' && d[31] == 'e' && d[32] == 'f' &&
                    d[33] == 'a' && d[34] == 'u' && d[35] == 'l' && d[36] == 't') {
                    subvol = rd64(d);
                    break;
                }
                path_advance(&p);
            }
        }
        if (subvol_root(b, subvol, &b->def_tree_root, &b->def_dirid) != 0) {
            print("btrfs: cannot find the default subvolume\n");
            drop(b);
            return VFS_ERR;
        }
    }
    {
        struct bnode r; struct inode in;
        r.tree_root = b->def_tree_root; r.ino = b->def_dirid;
        if (read_inode(b, &r, &in) != 0 || (in.mode & S_IFMT) != S_IFDIR) { drop(b); return VFS_ERR; }
    }

    print("btrfs: ");
    print(dev->name);
    print(": btrfs, read-only (no compression support)\n");
    *sbp = b;
    return VFS_OK;
}

static void b_unmount(void* sb) { drop((struct btrfs*)sb); }

static struct vfs_ops btrfs_ops;

const struct vfs_ops* btrfs_get_ops(void)
{
    struct vfs_ops* o = &btrfs_ops;
    rfs_zero(o, sizeof *o);
    o->name = "btrfs";
    o->create_file = b_ro1;
    o->create_dir = b_ro1;
    o->delete_file = b_ro1;
    o->delete_dir = b_ro1;
    o->delete_dir_recursive = b_ro1;
    o->write_file = b_ro_w;
    o->append_file = b_ro_w;
    o->read_file = b_read_file;
    o->read_file_range = b_read_file_range;
    o->get_file_size = b_get_file_size;
    o->file_exists = b_file_exists;
    o->dir_exists = b_dir_exists;
    o->dir_is_empty = b_dir_is_empty;
    o->list_dir = b_list_dir;
    o->chmod = b_ro_chmod;
    o->chown = b_ro_chmod;
    o->get_owner = b_get_owner;
    o->get_mode = b_get_mode;
    o->write_at = b_ro_at;
    o->prealloc = b_ro_prealloc;
    o->mount = b_mount;
    o->unmount = b_unmount;
    return o;
}
