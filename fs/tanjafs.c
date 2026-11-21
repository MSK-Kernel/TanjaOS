#include <stdint.h>
#include "../include/vfs.h"
#include "../include/iso9660.h"
#include "../include/store.h"
#include "../include/ramfs.h"

/*   mount /dev/ata1 /mnt        (type defaults to tanjafs for a device)
 *   mount -t iso9660 /dev/sr0 /mnt   (boot image only, ignore any Storefile)
 *
 * A TanjaOS disk has two parts (see kernel/store.c):
 *   - the boot image the installer wrote to the front of the disk: an
 *     ISO9660 image with GRUB, the kernel and the boot/ folder
 *   - the Storefile at the end of the disk: the saved TanjaOS filesystem
 *     (/etc, /usr, /home, ...), plus the data blocks of big files
 * The mount shows both at once, like the root of a booted live ISO does:
 * the boot image's files (boot/, boot.catalog, ...) next to the saved tree.
 *
 * The Storefile is a raw dump of the file tables of the build that wrote it,
 * and its position and layout depend on that build's constants. So the
 * mount does not assume this kernel's: it looks where this build would have
 * put the Storefile, scans the end of the disk for it if it is not there,
 * and parses it with the layout of this build or of a known earlier one
 * (see geoms[] below), checking that the sizes add up exactly. Several
 * volumes can be mounted at the same time.
 */

extern int fs_cur_uid;
extern int fs_errno;
extern void* malloc(unsigned int n);
extern void  free(void* p);
extern void  print(const char* s);

#define TFS_LIST_LIMIT 16000

struct tfs {
    struct vfs_bdev* dev;

    struct iso_vol iso;
    int      has_iso;

    int      store;              /* 0 none, 2 loaded */
    int      other_build;        /* loaded, but written by a different build */
    uint64_t img_byte;           /* disk byte offset of image byte 0 */
    uint64_t dsk_byte;           /* disk byte offset of file-data block 1 */
    uint32_t max_f, max_d, pcap;
    uint8_t* meta;               /* used flags, sizes, names, extents */
    uint32_t o_dused, o_dname, o_fused, o_fsize, o_fname, o_fstart;
    uint32_t* f_off;             /* image offset of each pool-resident file */
    uint32_t* f_owner;
    uint16_t* f_mode;
    uint32_t* d_owner;
    uint16_t* d_mode;
};

/* The mount currently being served. Every entry point sets it from its `sb`
 * argument (ENTER), so any number of volumes can be mounted at once. */
static struct tfs* TT;
#define T (*TT)
#define ENTER(sb) do { if (sb) TT = (struct tfs*)(sb); } while (0)

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static int seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* read `len` bytes at absolute disk byte offset `off` */
static int dread(uint64_t off, void* dst, uint32_t len) {
    static uint8_t bounce[4096];
    uint8_t* d = (uint8_t*)dst;
    while (len) {
        uint64_t sec = off / 512;
        uint32_t so = (uint32_t)(off % 512);
        uint32_t nsec, take, i;
        if (sec >= T.dev->sectors) return -1;
        if (so == 0 && len >= 512) {                 /* aligned: straight into dst */
            nsec = len / 512;
            if (nsec > 128) nsec = 128;
            if (sec + nsec > T.dev->sectors) nsec = (uint32_t)(T.dev->sectors - sec);
            if (T.dev->read(T.dev, sec, nsec, d) != 0) return -1;
            take = nsec * 512;
        } else {
            nsec = 8;
            if (sec + nsec > T.dev->sectors) nsec = (uint32_t)(T.dev->sectors - sec);
            if (T.dev->read(T.dev, sec, nsec, bounce) != 0) return -1;
            take = nsec * 512 - so;
            if (take > len) take = len;
            for (i = 0; i < take; i++) d[i] = bounce[so + i];
        }
        d += take;
        off += take;
        len -= take;
    }
    return 0;
}

/* Storefile layer                                                   */

