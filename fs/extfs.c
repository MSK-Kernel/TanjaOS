/* ext2 / ext3 / ext4, read-only.
 *
 * One driver for all three: ext2 and ext3 files use the classic direct /
 * indirect block map, ext4 files use extent trees; the journal is neither
 * written nor replayed (a volume that was not cleanly unmounted may show
 * stale data - a warning says so). Supported: 1 KiB..64 KiB blocks, 32- and
 * 64-bit block numbers, flex_bg, htree directories (read linearly), symlinks.
 * Refused: inline_data, encryption, casefold, meta_bg, external journals.
 */
#include <stdint.h>
#include <stddef.h>
#include "../include/vfs.h"
#include "../include/blkio.h"

extern void* malloc(size_t n);
extern void free(void* p);
extern void print(const char* s);

#define EXT_MAGIC 0xEF53u
#define EXT_ROOT_INO 2u

#define INCOMPAT_FILETYPE   0x0002u
#define INCOMPAT_RECOVER    0x0004u
#define INCOMPAT_JOURNALDEV 0x0008u
#define INCOMPAT_META_BG    0x0010u
#define INCOMPAT_EXTENTS    0x0040u
#define INCOMPAT_64BIT      0x0080u
#define INCOMPAT_MMP        0x0100u
#define INCOMPAT_FLEX_BG    0x0200u
#define INCOMPAT_EA_INODE   0x0400u
#define INCOMPAT_CSUM_SEED  0x2000u
#define INCOMPAT_LARGEDIR   0x4000u
#define INCOMPAT_INLINE     0x8000u
#define INCOMPAT_ENCRYPT    0x10000u
#define INCOMPAT_CASEFOLD   0x20000u
#define INCOMPAT_OK (INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT | \
                     INCOMPAT_MMP | INCOMPAT_FLEX_BG | INCOMPAT_EA_INODE | INCOMPAT_CSUM_SEED | \
                     INCOMPAT_LARGEDIR)

#define COMPAT_HAS_JOURNAL 0x0004u

#define FL_EXTENTS 0x00080000u

#define S_IFMT   0170000u
#define S_IFDIR  0040000u
#define S_IFREG  0100000u
#define S_IFLNK  0120000u

#define LIST_LIMIT 16000u
#define MAX_SYMLINK_DEPTH 8

struct cent { uint64_t blk; int valid; };

struct ext {
    struct vfs_bdev* dev;
    uint32_t bs, ipg, bpg, isz, first_data, groups, dsz;
    uint32_t compat, incompat, ro_compat;
    uint64_t blocks;
    int is64;
    struct cent* ce;
    uint8_t* cdata;
    uint32_t ncache;
    uint8_t* dirbuf;           /* private copy of the directory block being walked */
};

struct inode {
    uint32_t mode, uid, flags, ino;
    uint64_t size;
    uint8_t iblock[60];
};

/* ---- block cache: direct mapped, pointers valid until the next getblk() on the same slot ---- */

static uint8_t* getblk(struct ext* e, uint64_t blk)
{
    uint32_t slot = (uint32_t)(blk % e->ncache);
    uint8_t* data = e->cdata + (uint64_t)slot * e->bs;
    if (e->ce[slot].valid && e->ce[slot].blk == blk) return data;
    e->ce[slot].valid = 0;
    if (blk >= e->blocks) return 0;
    if (blk_read(e->dev, blk * e->bs, data, e->bs) != 0) return 0;
    e->ce[slot].blk = blk;
    e->ce[slot].valid = 1;
    return data;
}

/* ---- inodes ---- */

static int read_inode(struct ext* e, uint32_t ino, struct inode* in)
{
    uint32_t grp, idx;
    uint64_t desc_off, itable, boff;
    uint8_t* b;
    uint8_t raw[256];
    uint32_t n;

    if (ino == 0) return -1;
    grp = (ino - 1) / e->ipg;
    idx = (ino - 1) % e->ipg;
    if (grp >= e->groups) return -1;

    desc_off = (uint64_t)(e->first_data + 1) * e->bs + (uint64_t)grp * e->dsz;
    b = getblk(e, desc_off / e->bs);
    if (!b) return -1;
    b += desc_off % e->bs;
    itable = rd32(b + 8);
    if (e->is64 && e->dsz >= 64) itable |= (uint64_t)rd32(b + 0x28) << 32;

    boff = itable * e->bs + (uint64_t)idx * e->isz;
    b = getblk(e, boff / e->bs);
    if (!b) return -1;
    n = e->isz < sizeof raw ? e->isz : (uint32_t)sizeof raw;
    rfs_copy(raw, b + boff % e->bs, n);

    in->ino = ino;
    in->mode = rd16(raw);
    in->uid = rd16(raw + 2) | ((uint32_t)rd16(raw + 120) << 16);
    in->size = rd32(raw + 4);
    if ((in->mode & S_IFMT) == S_IFREG) in->size |= (uint64_t)rd32(raw + 108) << 32;
    in->flags = rd32(raw + 32);
    rfs_copy(in->iblock, raw + 40, 60);
    return 0;
}

