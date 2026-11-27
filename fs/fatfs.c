/* FAT12 / FAT16 / FAT32 with VFAT long file names, read-only.
 *
 * Names are matched case-insensitively (as FAT does) and shown the way they
 * are stored: the long name when there is one, otherwise the 8.3 name with the
 * lower-case flags Windows NT / Linux set. Long names are converted from
 * UCS-2 to UTF-8.
 */
#include <stdint.h>
#include <stddef.h>
#include "../include/vfs.h"
#include "../include/blkio.h"

extern void* malloc(size_t n);
extern void free(void* p);
extern void print(const char* s);

#define LIST_LIMIT 16000u
#define ATTR_RO   0x01
#define ATTR_HID  0x02
#define ATTR_SYS  0x04
#define ATTR_VOL  0x08
#define ATTR_DIR  0x10
#define ATTR_LFN  0x0F

struct fat {
    struct vfs_bdev* dev;
    uint32_t bps, spc, reserved, nfats, fatsz, root_ents, total;
    uint32_t root_start;            /* first sector of the fixed root directory (FAT12/16) */
    uint32_t root_sectors;
    uint32_t data_start;            /* first sector of cluster 2 */
    uint32_t clusters;
    uint32_t root_cluster;          /* FAT32 */
    uint32_t cbytes;                /* bytes per cluster */
    int type;                       /* 12, 16, 32 */
    uint8_t* fatbuf;                /* one cached FAT sector */
    int64_t fat_cached;             /* its sector number within the FAT, -1 = none */
    uint8_t* dirbuf;                /* one cluster of a directory being walked */
    /* where the last file read ended, to continue a sequential read without
       walking the whole cluster chain again */
    uint32_t cc_first, cc_idx, cc_clu;
};

struct fent {
    char name[800];
    uint8_t attr;
    uint32_t cluster;
    uint32_t size;
};

/* ---- FAT ---- */

static int fat_byte(struct fat* f, uint32_t off, uint8_t* out)
{
    uint32_t sec = off / f->bps;
    if (f->fat_cached != (int64_t)sec) {
        if (blk_read(f->dev, ((uint64_t)f->reserved + sec) * f->bps, f->fatbuf, f->bps) != 0) {
            f->fat_cached = -1;
            return -1;
        }
        f->fat_cached = (int64_t)sec;
    }
    *out = f->fatbuf[off % f->bps];
    return 0;
}

/* next cluster in the chain; 0xFFFFFFFF = end of chain / bad / error */
static uint32_t fat_next(struct fat* f, uint32_t c)
{
    uint8_t a, b, c2, d;
    if (c < 2 || c >= f->clusters + 2) return 0xFFFFFFFFu;
    if (f->type == 12) {
        uint32_t off = c + c / 2, v;
        if (fat_byte(f, off, &a) || fat_byte(f, off + 1, &b)) return 0xFFFFFFFFu;
        v = a | ((uint32_t)b << 8);
        v = (c & 1) ? (v >> 4) : (v & 0xFFF);
        return v >= 0xFF7 ? 0xFFFFFFFFu : v;
    }
    if (f->type == 16) {
        uint32_t v;
        if (fat_byte(f, c * 2, &a) || fat_byte(f, c * 2 + 1, &b)) return 0xFFFFFFFFu;
        v = a | ((uint32_t)b << 8);
        return v >= 0xFFF7 ? 0xFFFFFFFFu : v;
    } else {
        uint32_t v;
        if (fat_byte(f, c * 4, &a) || fat_byte(f, c * 4 + 1, &b) ||
            fat_byte(f, c * 4 + 2, &c2) || fat_byte(f, c * 4 + 3, &d)) return 0xFFFFFFFFu;
        v = (a | ((uint32_t)b << 8) | ((uint32_t)c2 << 16) | ((uint32_t)d << 24)) & 0x0FFFFFFFu;
        return v >= 0x0FFFFFF7u ? 0xFFFFFFFFu : v;
    }
}

static uint64_t clu_off(struct fat* f, uint32_t c)
{
    return ((uint64_t)f->data_start + (uint64_t)(c - 2) * f->spc) * f->bps;
}

/* ---- names ---- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int name_eq(const char* a, const char* b)
{
    while (*a && *b) {
        if (lower(*a) != lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static uint32_t put_utf8(char* out, uint32_t cp)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static uint8_t sfn_sum(const uint8_t* n11)
{
    uint8_t s = 0;
    int i;
    for (i = 0; i < 11; i++) s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + n11[i]);
    return s;
}

/* ---- directory walking ---- */