static const char* s_dname(uint32_t i) { return (const char*)(T.meta + T.o_dname + i * T.pcap); }
static const char* s_fname(uint32_t i) { return (const char*)(T.meta + T.o_fname + i * T.pcap); }
static int s_dused(uint32_t i) { return T.meta[T.o_dused + i] != 0; }
static int s_fused(uint32_t i) { return T.meta[T.o_fused + i] != 0; }
static uint32_t s_fsize(uint32_t i) { return le32(T.meta + T.o_fsize + i * 4); }
static uint32_t s_fstart(uint32_t i) { return le32(T.meta + T.o_fstart + i * 8); }

static int s_find_file(const char* p) {
    uint32_t i;
    if (T.store != 2) return -1;
    for (i = 0; i < T.max_f; i++)
        if (s_fused(i) && seq(s_fname(i), p)) return (int)i;
    return -1;
}

static int s_find_dir(const char* p) {
    uint32_t i;
    if (T.store != 2) return -1;
    for (i = 0; i < T.max_d; i++)
        if (s_dused(i) && seq(s_dname(i), p)) return (int)i;
    return -1;
}

/* read from a Storefile file */
static int s_read(int idx, uint32_t off, void* dst, uint32_t len) {
    uint32_t sz = s_fsize((uint32_t)idx);
    uint32_t start = s_fstart((uint32_t)idx);
    if (off >= sz) return 0;
    if (len > sz - off) len = sz - off;
    if (start) {                                     /* data blocks on the disk */
        uint64_t base = T.dsk_byte + (uint64_t)(start - 1) * 4096u + off;
        if (dread(base, dst, len) != 0) return -1;
    } else {                                         /* inside the image */
        if (dread(T.img_byte + T.f_off[idx] + off, dst, len) != 0) return -1;
    }
    return (int)len;
}

/* permissions (read-only view: only read access is ever granted)    */

static int may(int mode, int owner, int bit) {
    if (fs_cur_uid == 0) return 1;
    if (fs_cur_uid == owner) return (mode & bit) != 0;
    return (mode & (bit >> 6)) != 0;
}

/* queries over both layers                                          */

static int t_file_exists(void* sb, const char* p) {
    struct iso_node n;
    ENTER(sb);
    if (s_find_file(p) >= 0) return 1;
    if (s_find_dir(p) >= 0) return 0;
    return T.has_iso && iso_v_lookup(&T.iso, p, &n) == 0 && !n.is_dir;
}

static int t_dir_exists(void* sb, const char* p) {
    struct iso_node n;
    ENTER(sb);
    if (p[0] == '/' && p[1] == 0) return 1;
    if (s_find_dir(p) >= 0) return 1;
    if (s_find_file(p) >= 0) return 0;
    return T.has_iso && iso_v_lookup(&T.iso, p, &n) == 0 && n.is_dir;
}

static uint32_t t_get_file_size(void* sb, const char* p) {
    struct iso_node n;
    int i;
    ENTER(sb);
    i = s_find_file(p);
    if (i >= 0) return s_fsize((uint32_t)i);
    if (T.has_iso && iso_v_lookup(&T.iso, p, &n) == 0 && !n.is_dir && !n.is_link) return n.size;
    return 0;
}

static int t_get_mode(void* sb, const char* p) {
    struct iso_node n;
    int i;
    ENTER(sb);
    i = s_find_file(p);
    if (i >= 0) return T.f_mode[i];
    i = s_find_dir(p);
    if (i >= 0) return T.d_mode[i];
    if (p[0] == '/' && p[1] == 0) return 0755;
    if (T.has_iso && iso_v_lookup(&T.iso, p, &n) == 0) return n.mode;
    return -1;
}

static int t_get_owner(void* sb, const char* p) {
    struct iso_node n;
    int i;
    ENTER(sb);
    i = s_find_file(p);
    if (i >= 0) return (int)T.f_owner[i];
    i = s_find_dir(p);
    if (i >= 0) return (int)T.d_owner[i];
    if (p[0] == '/' && p[1] == 0) return 0;
    if (T.has_iso && iso_v_lookup(&T.iso, p, &n) == 0) return 0;
    return -1;
}