/* ---- logical -> physical block ---- */

/* 0 = mapped (*pblk, 0 = hole), 1 = uninitialised extent (reads as zeros), -1 = error */
static int bmap(struct ext* e, const struct inode* in, uint32_t lblk, uint64_t* pblk)
{
    *pblk = 0;
    if (in->flags & FL_EXTENTS) {
        const uint8_t* cur = in->iblock;
        uint32_t depth, entries, i, level;
        if (rd16(cur) != 0xF30A) return -1;
        for (level = 0; level < 8; level++) {
            entries = rd16(cur + 2);
            depth = rd16(cur + 6);
            if (entries > 340) return -1;
            if (depth == 0) {
                for (i = 0; i < entries; i++) {
                    const uint8_t* x = cur + 12 + 12 * i;
                    uint32_t first = rd32(x);
                    uint32_t len = rd16(x + 4);
                    int uninit = 0;
                    if (len > 32768) { len -= 32768; uninit = 1; }
                    if (lblk >= first && lblk < first + len) {
                        *pblk = (((uint64_t)rd16(x + 6)) << 32 | rd32(x + 8)) + (lblk - first);
                        return uninit ? 1 : 0;
                    }
                }
                return 0;                                  /* hole */
            } else {
                uint64_t child = 0;
                int found = 0;
                for (i = 0; i < entries; i++) {
                    const uint8_t* x = cur + 12 + 12 * i;
                    if (rd32(x) <= lblk) {
                        child = rd32(x + 4) | ((uint64_t)rd16(x + 8) << 32);
                        found = 1;
                    } else break;
                }
                if (!found) return 0;
                cur = getblk(e, child);
                if (!cur || rd16(cur) != 0xF30A) return -1;
            }
        }
        return -1;
    } else {
        uint32_t ppb = e->bs / 4, ptr;
        const uint8_t* b;
        if (lblk < 12) { *pblk = rd32(in->iblock + 4 * lblk); return 0; }
        lblk -= 12;
        if (lblk < ppb) {
            ptr = rd32(in->iblock + 48);
            if (!ptr) return 0;
            b = getblk(e, ptr); if (!b) return -1;
            *pblk = rd32(b + 4 * lblk);
            return 0;
        }
        lblk -= ppb;
        if (lblk < ppb * ppb) {
            ptr = rd32(in->iblock + 52);
            if (!ptr) return 0;
            b = getblk(e, ptr); if (!b) return -1;
            ptr = rd32(b + 4 * (lblk / ppb));
            if (!ptr) return 0;
            b = getblk(e, ptr); if (!b) return -1;
            *pblk = rd32(b + 4 * (lblk % ppb));
            return 0;
        }
        lblk -= ppb * ppb;
        ptr = rd32(in->iblock + 56);
        if (!ptr) return 0;
        b = getblk(e, ptr); if (!b) return -1;
        ptr = rd32(b + 4 * (lblk / (ppb * ppb)));
        if (!ptr) return 0;
        b = getblk(e, ptr); if (!b) return -1;
        ptr = rd32(b + 4 * ((lblk / ppb) % ppb));
        if (!ptr) return 0;
        b = getblk(e, ptr); if (!b) return -1;
        *pblk = rd32(b + 4 * (lblk % ppb));
        return 0;
    }
}

/* ---- file data ---- */

