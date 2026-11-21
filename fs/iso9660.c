#include "../include/iso9660.h"
#include "../include/vfs.h"

/* TanjaOS ISO9660 reader - see include/iso9660.h.
 *
 * Everything is read on demand through the block device the volume lives
 * on (struct vfs_bdev, 512-byte sectors; one ISO block = 4 sectors).
 * Directory blocks go through a small block cache: `ls` on a directory
 * with a few hundred entries stats every entry, and without the cache
 * each of those would re-read the directory from a (slow) optical drive.
 */

#define BLK        2048u
#define SECT_PER_BLK 4u

/* All state of one ISO volume lives in a struct iso_vol (include/iso9660.h),
 * so the boot ISO and a second ISO/disk mounted later can be read at the
 * same time. The routines below work on the "current" volume V; every
 * public entry point sets V first (single-threaded kernel). */
static struct iso_vol iso_primary;
static struct iso_vol* V = &iso_primary;
#define iso_dev      (V->dev)
#define iso_active   (V->active)
#define root_extent  (V->root_extent)
#define root_size    (V->root_size)
#define iso_style    (V->style)
#define rr_skip      (V->rr_skip)
#define iso_label    (V->label)

static const char* const style_names[] = { "plain", "joliet", "rockridge" };

/* small helpers (this file does not depend on libc)                 */

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mcopy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}