static int t_read_file_range(void* sb, const char* p, uint32_t offset,
                             char* buf, uint32_t capacity, uint32_t* size) {
    struct iso_node n;
    int i;
    ENTER(sb);
    fs_errno = 0;
    if (!buf || !size || capacity == 0) return -1;
    *size = 0;
    i = s_find_file(p);
    if (i >= 0) {
        int got;
        if (!may(T.f_mode[i], (int)T.f_owner[i], 0400)) { fs_errno = 13; return -2; }
        got = s_read(i, offset, buf, capacity);
        if (got < 0) return -1;
        *size = (uint32_t)got;
        return 0;
    }
    if (T.has_iso && iso_v_lookup(&T.iso, p, &n) == 0 && !n.is_dir) {
        if (!may(n.mode, 0, 0400)) { fs_errno = 13; return -2; }
        return iso_v_read_range(&T.iso, &n, offset, buf, capacity, size) == 0 ? 0 : -1;
    }
    return -1;
}

static int t_read_file(void* sb, const char* p, char* buf, uint32_t* size) {
    uint32_t cap, got = 0;
    int rc;
    ENTER(sb);
    if (!buf || !size) return -1;
    cap = *size;                                  /* caller: max payload bytes */
    if (cap == 0) { buf[0] = 0; return t_file_exists(sb, p) ? 0 : -1; }
    rc = t_read_file_range(sb, p, 0, buf, cap, &got);
    if (rc != 0) { buf[0] = 0; *size = 0; return rc; }
    buf[got] = 0;
    *size = got;
    return 0;
}

/* ---- directory listing ---- */

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

/* if `full` is a direct child of directory `dir`, return its name part */
static const char* child_name(const char* full, const char* dir) {
    int dl = slen(dir), i;
    const char* r;
    if (dl == 1 && dir[0] == '/') dl = 0;            /* root: names start with "/x" */
    for (i = 0; i < dl; i++) if (full[i] != dir[i]) return 0;
    if (full[dl] != '/') return 0;
    r = full + dl + 1;
    if (!*r) return 0;
    for (i = 0; r[i]; i++) if (r[i] == '/') return 0;
    return r;
}

static int t_list_dir(void* sb, const char* p, char* out, uint32_t* size) {
    static char lb[16384];
    uint32_t ls = 0, pos = 0, i;
    int have_l = 0, pass;
    ENTER(sb);
    if (!p || !out || !size) return -1;
    *size = 0;
    out[0] = 0;
    if (!t_dir_exists(0, p)) return -1;

    if (T.has_iso) have_l = (iso_v_list(&T.iso, p, lb, sizeof(lb), &ls) == 0);

    for (pass = 0; pass < 2; pass++) {               /* 0 = directories, 1 = files */
        /* Storefile entries */
        for (i = 0; T.store == 2 && i < (pass == 0 ? T.max_d : T.max_f); i++) {
            const char* full;
            const char* nm;
            int nl;
            if (pass == 0 ? !s_dused(i) : !s_fused(i)) continue;
            full = pass == 0 ? s_dname(i) : s_fname(i);
            nm = child_name(full, p);
            if (!nm) continue;
            nl = slen(nm);
            if (name_in(out, pos, nm, nl)) continue;
            if (pos + (uint32_t)nl + 3 > TFS_LIST_LIMIT) goto done;
            if (pos) out[pos++] = '\n';
            { int k; for (k = 0; k < nl; k++) out[pos++] = nm[k]; }
            if (pass == 0) out[pos++] = '/';
            out[pos] = 0;
        }
        /* boot image entries */
        if (have_l) {
            uint32_t j = 0;
            while (j < ls) {
                uint32_t s = j, e;
                int is_dir, nl;
                while (j < ls && lb[j] != '\n') j++;
                e = j;
                if (j < ls) j++;
                if (e == s) continue;
                is_dir = (lb[e - 1] == '/');
                if (is_dir != (pass == 0)) continue;
                nl = (int)(e - s) - (is_dir ? 1 : 0);
                if (nl <= 0 || name_in(out, pos, lb + s, nl)) continue;
                if (pos + (uint32_t)(e - s) + 2 > TFS_LIST_LIMIT) goto done;
                if (pos) out[pos++] = '\n';
                { uint32_t k; for (k = s; k < e; k++) out[pos++] = lb[k]; }
                out[pos] = 0;
            }
        }
    }
done:
    *size = pos;
    return 0;
}