/* read up to len bytes at offset; returns bytes read or -1 */
static int64_t read_data(struct ext* e, const struct inode* in, uint64_t off, uint8_t* buf, uint64_t len)
{
    uint64_t done = 0;
    if (off >= in->size) return 0;
    if (len > in->size - off) len = in->size - off;

    while (done < len) {
        uint64_t pos = off + done;
        uint32_t lblk = (uint32_t)(pos / e->bs);
        uint32_t boff = (uint32_t)(pos % e->bs);
        uint32_t take = e->bs - boff;
        uint64_t pblk;
        int rc = bmap(e, in, lblk, &pblk);
        if (rc < 0) return -1;
        if (take > len - done) take = (uint32_t)(len - done);

        if (rc == 1 || pblk == 0) {                        /* hole / uninitialised: zeros */
            rfs_zero(buf + done, take);
        } else if (boff == 0 && take == e->bs && (((uintptr_t)(buf + done)) & 3) == 0) {
            /* whole blocks: pull a contiguous run in one device read */
            uint32_t run = 1;
            uint64_t p2;
            while (done + (uint64_t)(run + 1) * e->bs <= len && run < 64) {
                int r2 = bmap(e, in, lblk + run, &p2);
                if (r2 != 0 || p2 != pblk + run) break;
                run++;
            }
            if (blk_read(e->dev, pblk * e->bs, buf + done, run * e->bs) != 0) return -1;
            take = run * e->bs;
        } else {
            const uint8_t* b = getblk(e, pblk);
            if (!b) return -1;
            rfs_copy(buf + done, b + boff, take);
        }
        done += take;
    }
    return (int64_t)done;
}

/* ---- directories ---- */

/* callback returns non-zero to stop */
typedef int (*dir_cb)(void* ctx, const uint8_t* name, uint32_t nl, uint32_t ino, uint32_t ftype);

static int dir_iter(struct ext* e, const struct inode* dir, dir_cb cb, void* ctx)
{
    uint32_t nblocks = (uint32_t)((dir->size + e->bs - 1) / e->bs), lb;
    for (lb = 0; lb < nblocks; lb++) {
        uint64_t pblk;
        uint32_t off = 0;
        const uint8_t* b;
        if (bmap(e, dir, lb, &pblk) != 0 || pblk == 0) continue;
        b = getblk(e, pblk);
        if (!b) return -1;
        rfs_copy(e->dirbuf, b, e->bs);                     /* callbacks may evict the cache slot */
        while (off + 8 <= e->bs) {
            uint32_t ino = rd32(e->dirbuf + off);
            uint32_t rl = rd16(e->dirbuf + off + 4);
            uint32_t nl = e->dirbuf[off + 6];
            uint32_t ft = e->dirbuf[off + 7];
            if (rl < 8 || (rl & 3) || off + rl > e->bs) break;
            if (!(e->incompat & INCOMPAT_FILETYPE)) { nl |= (ft << 8); ft = 0; }
            if (ino != 0 && nl > 0 && 8 + nl <= rl)
                if (cb(ctx, e->dirbuf + off + 8, nl, ino, ft)) return 1;
            off += rl;
        }
    }
    return 0;
}

struct find_ctx { const char* name; uint32_t nl; uint32_t ino; };

static int find_cb(void* ctx, const uint8_t* name, uint32_t nl, uint32_t ino, uint32_t ft)
{
    struct find_ctx* f = (struct find_ctx*)ctx;
    uint32_t i;
    (void)ft;
    if (nl != f->nl) return 0;
    for (i = 0; i < nl; i++) if ((char)name[i] != f->name[i]) return 0;
    f->ino = ino;
    return 1;
}

/* ---- path resolution (with symlinks) ---- */

static int resolve(struct ext* e, uint32_t start_ino, const char* path, int follow_last,
                   struct inode* out, int depth);

static int read_link(struct ext* e, const struct inode* in, char* buf, uint32_t cap)
{
    int64_t n;
    if (in->size == 0 || in->size >= cap) return -1;
    if (in->size < 60 && !(in->flags & FL_EXTENTS)) {      /* fast symlink: target lives in i_block */
        rfs_copy(buf, in->iblock, (uint32_t)in->size);
        n = (int64_t)in->size;
    } else if (in->size < 60 && (in->flags & FL_EXTENTS) && rd16(in->iblock) != 0xF30A) {
        rfs_copy(buf, in->iblock, (uint32_t)in->size);
        n = (int64_t)in->size;
    } else {
        n = read_data(e, in, 0, (uint8_t*)buf, in->size);
    }
    if (n < 0) return -1;
    buf[n] = 0;
    return 0;
}