/* callback returns non-zero to stop */
typedef int (*dir_cb)(void* ctx, const struct fent* e);

struct lfn_state {
    uint16_t u[270];
    int have;               /* collected a plausible sequence */
    int expect;             /* next sequence number we want (counting down) */
    uint8_t sum;
    int total;
};

static void lfn_reset(struct lfn_state* l) { l->have = 0; l->expect = 0; l->total = 0; }

/* returns 1 = stop (callback said so), 0 = keep going, -1 = error */
static int scan_entries(struct fat* f, const uint8_t* buf, uint32_t bytes, struct lfn_state* l,
                        dir_cb cb, void* ctx, int* end)
{
    uint32_t off;
    static struct fent ent;
    for (off = 0; off + 32 <= bytes; off += 32) {
        const uint8_t* d = buf + off;
        uint8_t attr;
        if (d[0] == 0x00) { *end = 1; return 0; }
        if (d[0] == 0xE5) { lfn_reset(l); continue; }
        attr = d[11];
        if ((attr & ATTR_LFN) == ATTR_LFN) {
            int seq = d[0] & 0x3F, i;
            if (d[0] & 0x40) {                                    /* last (= first physical) entry */
                lfn_reset(l);
                l->expect = seq;
                l->total = seq;
                l->sum = d[13];
                for (i = 0; i < 270; i++) l->u[i] = 0xFFFF;
            }
            if (l->expect != seq || seq < 1 || seq > 20 || d[13] != l->sum) { lfn_reset(l); continue; }
            {
                static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (i = 0; i < 13; i++)
                    l->u[(seq - 1) * 13 + i] = (uint16_t)(d[pos[i]] | (d[pos[i] + 1] << 8));
            }
            l->expect = seq - 1;
            if (seq == 1) l->have = 1;
            continue;
        }
        if (attr & ATTR_VOL) { lfn_reset(l); continue; }

        /* a real entry: build its name */
        {
            uint32_t n = 0;
            int use_lfn = l->have && l->expect == 0 && sfn_sum(d) == l->sum;
            if (use_lfn) {
                int i;
                for (i = 0; i < l->total * 13 && l->u[i] != 0x0000 && l->u[i] != 0xFFFF && n < 780; i++) {
                    uint32_t cp = l->u[i];
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < l->total * 13 &&
                        l->u[i + 1] >= 0xDC00 && l->u[i + 1] < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (l->u[i + 1] - 0xDC00);
                        i++;
                        ent.name[n++] = (char)(0xF0 | (cp >> 18));
                        ent.name[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        ent.name[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        ent.name[n++] = (char)(0x80 | (cp & 0x3F));
                        continue;
                    }
                    n += put_utf8(ent.name + n, cp);
                }
            } else {
                int i, bl = 8, el = 3, lowb = (d[12] & 0x08) != 0, lowe = (d[12] & 0x10) != 0;
                uint8_t first = d[0] == 0x05 ? 0xE5 : d[0];
                while (bl > 0 && d[bl - 1] == ' ') bl--;
                while (el > 0 && d[8 + el - 1] == ' ') el--;
                for (i = 0; i < bl; i++) {
                    char c = (char)(i == 0 ? first : d[i]);
                    ent.name[n++] = lowb ? lower(c) : c;
                }
                if (el) {
                    ent.name[n++] = '.';
                    for (i = 0; i < el; i++) ent.name[n++] = lowe ? lower((char)d[8 + i]) : (char)d[8 + i];
                }
            }
            ent.name[n] = 0;
            lfn_reset(l);
            if (n == 0) continue;
            if ((n == 1 && ent.name[0] == '.') || (n == 2 && ent.name[0] == '.' && ent.name[1] == '.')) continue;
            ent.attr = attr;
            ent.cluster = rd16(d + 26) | (f->type == 32 ? ((uint32_t)rd16(d + 20) << 16) : 0);
            ent.size = rd32(d + 28);
            if (cb(ctx, &ent)) return 1;
        }
    }
    return 0;
}

/* dir_cluster 0 = the fixed root directory of FAT12/16 */
static int dir_iter(struct fat* f, uint32_t dir_cluster, dir_cb cb, void* ctx)
{
    struct lfn_state* l = (struct lfn_state*)malloc(sizeof *l);
    int end = 0, rc = 0;
    if (!l) return -1;
    lfn_reset(l);

    if (dir_cluster == 0 && f->type != 32) {
        uint32_t s;
        for (s = 0; s < f->root_sectors && !end && rc == 0; s++) {
            if (blk_read(f->dev, ((uint64_t)f->root_start + s) * f->bps, f->dirbuf, f->bps) != 0) { rc = -1; break; }
            rc = scan_entries(f, f->dirbuf, f->bps, l, cb, ctx, &end);
        }
    } else {
        uint32_t c = dir_cluster ? dir_cluster : f->root_cluster, guard = 0;
        while (c != 0xFFFFFFFFu && !end && rc == 0 && guard++ < 1000000) {
            if (blk_read(f->dev, clu_off(f, c), f->dirbuf, f->cbytes) != 0) { rc = -1; break; }
            rc = scan_entries(f, f->dirbuf, f->cbytes, l, cb, ctx, &end);
            if (rc == 0) c = fat_next(f, c);
        }
    }
    free(l);
    return rc;
}

/* ---- lookup ---- */

struct node { uint32_t cluster; uint32_t size; uint8_t attr; int is_root; };

struct find_ctx { const char* name; struct node* out; int found; };

static int find_cb(void* ctx, const struct fent* e)
{
    struct find_ctx* fc = (struct find_ctx*)ctx;
    if (!name_eq(e->name, fc->name)) return 0;
    fc->out->cluster = e->cluster;
    fc->out->size = e->size;
    fc->out->attr = e->attr;
    fc->out->is_root = 0;
    fc->found = 1;
    return 1;
}

static int lookup(struct fat* f, const char* path, struct node* out)
{
    struct node cur;
    const char* p = path;
    cur.cluster = 0; cur.size = 0; cur.attr = ATTR_DIR; cur.is_root = 1;
    for (;;) {
        char comp[300];
        uint32_t n = 0;
        struct find_ctx fc;
        while (*p == '/') p++;
        if (!*p) break;
        while (*p && *p != '/' && n < 299) comp[n++] = *p++;
        if (*p && *p != '/') return -1;
        comp[n] = 0;
        if (n == 1 && comp[0] == '.') continue;
        if (!(cur.attr & ATTR_DIR)) return -1;
        fc.name = comp; fc.out = &cur; fc.found = 0;
        {
            struct node next;
            fc.out = &next;
            if (dir_iter(f, cur.is_root ? 0 : cur.cluster, find_cb, &fc) < 0 || !fc.found) return -1;
            cur = next;
            if ((cur.attr & ATTR_DIR) && cur.cluster == 0) cur.is_root = 1;     /* ".." style pointer to the root */
        }
    }
    *out = cur;
    return 0;
}

/* ---- file data ---- */

static int64_t read_data(struct fat* f, const struct node* nd, uint64_t off, uint8_t* buf, uint64_t len)
{
    uint64_t done = 0;
    uint32_t idx, c;
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = nd->size - off;
    if (nd->cluster < 2) return 0;

    idx = (uint32_t)(off / f->cbytes);
    if (f->cc_first == nd->cluster && f->cc_idx <= idx && f->cc_clu) { c = f->cc_clu; }
    else { c = nd->cluster; f->cc_first = nd->cluster; f->cc_idx = 0; f->cc_clu = c; }
    while (f->cc_idx < idx) {
        c = fat_next(f, c);
        if (c == 0xFFFFFFFFu) return -1;
        f->cc_idx++;
        f->cc_clu = c;
    }

    while (done < len) {
        uint64_t pos = off + done;
        uint32_t coff = (uint32_t)(pos % f->cbytes);
        uint32_t take = f->cbytes - coff;
        if (take > len - done) take = (uint32_t)(len - done);
        if (coff == 0 && take == f->cbytes && (((uintptr_t)(buf + done)) & 3) == 0) {
            /* whole clusters: gather a contiguous run in one device read */
            uint32_t run = 1, last = c;
            while (done + (uint64_t)(run + 1) * f->cbytes <= len && run < 32) {
                uint32_t nx = fat_next(f, last);
                if (nx != last + 1) break;
                last = nx;
                run++;
            }
            if (blk_read(f->dev, clu_off(f, c), buf + done, run * f->cbytes) != 0) return -1;
            take = run * f->cbytes;
            done += take;
            f->cc_idx += run - 1; f->cc_clu = last;
            c = last;
            if (done < len) {
                c = fat_next(f, c);
                if (c == 0xFFFFFFFFu) return -1;
                f->cc_idx++; f->cc_clu = c;
            }
            continue;
        } else {
            if (blk_read(f->dev, clu_off(f, c) + coff, buf + done, take) != 0) return -1;
        }
        done += take;
        if (coff + take == f->cbytes && done < len) {
            c = fat_next(f, c);
            if (c == 0xFFFFFFFFu) return -1;
            f->cc_idx++; f->cc_clu = c;
        }
    }
    return (int64_t)done;
}

/* ---- VFS operations ---- */

static int f_file_exists(void* sb, const char* p) {
    struct node n;
    if (!sb || lookup((struct fat*)sb, p, &n) != 0) return 0;
    return !(n.attr & ATTR_DIR);
}

static int f_dir_exists(void* sb, const char* p) {
    struct node n;
    if (!sb || lookup((struct fat*)sb, p, &n) != 0) return 0;
    return (n.attr & ATTR_DIR) != 0;
}

static uint32_t f_get_file_size(void* sb, const char* p) {
    struct node n;
    if (!sb || lookup((struct fat*)sb, p, &n) != 0 || (n.attr & ATTR_DIR)) return 0;
    return n.size;
}

static int f_read_file_range(void* sb, const char* p, uint32_t offset, char* buf, uint32_t cap, uint32_t* size)
{
    struct node n;
    int64_t got;
    if (!sb || !buf || !size) return -1;
    *size = 0;
    if (lookup((struct fat*)sb, p, &n) != 0 || (n.attr & ATTR_DIR)) return -1;
    got = read_data((struct fat*)sb, &n, offset, (uint8_t*)buf, cap);
    if (got < 0) return -1;
    *size = (uint32_t)got;
    return 0;
}

static int f_read_file(void* sb, const char* p, char* buf, uint32_t* size)
{
    uint32_t cap, got = 0;
    if (!buf || !size) return -1;
    cap = *size;
    if (cap == 0) { buf[0] = 0; return f_file_exists(sb, p) ? 0 : -1; }
    if (f_read_file_range(sb, p, 0, buf, cap, &got) != 0) { buf[0] = 0; *size = 0; return -1; }
    buf[got] = 0;
    *size = got;
    return 0;
}

struct list_ctx { char* out; uint32_t pos; };

static int list_cb(void* ctx, const struct fent* e)
{
    struct list_ctx* l = (struct list_ctx*)ctx;
    uint32_t nl = rfs_len(e->name), i;
    if (l->pos + nl + 3 > LIST_LIMIT) return 1;
    if (l->pos) l->out[l->pos++] = '\n';
    for (i = 0; i < nl; i++) l->out[l->pos++] = e->name[i] == '\n' ? '?' : e->name[i];
    if (e->attr & ATTR_DIR) l->out[l->pos++] = '/';
    l->out[l->pos] = 0;
    return 0;
}

static int f_list_dir(void* sb, const char* p, char* out, uint32_t* size)
{
    struct fat* f = (struct fat*)sb;
    struct node n;
    struct list_ctx l;
    if (!f || !out || !size) return -1;
    *size = 0; out[0] = 0;
    if (lookup(f, p, &n) != 0 || !(n.attr & ATTR_DIR)) return -1;
    l.out = out; l.pos = 0;
    if (dir_iter(f, n.is_root ? 0 : n.cluster, list_cb, &l) < 0) return -1;
    *size = l.pos;
    return 0;
}

static int f_dir_is_empty(void* sb, const char* p) {
    static char tmp[LIST_LIMIT + 8];
    uint32_t sz = 0;
    if (!f_dir_exists(sb, p)) return 0;
    if (f_list_dir(sb, p, tmp, &sz) != 0) return 0;
    return sz == 0;
}

static int f_get_mode(void* sb, const char* p) {
    struct node n;
    if (!sb || lookup((struct fat*)sb, p, &n) != 0) return 0644;
    if (n.attr & ATTR_DIR) return 0755;
    return (n.attr & ATTR_RO) ? 0444 : 0644;
}

static int f_get_owner(void* sb, const char* p) { (void)sb; (void)p; return 0; }

static int f_ro1(void* sb, const char* p) { (void)sb; (void)p; return VFS_EACCES; }
static int f_ro_w(void* sb, const char* p, const char* d, uint32_t n) { (void)sb; (void)p; (void)d; (void)n; return VFS_EACCES; }
static int f_ro_chmod(void* sb, const char* p, int v) { (void)sb; (void)p; (void)v; return VFS_EACCES; }
static int f_ro_at(void* sb, const char* p, uint32_t o, const char* d, uint32_t n) { (void)sb; (void)p; (void)o; (void)d; (void)n; return VFS_EACCES; }
static int f_ro_prealloc(void* sb, const char* p, uint32_t n) { (void)sb; (void)p; (void)n; return VFS_EACCES; }

/* ---- probe / mount ---- */

static int parse_bpb(const uint8_t* b, struct fat* f)
{
    uint32_t root_sec;
    uint32_t data_sectors;
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;
    f->bps = rd16(b + 11);
    f->spc = b[13];
    f->reserved = rd16(b + 14);
    f->nfats = b[16];
    f->root_ents = rd16(b + 17);
    f->total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    f->fatsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    if (f->bps != 512 && f->bps != 1024 && f->bps != 2048 && f->bps != 4096) return 0;
    if (f->spc == 0 || (f->spc & (f->spc - 1))) return 0;
    if (f->reserved == 0 || f->nfats == 0 || f->nfats > 4 || f->fatsz == 0 || f->total == 0) return 0;
    if (b[21] < 0xF0) return 0;                                       /* media descriptor */
    root_sec = (f->root_ents * 32 + f->bps - 1) / f->bps;
    if (f->reserved + f->nfats * f->fatsz + root_sec >= f->total) return 0;
    data_sectors = f->total - (f->reserved + f->nfats * f->fatsz + root_sec);
    f->clusters = data_sectors / f->spc;
    f->root_sectors = root_sec;
    f->root_start = f->reserved + f->nfats * f->fatsz;
    f->data_start = f->root_start + root_sec;
    f->type = f->clusters < 4085 ? 12 : (f->clusters < 65525 ? 16 : 32);
    if (f->type == 32) {
        f->root_cluster = rd32(b + 44);
        if (f->root_ents != 0 || f->root_cluster < 2) return 0;
    } else if (f->root_ents == 0) {
        return 0;
    }
    f->cbytes = f->bps * f->spc;
    return 1;
}

/* 12, 16 or 32 when the device holds a FAT filesystem, else 0 */
int fatfs_probe(struct vfs_bdev* dev)
{
    uint8_t b[512];
    struct fat f;
    rfs_zero(&f, sizeof f);
    if (!dev || blk_read(dev, 0, b, sizeof b) != 0) return 0;
    if (b[3] == 'N' && b[4] == 'T' && b[5] == 'F' && b[6] == 'S') return 0;
    if (b[3] == 'E' && b[4] == 'X' && b[5] == 'F' && b[6] == 'A' && b[7] == 'T') return 0;
    return parse_bpb(b, &f) ? f.type : 0;
}

static int f_mount(void** sbp, struct vfs_bdev* dev, const char* opts)
{
    uint8_t b[512];
    struct fat* f;
    (void)opts;
    if (!dev || !dev->present || !dev->read) return VFS_EINVAL;
    if (blk_read(dev, 0, b, sizeof b) != 0) return VFS_ERR;
    f = (struct fat*)malloc(sizeof *f);
    if (!f) return VFS_ENOSPC;
    rfs_zero(f, sizeof *f);
    f->dev = dev;
    if (!parse_bpb(b, f)) { free(f); return VFS_ENOENT; }
    f->fatbuf = (uint8_t*)malloc(f->bps);
    f->dirbuf = (uint8_t*)malloc(f->cbytes > f->bps ? f->cbytes : f->bps);
    f->fat_cached = -1;
    if (!f->fatbuf || !f->dirbuf) { free(f->fatbuf); free(f->dirbuf); free(f); return VFS_ENOSPC; }

    print("fatfs: ");
    print(dev->name);
    print(f->type == 12 ? ": FAT12" : f->type == 16 ? ": FAT16" : ": FAT32");
    print(" (vfat), read-only\n");
    *sbp = f;
    return VFS_OK;
}

static void f_unmount(void* sb)
{
    struct fat* f = (struct fat*)sb;
    if (!f) return;
    free(f->fatbuf); free(f->dirbuf); free(f);
}

static struct vfs_ops fat_ops;

const struct vfs_ops* fatfs_get_ops(void)
{
    struct vfs_ops* o = &fat_ops;
    rfs_zero(o, sizeof *o);
    o->name = "vfat";
    o->create_file = f_ro1;
    o->create_dir = f_ro1;
    o->delete_file = f_ro1;
    o->delete_dir = f_ro1;
    o->delete_dir_recursive = f_ro1;
    o->write_file = f_ro_w;
    o->append_file = f_ro_w;
    o->read_file = f_read_file;
    o->read_file_range = f_read_file_range;
    o->get_file_size = f_get_file_size;
    o->file_exists = f_file_exists;
    o->dir_exists = f_dir_exists;
    o->dir_is_empty = f_dir_is_empty;
    o->list_dir = f_list_dir;
    o->chmod = f_ro_chmod;
    o->chown = f_ro_chmod;
    o->get_owner = f_get_owner;
    o->get_mode = f_get_mode;
    o->write_at = f_ro_at;
    o->prealloc = f_ro_prealloc;
    o->mount = f_mount;
    o->unmount = f_unmount;
    return o;
}