static int t_dir_is_empty(void* sb, const char* p) {
    static char tmp[16384];          /* t_list_dir may write up to TFS_LIST_LIMIT */
    uint32_t sz = 0;
    ENTER(sb);
    if (!t_dir_exists(sb, p)) return 0;
    if (t_list_dir(sb, p, tmp, &sz) != 0) return 0;
    return sz == 0;
}

/* ---- everything that would write ---- */

static int ro_denied(void) { fs_errno = 13; return -2; }
static int t_create_file(void* sb, const char* p) { (void)sb; (void)p; return ro_denied(); }
static int t_create_dir(void* sb, const char* p) { (void)sb; (void)p; return ro_denied(); }
static int t_delete_file(void* sb, const char* p) { (void)sb; (void)p; return ro_denied(); }
static int t_delete_dir(void* sb, const char* p) { (void)sb; (void)p; return ro_denied(); }
static int t_write_file(void* sb, const char* p, const char* d, uint32_t n) {
    (void)sb; (void)p; (void)d; (void)n; return ro_denied();
}
static int t_append_file(void* sb, const char* p, const char* d, uint32_t n) {
    (void)sb; (void)p; (void)d; (void)n; return ro_denied();
}
static int t_chmod(void* sb, const char* p, int m) { (void)sb; (void)p; (void)m; return ro_denied(); }
static int t_chown(void* sb, const char* p, int u) { (void)sb; (void)p; (void)u; return ro_denied(); }

/* mount / unmount                                                   */

static void drop_store(void) {
    if (T.meta) free(T.meta);
    if (T.f_off) free(T.f_off);
    if (T.f_owner) free(T.f_owner);
    if (T.f_mode) free(T.f_mode);
    if (T.d_owner) free(T.d_owner);
    if (T.d_mode) free(T.d_mode);
    T.meta = 0; T.f_off = 0; T.f_owner = 0; T.f_mode = 0; T.d_owner = 0; T.d_mode = 0;
    T.store = 0;
}

/* Layouts a Storefile image can have. Entry 0 is filled in at runtime with
 * this build's own constants; the others are earlier builds. The image does
 * not describe its own layout, so each candidate is tried and accepted only
 * if every size in the image adds up exactly to the stored length. */
struct geom { uint32_t f, d, p, front; };
static struct geom geoms[] = {
    { 0, 0, 0, 0 },          /* this build */
    { 96, 64, 256, 16384 },  /* original: 96 files, 64 dirs, 8 MiB boot reserve */
};
#define N_GEOMS (int)(sizeof(geoms) / sizeof(geoms[0]))

#define ALLOC_STATE_BYTES (4 + 4 + 8 * 8)     /* dsk_high, hole_n, 8 holes */