static int resolve(struct ext* e, uint32_t start_ino, const char* path, int follow_last,
                   struct inode* out, int depth)
{
    struct inode cur;
    const char* p = path;
    uint32_t dir_ino;

    if (depth > MAX_SYMLINK_DEPTH) return -1;
    if (*p == '/') start_ino = EXT_ROOT_INO;
    dir_ino = start_ino;
    if (read_inode(e, dir_ino, &cur) != 0) return -1;

    for (;;) {
        char comp[256];
        uint32_t n = 0;
        struct find_ctx fc;
        struct inode child;
        int last;

        while (*p == '/') p++;
        if (!*p) break;
        while (*p && *p != '/' && n < 255) comp[n++] = *p++;
        if (*p && *p != '/') return -1;                    /* component too long */
        comp[n] = 0;
        while (*p == '/') p++;
        last = (*p == 0);

        if (n == 1 && comp[0] == '.') continue;
        if ((cur.mode & S_IFMT) != S_IFDIR) return -1;
        fc.name = comp; fc.nl = n; fc.ino = 0;
        if (dir_iter(e, &cur, find_cb, &fc) != 1) return -1;
        if (read_inode(e, fc.ino, &child) != 0) return -1;

        if ((child.mode & S_IFMT) == S_IFLNK && (!last || follow_last)) {
            char target[512], joined[768];
            uint32_t tl, rl, k;
            if (read_link(e, &child, target, sizeof target) != 0) return -1;
            tl = rfs_len(target); rl = rfs_len(p);
            if (tl + 1 + rl + 1 > sizeof joined) return -1;
            for (k = 0; k < tl; k++) joined[k] = target[k];
            joined[tl] = '/';
            for (k = 0; k < rl; k++) joined[tl + 1 + k] = p[k];
            joined[tl + 1 + rl] = 0;
            return resolve(e, dir_ino, joined, follow_last, out, depth + 1);
        }
        dir_ino = child.ino;
        cur = child;
    }
    *out = cur;
    return 0;
}

static int lookup(struct ext* e, const char* path, struct inode* out)
{
    return resolve(e, EXT_ROOT_INO, path, 1, out, 0);
}

/* ---- VFS operations ---- */

static int x_file_exists(void* sb, const char* p) {
    struct inode in;
    if (!sb || lookup((struct ext*)sb, p, &in) != 0) return 0;
    return (in.mode & S_IFMT) != S_IFDIR;
}

static int x_dir_exists(void* sb, const char* p) {
    struct inode in;
    if (!sb || lookup((struct ext*)sb, p, &in) != 0) return 0;
    return (in.mode & S_IFMT) == S_IFDIR;
}

static uint32_t x_get_file_size(void* sb, const char* p) {
    struct inode in;
    if (!sb || lookup((struct ext*)sb, p, &in) != 0) return 0;
    if ((in.mode & S_IFMT) == S_IFDIR) return 0;
    return in.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)in.size;
}

static int x_read_file_range(void* sb, const char* p, uint32_t offset, char* buf,
                             uint32_t cap, uint32_t* size)
{
    struct inode in;
    int64_t n;
    if (!sb || !buf || !size) return -1;
    *size = 0;
    if (lookup((struct ext*)sb, p, &in) != 0 || (in.mode & S_IFMT) == S_IFDIR) return -1;
    n = read_data((struct ext*)sb, &in, offset, (uint8_t*)buf, cap);
    if (n < 0) return -1;
    *size = (uint32_t)n;
    return 0;
}

static int x_read_file(void* sb, const char* p, char* buf, uint32_t* size)
{
    uint32_t cap, got = 0;
    if (!buf || !size) return -1;
    cap = *size;
    if (cap == 0) { buf[0] = 0; return x_file_exists(sb, p) ? 0 : -1; }
    if (x_read_file_range(sb, p, 0, buf, cap, &got) != 0) { buf[0] = 0; *size = 0; return -1; }
    buf[got] = 0;
    *size = got;
    return 0;
}

struct list_ctx { struct ext* e; char* out; uint32_t pos; };