static int bytes_eq(const uint8_t* a, const char* b, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

static uint32_t strlen_local(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* block I/O                                                         */

/* Read `count` ISO blocks straight into buf (no cache). */
static int read_raw(uint32_t lba, uint32_t count, void* buf) {
    uint8_t* p = (uint8_t*)buf;
    if (!iso_dev || !iso_dev->read) return -1;
    while (count) {
        uint32_t n = count > 32 ? 32 : count;      /* 128 sectors per call */
        if (iso_dev->read(iso_dev, (uint64_t)lba * SECT_PER_BLK,
                          n * SECT_PER_BLK, p) != 0)
            return -1;
        p += n * BLK;
        lba += n;
        count -= n;
    }
    return 0;
}

#define BC_N 64
static uint8_t  bc_data[BC_N][BLK];
static uint32_t bc_tag[BC_N];              /* lba + 1, 0 = empty */
static const struct iso_vol* bc_vol[BC_N]; /* which volume the block belongs to */

static void bc_flush(void) {
    int i;
    for (i = 0; i < BC_N; i++) bc_tag[i] = 0;
}

/* Pointer into the cache; valid only until the next get_block() call. */
static const uint8_t* get_block(uint32_t lba) {
    uint32_t idx = (lba ^ (lba >> 6)) & (BC_N - 1);
    if (bc_tag[idx] == lba + 1 && bc_vol[idx] == V) return bc_data[idx];
    if (read_raw(lba, 1, bc_data[idx]) != 0) {
        bc_tag[idx] = 0;
        return 0;
    }
    bc_tag[idx] = lba + 1;
    bc_vol[idx] = V;
    return bc_data[idx];
}

/* directory records                                                 */

struct iso_ent {
    char     name[256];
    uint32_t extent;
    uint32_t size;
    uint16_t mode;
    uint8_t  is_dir;
    uint8_t  is_link;
    char     link[256];       /* Rock Ridge symlink target ("" = none) */
};

struct rr_info {
    char     link[256];
    int      llen;
    int      have_sl;
    int      link_cont;
    char     name[256];
    int      nlen;
    int      have_nm;
    uint32_t mode;
    int      have_px;
};

/* Walk a System Use area. `follow_ce` allows one level of continuation
 * area (the continuation block is only valid until the next get_block()
 * call, so a CE inside a CE is not followed). */
static void rr_scan(const uint8_t* su, uint32_t sulen, struct rr_info* ri,
                    int follow_ce, uint32_t* sp_skip_out)
{
    uint32_t i = 0;
    while (i + 4 <= sulen) {
        uint8_t a = su[i], b = su[i + 1], l = su[i + 2];
        if (l < 4 || i + l > sulen) break;

        if (a == 'N' && b == 'M') {
            uint8_t fl = su[i + 4];
            if (!(fl & 0x06)) {                   /* not "." / ".." */
                int k;
                for (k = 5; k < l && ri->nlen < 255; k++)
                    ri->name[ri->nlen++] = (char)su[i + k];
                ri->name[ri->nlen] = 0;
            }
            ri->have_nm = 1;
        } else if (a == 'S' && b == 'L' && l >= 5) {
            /* symlink target: a list of component records
             * (flags, length, text); flags: 1 = continues in the next
             * record, 2 = ".", 4 = "..", 8 = root "/" */
            uint32_t k = i + 5;
            ri->have_sl = 1;
            while (k + 2 <= (uint32_t)i + l) {
                uint8_t cf = su[k], cl = su[k + 1];
                uint32_t m;
                if (k + 2 + cl > (uint32_t)i + l) break;
                if (cf & 8) {
                    ri->llen = 0;
                    if (ri->llen < 255) ri->link[ri->llen++] = '/';
                } else {
                    if (ri->llen > 0 && ri->link[ri->llen - 1] != '/' &&
                        !ri->link_cont && ri->llen < 255)
                        ri->link[ri->llen++] = '/';
                    if (cf & 2) { if (ri->llen < 255) ri->link[ri->llen++] = '.'; }
                    else if (cf & 4) {
                        if (ri->llen < 254) { ri->link[ri->llen++] = '.'; ri->link[ri->llen++] = '.'; }
                    } else {
                        for (m = 0; m < cl && ri->llen < 255; m++)
                            ri->link[ri->llen++] = (char)su[k + 2 + m];
                    }
                }
                ri->link_cont = (cf & 1) ? 1 : 0;
                k += 2 + cl;
            }
            ri->link[ri->llen] = 0;
        } else if (a == 'P' && b == 'X' && l >= 12) {
            ri->mode = le32(su + i + 4);
            ri->have_px = 1;
        } else if (a == 'S' && b == 'P' && l >= 7 &&
                   su[i + 4] == 0xBE && su[i + 5] == 0xEF) {
            if (sp_skip_out) *sp_skip_out = su[i + 6];
        } else if (a == 'C' && b == 'E' && l >= 28 && follow_ce) {
            uint32_t blk = le32(su + i + 4);
            uint32_t off = le32(su + i + 12);
            uint32_t len = le32(su + i + 20);
            const uint8_t* cb = get_block(blk);
            if (cb && off < BLK && len <= BLK - off)
                rr_scan(cb + off, len, ri, 0, sp_skip_out);
            return;      /* `su` may have been evicted from the cache */
        } else if (a == 'S' && b == 'T') {
            break;
        }
        i += l;
    }
}

/* Convert a Joliet (UCS-2BE) name to UTF-8. */
static int joliet_name(const uint8_t* id, int idlen, char* out, int cap) {
    int n = 0, i;
    for (i = 0; i + 1 < idlen; i += 2) {
        uint32_t cp = ((uint32_t)id[i] << 8) | id[i + 1];
        if (cp < 0x80) {
            if (n + 1 >= cap) break;
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            if (n + 2 >= cap) break;
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (n + 3 >= cap) break;
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = 0;
    return n;
}

/* Strip the ";1" version suffix and a dangling '.' from a plain name. */
static void trim_version(char* s, int n) {
    int i;
    for (i = n - 1; i > 0; i--) {
        if (s[i] == ';') { s[i] = 0; n = i; break; }
        if (s[i] < '0' || s[i] > '9') break;
    }
    if (n > 1 && s[n - 1] == '.') s[n - 1] = 0;
}

/* Parse one directory record (a local copy, `len` bytes). Returns 0 for a
 * normal entry, 1 for ".", 2 for "..", -1 for a record that cannot be used. */
static int parse_record(const uint8_t* r, uint32_t len, struct iso_ent* e)
{
    uint32_t idlen, su_off, flags;
    const uint8_t* id;

    if (len < 34) return -1;
    idlen = r[32];
    if (33 + idlen > len) return -1;
    id = r + 33;

    if (idlen == 1 && id[0] == 0) return 1;
    if (idlen == 1 && id[0] == 1) return 2;

    flags = r[25];
    e->is_dir  = (flags & 2) ? 1 : 0;
    e->is_link = 0;
    e->extent  = le32(r + 2) + r[1];         /* skip extended attribute blocks */
    e->size    = le32(r + 10);
    e->mode    = e->is_dir ? 0755 : 0644;
    e->name[0] = 0;
    e->link[0] = 0;

    if (iso_style == 2) {
        struct rr_info ri;
        su_off = 33 + idlen + ((idlen & 1) ? 0 : 1);
        ri.nlen = 0; ri.name[0] = 0; ri.have_nm = 0; ri.have_px = 0; ri.mode = 0;
        ri.llen = 0; ri.link[0] = 0; ri.have_sl = 0; ri.link_cont = 0;
        if (su_off + rr_skip < len)
            rr_scan(r + su_off + rr_skip, len - su_off - rr_skip, &ri, 1, 0);
        if (ri.have_px) {
            e->mode = (uint16_t)(ri.mode & 0777);
            if ((ri.mode & 0170000) == 0120000) e->is_link = 1;
        }
        if (ri.have_sl && ri.llen > 0) {
            e->is_link = 1;
            mcopy(e->link, ri.link, (uint32_t)ri.llen + 1);
        }
        if (ri.have_nm && ri.nlen > 0) {
            mcopy(e->name, ri.name, (uint32_t)ri.nlen + 1);
            return 0;
        }
    }

    if (iso_style == 1) {
        joliet_name(id, (int)idlen, e->name, sizeof(e->name));
        trim_version(e->name, (int)strlen_local(e->name));
    } else {
        uint32_t k;
        for (k = 0; k < idlen && k < 255; k++) e->name[k] = (char)id[k];
        e->name[k] = 0;
        trim_version(e->name, (int)k);
    }
    return e->name[0] ? 0 : -1;
}

/* directory walking                                                 */

typedef int (*dir_cb)(const struct iso_ent* e, void* ctx);

/* Where the record most recently handed to a dir_walk callback lives (ISO
 * block + byte offset inside it). iso_v_update_file() uses this to patch the
 * size fields of the record it just looked up. */
static uint32_t walk_rec_lba;
static uint32_t walk_rec_bo;

/* Calls cb for every entry (never "." or ".."). A non-zero return from cb
 * stops the walk and is passed back. -1 = I/O error / corrupt directory. */
static int dir_walk(uint32_t ext, uint32_t size, dir_cb cb, void* ctx)
{
    static struct iso_ent e;                 /* too big for a comfy stack frame */
    uint8_t rec[256];
    uint32_t off = 0;

    while (off < size) {
        uint32_t lba = ext + off / BLK;
        uint32_t bo  = off % BLK;
        const uint8_t* blk = get_block(lba);   /* re-fetched every record: a
                                                  CE lookup below may evict it */
        uint32_t len;
        int rc;
        if (!blk) return -1;
        len = blk[bo];
        if (len == 0) {                       /* padding to the next block */
            off = (off / BLK + 1) * BLK;
            continue;
        }
        if (bo + len > BLK) return -1;
        mcopy(rec, blk + bo, len);
        walk_rec_lba = lba;
        walk_rec_bo  = bo;
        off += len;

        rc = parse_record(rec, len, &e);
        if (rc != 0) continue;
        rc = cb(&e, ctx);
        if (rc) return rc;
    }
    return 0;
}

struct find_ctx {
    const char*     name;
    struct iso_ent* out;
};

static int find_cb(const struct iso_ent* e, void* vctx) {
    struct find_ctx* c = (struct find_ctx*)vctx;
    if (!streq(e->name, c->name)) return 0;
    *c->out = *e;
    return 1;
}

#define LOOKUP_DEPTH 32
#define LOOKUP_HOPS  8

/* Walk `path` from the root of the ISO. "." and ".." are resolved, and a
 * Rock Ridge symlink met on the way (also as the last component) is
 * replaced by its target and the walk restarted - at most LOOKUP_HOPS times,
 * so a link loop just fails. */
static int iso_lookup_impl(const char* path, struct iso_node* out)
{
    static char work[640];
    static struct iso_ent cur, next;
    struct { uint32_t ext, size; } dirs[LOOKUP_DEPTH];
    char comp[256];
    int hops = 0, depth, i, k;

    if (!iso_active || !path || path[0] != '/' || !out) return -1;
    for (k = 0; path[k] && k < 639; k++) work[k] = path[k];
    work[k] = 0;

restart:
    depth = 0;
    dirs[0].ext = root_extent; dirs[0].size = root_size;
    cur.extent = root_extent; cur.size = root_size;
    cur.is_dir = 1; cur.is_link = 0; cur.mode = 0755; cur.name[0] = 0; cur.link[0] = 0;
    i = 0;

    for (;;) {
        int n = 0, start;
        while (work[i] == '/') i++;
        if (!work[i]) break;
        start = i;
        while (work[i] && work[i] != '/' && n < 255) comp[n++] = work[i++];
        comp[n] = 0;
        if (work[i] && work[i] != '/') return -1;        /* component too long */
        if (n == 1 && comp[0] == '.') continue;
        if (n == 2 && comp[0] == '.' && comp[1] == '.') {
            if (depth > 0) depth--;
            cur.extent = dirs[depth].ext; cur.size = dirs[depth].size;
            cur.is_dir = 1; cur.is_link = 0; cur.mode = 0755;
            continue;
        }

        if (!cur.is_dir) return -1;
        {
            struct find_ctx fc;
            fc.name = comp; fc.out = &next;
            if (dir_walk(cur.extent, cur.size, find_cb, &fc) != 1) return -1;
        }

        if (next.is_link && next.link[0]) {
            /* splice: <target> + rest of the path; a relative target is
             * relative to the directory the link lives in */
            static char tmp[640];
            int t = 0, m;
            if (++hops > LOOKUP_HOPS) return -1;
            if (next.link[0] != '/') {
                for (m = 0; m < start && t < 600; m++) tmp[t++] = work[m];
                if (t > 0 && tmp[t - 1] != '/') tmp[t++] = '/';
            }
            for (m = 0; next.link[m] && t < 600; m++) tmp[t++] = next.link[m];
            for (m = i; work[m] && t < 639; m++) tmp[t++] = work[m];
            tmp[t] = 0;
            for (m = 0; m <= t; m++) work[m] = tmp[m];
            goto restart;
        }

        cur = next;
        if (cur.is_dir && depth < LOOKUP_DEPTH - 1) {
            depth++;
            dirs[depth].ext = cur.extent; dirs[depth].size = cur.size;
        }
    }
    out->extent  = cur.extent;
    out->size    = cur.size;
    out->mode    = cur.mode;
    out->is_dir  = cur.is_dir;
    out->is_link = cur.is_link;
    return 0;
}

/* listing                                                           */

struct list_ctx {
    char*    buf;
    uint32_t cap;       /* usable bytes (excluding the terminating NUL) */
    uint32_t pos;
    int      want_dirs;
};

static int list_cb(const struct iso_ent* e, void* vctx) {
    struct list_ctx* c = (struct list_ctx*)vctx;
    uint32_t nlen = strlen_local(e->name);
    uint32_t need = nlen + (e->is_dir ? 1 : 0) + (c->pos ? 1 : 0);
    if ((e->is_dir ? 1 : 0) != c->want_dirs) return 0;
    if (c->pos + need > c->cap) return 0;            /* silently truncate */
    if (c->pos) c->buf[c->pos++] = '\n';
    mcopy(c->buf + c->pos, e->name, nlen);
    c->pos += nlen;
    if (e->is_dir) c->buf[c->pos++] = '/';
    c->buf[c->pos] = 0;
    return 0;
}

static int iso_list_impl(const char* path, char* buf, uint32_t cap, uint32_t* size)
{
    struct iso_node n;
    struct list_ctx c;
    if (!buf || !size || cap < 2) return -1;
    *size = 0;
    buf[0] = 0;
    if (iso_lookup_impl(path, &n) != 0 || !n.is_dir) return -1;

    c.buf = buf; c.cap = cap - 1; c.pos = 0;
    c.want_dirs = 1;
    if (dir_walk(n.extent, n.size, list_cb, &c) < 0) return -1;
    c.want_dirs = 0;
    if (dir_walk(n.extent, n.size, list_cb, &c) < 0) return -1;
    *size = c.pos;
    return 0;
}

static int any_cb(const struct iso_ent* e, void* ctx) {
    (void)e; (void)ctx;
    return 1;
}

static int iso_dir_is_empty_impl(const char* path)
{
    struct iso_node n;
    if (iso_lookup_impl(path, &n) != 0 || !n.is_dir) return 0;
    return dir_walk(n.extent, n.size, any_cb, 0) == 0;
}

/* file data                                                         */

static int iso_read_range_impl(const struct iso_node* n, uint32_t offset,
                   void* buf, uint32_t cap, uint32_t* got)
{
    uint8_t* out = (uint8_t*)buf;
    uint32_t want, done = 0;
    if (!iso_active || !n || !got) return -1;
    *got = 0;
    if (n->is_link || offset >= n->size || cap == 0) return 0;
    want = n->size - offset;
    if (want > cap) want = cap;

    while (done < want) {
        uint32_t pos  = offset + done;
        uint32_t lba  = n->extent + pos / BLK;
        uint32_t bo   = pos % BLK;
        uint32_t left = want - done;

        if (bo == 0 && left >= BLK) {
            /* whole blocks: straight into the caller's buffer */
            uint32_t blocks = left / BLK;
            if (read_raw(lba, blocks, out + done) != 0) { *got = done; return -1; }
            done += blocks * BLK;
        } else {
            const uint8_t* b = get_block(lba);
            uint32_t take = BLK - bo;
            if (!b) { *got = done; return -1; }
            if (take > left) take = left;
            mcopy(out + done, b + bo, take);
            done += take;
        }
    }
    *got = done;
    return 0;
}

/* mounting                                                          */

/* Does the root directory's "." record carry Rock Ridge (SUSP) entries? */
static int detect_rockridge(uint32_t ext, uint32_t* skip_out)
{
    const uint8_t* b = get_block(ext);
    uint8_t rec[256];
    uint32_t len, su_off, idlen;
    struct rr_info ri;
    uint32_t skip = 0;
    int found = 0;
    uint32_t i;

    *skip_out = 0;
    if (!b) return 0;
    len = b[0];
    if (len < 34) return 0;
    mcopy(rec, b, len);
    idlen = rec[32];
    su_off = 33 + idlen + ((idlen & 1) ? 0 : 1);
    if (su_off >= len) return 0;

    /* look for SP / RR / PX / NM markers directly (no CE following here) */
    ri.nlen = 0; ri.name[0] = 0; ri.have_nm = 0; ri.have_px = 0; ri.mode = 0;
    ri.llen = 0; ri.link[0] = 0; ri.have_sl = 0; ri.link_cont = 0;
    for (i = su_off; i + 4 <= len; ) {
        uint8_t a = rec[i], c = rec[i + 1], l = rec[i + 2];
        if (l < 4 || i + l > len) break;
        if ((a == 'S' && c == 'P' && l >= 7 && rec[i + 4] == 0xBE && rec[i + 5] == 0xEF)) {
            skip = rec[i + 6];
            found = 1;
        } else if ((a == 'R' && c == 'R') || (a == 'P' && c == 'X') ||
                   (a == 'N' && c == 'M')) {
            found = 1;
        }
        i += l;
    }
    (void)ri;
    *skip_out = skip;
    return found;
}

static int try_device(struct vfs_bdev* d)
{
    static uint8_t sect[BLK];
    uint8_t pvd_root[34], jol_root[34];
    int have_pvd = 0, have_jol = 0;
    uint32_t blk;
    char label[40];
    int i;

    iso_dev = d;
    bc_flush();

    for (blk = 16; blk < 16 + 24; blk++) {
        uint8_t type;
        if (read_raw(blk, 1, sect) != 0) { if (blk == 16) return 0; break; }
        if (!bytes_eq(sect + 1, "CD001", 5) || sect[6] != 1) {
            if (blk == 16) return 0;                 /* not an ISO9660 volume */
            break;
        }
        type = sect[0];
        if (type == 255) break;
        if (type == 1 && !have_pvd) {
            uint32_t bsz = (uint32_t)sect[128] | ((uint32_t)sect[129] << 8);
            if (bsz != BLK) return 0;                /* only 2048-byte blocks */
            mcopy(pvd_root, sect + 156, 34);
            for (i = 0; i < 32; i++) label[i] = (char)sect[40 + i];
            label[32] = 0;
            have_pvd = 1;
        } else if (type == 2 && !have_jol && sect[88] == '%' && sect[89] == '/' &&
                   (sect[90] == '@' || sect[90] == 'C' || sect[90] == 'E')) {
            mcopy(jol_root, sect + 156, 34);
            have_jol = 1;
        }
    }
    if (!have_pvd) return 0;

    /* trim the volume label */
    i = 31;
    while (i >= 0 && (label[i] == ' ' || label[i] == 0)) { label[i] = 0; i--; }
    for (i = 0; i < 32; i++) iso_label[i] = label[i];
    iso_label[32] = 0;

    {
        uint32_t skip = 0;
        uint32_t pext = le32(pvd_root + 2) + pvd_root[1];
        rr_skip = 0;
        if (detect_rockridge(pext, &skip)) {
            iso_style = 2;
            rr_skip = skip;
            root_extent = pext;
            root_size = le32(pvd_root + 10);
        } else if (have_jol) {
            iso_style = 1;
            root_extent = le32(jol_root + 2) + jol_root[1];
            root_size = le32(jol_root + 10);
        } else {
            iso_style = 0;
            root_extent = pext;
            root_size = le32(pvd_root + 10);
        }
    }
    iso_active = 1;
    return 1;
}

int iso_probe(void)
{
    int n = vfs_bdev_count();
    int pass, i;

    V = &iso_primary;
    iso_active = 0;
    iso_dev = 0;
    iso_label[0] = 0;
    bc_flush();

    /* Prefer optical drives, then removable media (such as a USB installer),
     * and only then a fixed disk: an installed TanjaOS disk carries its own
     * boot image (GRUB, kernel, grub.cfg) at the front, and that is what
     * /boot shows. A live/installer medium, when present, always wins, so an
     * installer menu can never be mistaken for the installed one; edits to
     * the boot config are written back to the image by kernel/store.c. */
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < n; i++) {
            struct vfs_bdev* d = vfs_bdev_at(i);
            int is_sr;
            if (!d || !d->present || !d->read) continue;
            is_sr = (d->name[0] == 's' && d->name[1] == 'r');
            if (pass == 0) {
                if (!is_sr) continue;
            } else if (pass == 1) {
                if (is_sr || !d->removable) continue;
            } else {
                if (is_sr || d->removable) continue;
            }
            if (try_device(d)) return 1;
        }
    }
    iso_dev = 0;
    iso_active = 0;
    return 0;
}

/* ---- explicit-volume API (a second ISO / disk besides the boot one) ---- */

int iso_vol_open(struct iso_vol* v, struct vfs_bdev* d)
{
    if (!v || !d || !d->present || !d->read) return 0;
    V = v;
    v->active = 0;
    v->dev = 0;
    v->label[0] = 0;
    if (try_device(d)) return 1;
    v->dev = 0;
    v->active = 0;
    return 0;
}

/* Largest file iso_v_update_file() will write (8 blocks). */
#define UPD_MAX (8u * BLK)

/* Replace the contents of an existing, regular file of an ISO9660 volume
 * IN PLACE, for a volume on a writable disk (the boot image the installer
 * dd'd to the front of the disk). An ISO cannot grow or move files, so the
 * new contents have to fit in the blocks the file already owns; the data is
 * zero-padded to a block boundary and the size fields of the file's
 * directory record are patched (both-endian, ECMA-119 9.1.4).
 *
 * Returns 0 on success, -1 on I/O error or not found / not a plain file,
 * -2 if the data does not fit (*cap_out, if given, receives the number of
 * bytes the file can hold). */
int iso_v_update_file(struct iso_vol* v, const char* path,
                      const void* data, uint32_t len, uint32_t* cap_out)
{
    static uint8_t buf[UPD_MAX];
    static uint8_t dblk[BLK];
    struct iso_node n;
    uint32_t blocks, cap, i, pos;
    int rc;

    if (cap_out) *cap_out = 0;
    if (!v || !v->active || !v->dev || !v->dev->write || !path || (!data && len)) return -1;
    V = v;
    if (iso_lookup_impl(path, &n) != 0) return -1;
    if (n.is_dir || n.is_link) return -1;

    /* iso_lookup_impl() walked the parent directory last, so walk_rec_* is
     * the record of the file itself (a Rock Ridge symlink hop would break
     * that, and links are refused above). */
    pos = walk_rec_bo;
    i   = walk_rec_lba;

    cap = ((n.size + BLK - 1) / BLK) * BLK;
    if (cap_out) *cap_out = cap;
    if (cap == 0 || len > cap) return -2;
    if (len > UPD_MAX) return -2;

    blocks = (len + BLK - 1) / BLK;
    if (blocks == 0) blocks = 1;
    for (rc = 0; rc < (int)(blocks * BLK); rc++) buf[rc] = 0;
    mcopy(buf, data, len);

    /* 1. data blocks first, then 2. the directory record that points at
     * them: an interrupted update leaves either the old file or the new
     * data with the old size, never a record pointing at garbage. */
    if (v->dev->write(v->dev, (uint64_t)n.extent * SECT_PER_BLK,
                      blocks * SECT_PER_BLK, buf) != 0)
        return -1;

    if (read_raw(i, 1, dblk) != 0) return -1;
    if (pos + 18 > BLK) return -1;
    dblk[pos + 10] = (uint8_t)(len);        dblk[pos + 11] = (uint8_t)(len >> 8);
    dblk[pos + 12] = (uint8_t)(len >> 16);  dblk[pos + 13] = (uint8_t)(len >> 24);
    dblk[pos + 14] = (uint8_t)(len >> 24);  dblk[pos + 15] = (uint8_t)(len >> 16);
    dblk[pos + 16] = (uint8_t)(len >> 8);   dblk[pos + 17] = (uint8_t)(len);
    if (v->dev->write(v->dev, (uint64_t)i * SECT_PER_BLK, SECT_PER_BLK, dblk) != 0)
        return -1;

    bc_flush();
    return 0;
}

int iso_v_lookup(struct iso_vol* v, const char* path, struct iso_node* out) {
    V = v; return iso_lookup_impl(path, out);
}
int iso_v_list(struct iso_vol* v, const char* path, char* buf, uint32_t cap, uint32_t* size) {
    V = v; return iso_list_impl(path, buf, cap, size);
}
int iso_v_dir_is_empty(struct iso_vol* v, const char* path) {
    V = v; return iso_dir_is_empty_impl(path);
}
int iso_v_read_range(struct iso_vol* v, const struct iso_node* n, uint32_t offset,
                     void* buf, uint32_t cap, uint32_t* got) {
    V = v; return iso_read_range_impl(n, offset, buf, cap, got);
}
const char* iso_v_name_style(const struct iso_vol* v) {
    return style_names[v->active ? v->style : 0];
}

/* ---- boot volume (what the overlay uses) ---- */

int iso_lookup(const char* path, struct iso_node* out) { return iso_v_lookup(&iso_primary, path, out); }
int iso_list(const char* path, char* buf, uint32_t cap, uint32_t* size) { return iso_v_list(&iso_primary, path, buf, cap, size); }
int iso_dir_is_empty(const char* path) { return iso_v_dir_is_empty(&iso_primary, path); }
int iso_read_range(const struct iso_node* n, uint32_t offset, void* buf, uint32_t cap, uint32_t* got) {
    return iso_v_read_range(&iso_primary, n, offset, buf, cap, got);
}
int iso_available(void) { return iso_primary.active; }
const char* iso_device_name(void) { return iso_primary.active && iso_primary.dev ? iso_primary.dev->name : ""; }
const char* iso_volume_label(void) { return iso_primary.active ? iso_primary.label : ""; }
const char* iso_name_style(void) { return style_names[iso_primary.active ? iso_primary.style : 0]; }