/* Parse the image at `lba` using layout `g`. 1 = accepted (state loaded). */
static int try_geometry(uint32_t lba, uint32_t payload, const struct geom* g) {
    uint32_t total, i, cursor, tail_bytes, end;
    uint8_t* tail;

    T.max_f = g->f; T.max_d = g->d; T.pcap = g->p;
    T.img_byte = (uint64_t)lba * 512u + 16u;
    T.dsk_byte = (uint64_t)g->front * 512u;

    T.o_dused = 8;
    T.o_dname = T.o_dused + T.max_d;
    T.o_fused = T.o_dname + T.max_d * T.pcap;
    T.o_fsize = T.o_fused + T.max_f;
    T.o_fname = T.o_fsize + T.max_f * 4;
    T.o_fstart = T.o_fname + T.max_f * T.pcap;
    total = T.o_fstart + T.max_f * 8;
    if (total > payload) return 0;

    T.meta = (uint8_t*)malloc(total);
    T.f_off = (uint32_t*)malloc(T.max_f * 4);
    T.f_owner = (uint32_t*)malloc(T.max_f * 4);
    T.f_mode = (uint16_t*)malloc(T.max_f * 2);
    T.d_owner = (uint32_t*)malloc(T.max_d * 4);
    T.d_mode = (uint16_t*)malloc(T.max_d * 2);
    if (!T.meta || !T.f_off || !T.f_owner || !T.f_mode || !T.d_owner || !T.d_mode) goto fail;

    if (dread(T.img_byte, T.meta, total) != 0 ||
        T.meta[0] != 'T' || T.meta[1] != 'J' || T.meta[2] != 'F' || T.meta[3] != 'S' ||
        T.meta[4] != 5)
        goto fail;

    /* every used name must be an absolute path; sizes must stay inside the image */
    for (i = 0; i < T.max_d; i++)
        if (s_dused(i) && s_dname(i)[0] != '/') goto fail;
    cursor = total;
    for (i = 0; i < T.max_f; i++) {
        T.f_off[i] = 0;
        if (!s_fused(i)) continue;
        if (s_fname(i)[0] != '/') goto fail;
        if (s_fsize(i) > payload) goto fail;
        if (s_fsize(i) > 0 && !s_fstart(i)) {
            T.f_off[i] = cursor;
            cursor += s_fsize(i);
            if (cursor > payload) goto fail;
        }
    }
    cursor += 256;                                   /* saved cwd */
    tail_bytes = T.max_d * 6 + T.max_f * 6;
    end = cursor + tail_bytes + ALLOC_STATE_BYTES;
    /* what is left after the file-system part is the login/hostname config,
       a few KB; anything else means this is not the right layout */
    if (end > payload || payload - end > 65536u) goto fail;

    tail = (uint8_t*)malloc(tail_bytes);
    if (!tail) goto fail;
    if (dread(T.img_byte + cursor, tail, tail_bytes) != 0) { free(tail); goto fail; }
    for (i = 0; i < T.max_d; i++) {
        T.d_owner[i] = le32(tail + i * 6);
        T.d_mode[i] = (uint16_t)(tail[i * 6 + 4] | (tail[i * 6 + 5] << 8));
    }
    for (i = 0; i < T.max_f; i++) {
        const uint8_t* q = tail + T.max_d * 6 + i * 6;
        T.f_owner[i] = le32(q);
        T.f_mode[i] = (uint16_t)(q[4] | (q[5] << 8));
    }
    free(tail);
    T.store = 2;
    return 1;

fail:
    drop_store();
    return 0;
}

#define SCAN_MAX_SECTORS 160000u     /* ~78 MiB at the end of the disk */
#define SCAN_MAX_CAND 6

/* Find the Storefile and load it. Leaves T.store = 2 on success. */
static void load_store(void) {
    static uint8_t sec[512];
    static uint8_t scan[128 * 512];
    uint32_t cand[SCAN_MAX_CAND];
    uint32_t payloads[SCAN_MAX_CAND];
    int nc = 0, i, g;
    uint32_t lba, payload, rsv, first;
    uint64_t disk = T.dev->sectors;

    T.store = 0;
    ramfs_image_geometry(&geoms[0].f, &geoms[0].d, &geoms[0].p, &payload);
    geoms[0].front = store_front_reserve_sectors();

    /* 1. where this build would have put it */
    lba = store_locate_lba(disk);
    if (lba && T.dev->read(T.dev, lba, 1, sec) == 0 && store_check_header(sec, &payload) != 0) {
        cand[nc] = lba; payloads[nc] = payload; nc++;
    }

    /* 2. otherwise look for a header near the end of the disk (an image
     * written by a build with a different layout lives at a different
     * place). Read 64 KiB at a time. */
    if (nc == 0) {
        uint32_t last = (uint32_t)(disk > 0xFFFFFFFFu ? 0xFFFFFFFFu : disk);
        rsv = 16384;                                   /* smallest boot reserve of any build */
        first = last > SCAN_MAX_SECTORS + rsv ? last - SCAN_MAX_SECTORS : rsv;
        for (lba = first; lba < last && nc < SCAN_MAX_CAND; lba += 128) {
            uint32_t n = last - lba > 128 ? 128 : last - lba;
            if (T.dev->read(T.dev, lba, n, scan) != 0) break;
            for (i = 0; i < (int)n && nc < SCAN_MAX_CAND; i++) {
                const uint8_t* q = scan + i * 512;
                uint32_t pl;
                if (q[0] != 'T' || q[1] != 'J' || q[2] != 'S' || q[3] != '1') continue;
                pl = le32(q + 4);
                if (pl < 16 || pl > (64u << 20)) continue;
                if (((uint64_t)(lba + i) * 512u) + 16u + pl > disk * 512u) continue;
                cand[nc] = lba + i; payloads[nc] = pl; nc++;
            }
        }
    }

    /* 3. try each candidate (newest position first) with each layout */
    for (i = nc - 1; i >= 0; i--) {
        uint8_t hdr[512];
        int st;
        if (T.dev->read(T.dev, cand[i], 1, hdr) != 0) continue;
        st = store_check_header(hdr, &payload);
        if (st == 0) continue;
        for (g = 0; g < N_GEOMS; g++) {
            if (try_geometry(cand[i], payloads[i], &geoms[g])) {
                T.other_build = (st == 1);
                return;
            }
        }
    }
}