static int list_cb(void* ctx, const uint8_t* name, uint32_t nl, uint32_t ino, uint32_t ft)
{
    struct list_ctx* l = (struct list_ctx*)ctx;
    uint32_t i;
    int is_dir;
    if ((nl == 1 && name[0] == '.') || (nl == 2 && name[0] == '.' && name[1] == '.')) return 0;
    if (ft == 0) {                                          /* unknown type: ask the inode */
        struct inode in;
        is_dir = (read_inode(l->e, ino, &in) == 0 && (in.mode & S_IFMT) == S_IFDIR);
    } else {
        is_dir = (ft == 2);
    }
    if (l->pos + nl + 3 > LIST_LIMIT) return 1;
    if (l->pos) l->out[l->pos++] = '\n';
    for (i = 0; i < nl; i++) l->out[l->pos++] = name[i] == '\n' ? '?' : (char)name[i];
    if (is_dir) l->out[l->pos++] = '/';
    l->out[l->pos] = 0;
    return 0;
}

static int x_list_dir(void* sb, const char* p, char* out, uint32_t* size)
{
    struct ext* e = (struct ext*)sb;
    struct inode in;
    struct list_ctx l;
    if (!e || !out || !size) return -1;
    *size = 0; out[0] = 0;
    if (lookup(e, p, &in) != 0 || (in.mode & S_IFMT) != S_IFDIR) return -1;
    l.e = e; l.out = out; l.pos = 0;
    if (dir_iter(e, &in, list_cb, &l) < 0) return -1;
    *size = l.pos;
    return 0;
}

static int x_dir_is_empty(void* sb, const char* p) {
    static char tmp[LIST_LIMIT + 8];
    uint32_t sz = 0;
    if (!x_dir_exists(sb, p)) return 0;
    if (x_list_dir(sb, p, tmp, &sz) != 0) return 0;
    return sz == 0;
}

static int x_get_mode(void* sb, const char* p) {
    struct inode in;
    if (!sb || lookup((struct ext*)sb, p, &in) != 0) return 0644;
    return (int)(in.mode & 07777);
}

static int x_get_owner(void* sb, const char* p) {
    struct inode in;
    if (!sb || lookup((struct ext*)sb, p, &in) != 0) return 0;
    return (int)in.uid;
}

/* read-only: every modification is refused */
static int x_ro1(void* sb, const char* p) { (void)sb; (void)p; return VFS_EACCES; }
static int x_ro_w(void* sb, const char* p, const char* d, uint32_t n) { (void)sb; (void)p; (void)d; (void)n; return VFS_EACCES; }
static int x_ro_chmod(void* sb, const char* p, int v) { (void)sb; (void)p; (void)v; return VFS_EACCES; }
static int x_ro_at(void* sb, const char* p, uint32_t o, const char* d, uint32_t n) { (void)sb; (void)p; (void)o; (void)d; (void)n; return VFS_EACCES; }
static int x_ro_prealloc(void* sb, const char* p, uint32_t n) { (void)sb; (void)p; (void)n; return VFS_EACCES; }

/* ---- probe / mount ---- */

/* 2, 3 or 4 when the device holds an ext2/3/4 filesystem, else 0 */
int extfs_probe(struct vfs_bdev* dev)
{
    uint8_t sb[256];
    uint32_t compat, incompat, ro;
    if (!dev || blk_read(dev, 1024, sb, sizeof sb) != 0) return 0;
    if (rd16(sb + 56) != EXT_MAGIC) return 0;
    compat = rd32(sb + 92); incompat = rd32(sb + 96); ro = rd32(sb + 100);
    if (incompat & (INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG | INCOMPAT_MMP) )
        return 4;
    if (ro & 0x0040u /* HUGE_FILE */ || ro & 0x0020u /* DIR_NLINK */ || ro & 0x0010u /* GDT_CSUM */ || ro & 0x0100u /* METADATA_CSUM */)
        return 4;
    if (compat & COMPAT_HAS_JOURNAL) return 3;
    return 2;
}

static int x_mount(void** sbp, struct vfs_bdev* dev, const char* opts)
{
    uint8_t s[1024];
    struct ext* e;
    uint32_t log_bs;
    uint64_t blocks;
    (void)opts;

    if (!dev || !dev->present || !dev->read) return VFS_EINVAL;
    if (blk_read(dev, 1024, s, sizeof s) != 0) return VFS_ERR;
    if (rd16(s + 56) != EXT_MAGIC) return VFS_ENOENT;

    e = (struct ext*)malloc(sizeof *e);
    if (!e) return VFS_ENOSPC;
    rfs_zero(e, sizeof *e);
    e->dev = dev;
    log_bs = rd32(s + 24);
    if (log_bs > 6) { free(e); return VFS_EINVAL; }
    e->bs = 1024u << log_bs;
    e->first_data = rd32(s + 20);
    e->bpg = rd32(s + 32);
    e->ipg = rd32(s + 40);
    e->compat = rd32(s + 92);
    e->incompat = rd32(s + 96);
    e->ro_compat = rd32(s + 100);
    e->isz = rd32(s + 76) >= 1 ? rd16(s + 88) : 128;
    if (e->isz < 128 || e->isz > e->bs) { free(e); return VFS_EINVAL; }
    e->is64 = (e->incompat & INCOMPAT_64BIT) != 0;
    e->dsz = e->is64 ? rd16(s + 254) : 32;
    if (e->is64 && e->dsz < 64) e->dsz = 64;
    blocks = rd32(s + 4);
    if (e->is64) blocks |= (uint64_t)rd32(s + 336) << 32;
    e->blocks = blocks;
    if (e->bpg == 0 || e->ipg == 0 || blocks == 0) { free(e); return VFS_EINVAL; }
    e->groups = (uint32_t)((blocks - e->first_data + e->bpg - 1) / e->bpg);

    if (e->incompat & ~INCOMPAT_OK) {
        print("extfs: ");
        print(dev->name);
        if (e->incompat & INCOMPAT_INLINE) print(": inline_data is not supported\n");
        else if (e->incompat & INCOMPAT_ENCRYPT) print(": encrypted filesystems are not supported\n");
        else if (e->incompat & INCOMPAT_META_BG) print(": meta_bg is not supported\n");
        else if (e->incompat & INCOMPAT_JOURNALDEV) print(": external journal device, not a filesystem\n");
        else print(": unsupported feature flags\n");
        free(e);
        return VFS_EINVAL;
    }

    e->ncache = e->bs <= 4096 ? 64 : 16;
    e->ce = (struct cent*)malloc(e->ncache * sizeof(struct cent));
    e->cdata = (uint8_t*)malloc((size_t)e->ncache * e->bs);
    e->dirbuf = (uint8_t*)malloc(e->bs);
    if (!e->ce || !e->cdata || !e->dirbuf) {
        free(e->ce); free(e->cdata); free(e->dirbuf); free(e);
        return VFS_ENOSPC;
    }
    rfs_zero(e->ce, e->ncache * sizeof(struct cent));

    {
        struct inode root;
        if (read_inode(e, EXT_ROOT_INO, &root) != 0 || (root.mode & S_IFMT) != S_IFDIR) {
            free(e->ce); free(e->cdata); free(e->dirbuf); free(e);
            return VFS_ERR;
        }
    }

    print("extfs: ");
    print(dev->name);
    print(": ext");
    print((e->incompat & (INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG)) ? "4"
          : (e->compat & COMPAT_HAS_JOURNAL) ? "3" : "2");
    print(", read-only");
    if (e->incompat & INCOMPAT_RECOVER)
        print(" (WARNING: not cleanly unmounted, journal not replayed - data may be stale)");
    print("\n");

    *sbp = e;
    return VFS_OK;
}

static void x_unmount(void* sb)
{
    struct ext* e = (struct ext*)sb;
    if (!e) return;
    free(e->ce); free(e->cdata); free(e->dirbuf);
    free(e);
}

static struct vfs_ops ext_ops;

const struct vfs_ops* extfs_get_ops(void)
{
    struct vfs_ops* o = &ext_ops;
    rfs_zero(o, sizeof *o);
    o->name = "ext4";
    o->create_file = x_ro1;
    o->create_dir = x_ro1;
    o->delete_file = x_ro1;
    o->delete_dir = x_ro1;
    o->delete_dir_recursive = x_ro1;
    o->write_file = x_ro_w;
    o->append_file = x_ro_w;
    o->read_file = x_read_file;
    o->read_file_range = x_read_file_range;
    o->get_file_size = x_get_file_size;
    o->file_exists = x_file_exists;
    o->dir_exists = x_dir_exists;
    o->dir_is_empty = x_dir_is_empty;
    o->list_dir = x_list_dir;
    o->chmod = x_ro_chmod;
    o->chown = x_ro_chmod;
    o->get_owner = x_get_owner;
    o->get_mode = x_get_mode;
    o->write_at = x_ro_at;
    o->prealloc = x_ro_prealloc;
    o->mount = x_mount;
    o->unmount = x_unmount;
    return o;
}