static int tfs_mount_common(void** sbp, struct vfs_bdev* dev, int iso_only) {
    struct tfs* t;
    char* z;
    uint32_t k;

    if (!dev || !dev->present || !dev->read) return VFS_EINVAL;
    t = (struct tfs*)malloc(sizeof(struct tfs));
    if (!t) return VFS_ENOSPC;
    z = (char*)t;
    for (k = 0; k < sizeof(*t); k++) z[k] = 0;
    TT = t;
    T.dev = dev;
    T.has_iso = iso_vol_open(&T.iso, dev);
    if (!iso_only) load_store();

    if (!T.has_iso && T.store != 2) {
        drop_store();
        free(t);
        return VFS_ENOENT;
    }

    print("tanjafs: ");
    print(dev->name);
    print(": ");
    if (T.has_iso) { print("boot image ("); print(iso_v_name_style(&T.iso)); print(" names)"); }
    if (T.has_iso && T.store) print(" + ");
    if (T.store == 2) {
        print("saved TanjaOS files");
        if (T.other_build) print(" (written by a different TanjaOS build)");
    }
    print("\n");

    *sbp = t;
    return VFS_OK;
}

static int tfs_mount(void** sbp, struct vfs_bdev* dev, const char* opts) {
    (void)opts;
    return tfs_mount_common(sbp, dev, 0);
}

static int iso_mount_only(void** sbp, struct vfs_bdev* dev, const char* opts) {
    (void)opts;
    return tfs_mount_common(sbp, dev, 1);
}

static void tfs_unmount(void* sb) {
    if (!sb) return;
    TT = (struct tfs*)sb;
    drop_store();
    free(sb);
    TT = 0;
}

static struct vfs_ops tfs_ops;
static struct vfs_ops iso_ops;

static void fill_ops(struct vfs_ops* o, const char* name,
                     int (*mnt)(void**, struct vfs_bdev*, const char*)) {
    char* z = (char*)o;
    uint32_t k;
    for (k = 0; k < sizeof(*o); k++) z[k] = 0;
    o->name = name;
    o->create_file = t_create_file;
    o->create_dir = t_create_dir;
    o->delete_file = t_delete_file;
    o->delete_dir = t_delete_dir;
    o->write_file = t_write_file;
    o->read_file = t_read_file;
    o->read_file_range = t_read_file_range;
    o->append_file = t_append_file;
    o->get_file_size = t_get_file_size;
    o->file_exists = t_file_exists;
    o->dir_exists = t_dir_exists;
    o->dir_is_empty = t_dir_is_empty;
    o->list_dir = t_list_dir;
    o->chmod = t_chmod;
    o->chown = t_chown;
    o->get_owner = t_get_owner;
    o->get_mode = t_get_mode;
    o->mount = mnt;
    o->unmount = tfs_unmount;
}

const struct vfs_ops* tanjafs_get_ops(void) {
    fill_ops(&tfs_ops, "tanjafs", tfs_mount);
    return &tfs_ops;
}

const struct vfs_ops* iso9660_get_ops(void) {
    fill_ops(&iso_ops, "iso9660", iso_mount_only);
    return &iso_ops;
}
