/* ============================================================
 * TANJA OS - LINUX BINARY COMPATIBILITY LAYER
 * ------------------------------------------------------------
 * Runs real, upstream-built Linux i386 static ELF executables
 * (ET_EXEC) - e.g. the busybox applet binaries from
 * busybox.net/downloads/binaries - on top of TanjaOS.
 *
 *   - exec_file() spots an ELF whose e_type is EXEC and routes
 *     it here instead of the ET_REL program loader.
 *   - linux_run() copies PT_LOAD segments to their virtual
 *     addresses (typically 0x08048000), builds a Linux-style
 *     initial stack (argc/argv/envp/auxv), switches to a private
 *     stack and jumps to e_entry.
 *   - The program runs in ring 0 like every TanjaOS program.
 *     Its "kernel" is an int 0x80 gate (arch/x86/linux_asm.asm)
 *     that calls the syscall table below.
 *   - Syscalls are emulated on TanjaOS's console and whole-file
 *     filesystem API: files load whole into an fd slot on open()
 *     and are written back on close().
 *
 * Limitations (by design, for now): no fork/clone/execve, no
 * signals, no pipes, anonymous mmap only, ioctl = -ENOTTY.
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>
#include "../include/fs.h"

extern void print(const char* s);
extern void print_dec(uint32_t n);
extern void putc(char c);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void* memset(void* dst, int c, size_t n);
extern uint32_t get_uptime_ms(void);

/* asm helpers (arch/x86/linux_asm.asm) */
extern void linux_enter(uint32_t entry, uint32_t sp);
extern void linux_exit_restore(void);          /* noreturn */

/* register frame pushed by the int 0x80 stub (pusha order) */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
} linux_regs_t;

/* ---- ELF32 program header ------------------------------------ */
typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define PT_LOAD 1

/* ---- Linux ABI constants -------------------------------------- */
#define ENOSYS_L   38
#define ENOENT_L    2
#define EBADF_L     9
#define ENOTDIR_L  20
#define ENOTTY_L   25
#define EINVAL_L   22
#define EISDIR_L   21
#define ENODEV_L   19
#define ENOMEM_L   12

#define O_WRONLY_L     0x0001
#define O_RDWR_L       0x0002
#define O_CREAT_L      0x0040
#define O_TRUNC_L      0x0200
#define O_APPEND_L     0x0400

#define S_IFREG_L  0100000
#define S_IFDIR_L   040000

#define DT_DIR_L     4
#define DT_REG_L     8

#define AT_NULL_L      0
#define AT_PAGESZ_L    6
#define AT_UID_L      11
#define AT_EUID_L     12
#define AT_GID_L      13
#define AT_EGID_L     14
#define AT_HWCAP_L    16
#define AT_CLKTCK_L   17
#define AT_SECURE_L   23
#define AT_RANDOM_L   25

#define LINUX_STACK_TOP   0x09040000u
#define LINUX_MMAP_BASE   0x50000000u
#define LINUX_MMAP_LIMIT  0x51000000u

static uint32_t linux_brk_start;
static uint32_t linux_brk_cur;
static uint32_t linux_mmap_next = LINUX_MMAP_BASE;
static int      linux_exit_code;

/* ---- fd emulation --------------------------------------------- */
#define LINUX_MAX_FD   6

typedef struct {
    int      used;
    int      is_dir;
    int      writable;
    int      dirty;
    int      append;
    char     path[MAX_PATH];
    uint32_t pos;
    uint32_t len;
    uint32_t dir_pos;
    char     data[MAX_FILE_SIZE];
} linux_fd_t;

static linux_fd_t lfd[LINUX_MAX_FD];

static char* lxstrcpy(char* d, const char* s)
{
    while ((*d++ = *s++)) ;
    return d;
}

/* Normalize a Linux path for the TanjaOS fs: "." and "./" mean the
 * cwd, which the fs expects as an empty path; "./name" -> "name". */
static char lxnorm_buf[MAX_PATH];

static const char* lxnorm(const char* path)
{
    int i = 0;
    if (!path) return 0;
    if (!path[0]) return path;
    if (path[0] == '.') {
        if (path[1] == 0 || (path[1] == '/' && path[2] == 0)) {
            /* "." is the cwd; the fs wants an absolute path here */
            fs_get_current_path(lxnorm_buf);
            return lxnorm_buf;
        }
        if (path[1] == '/') path += 2;
    }
    while (path[i] && i < MAX_PATH - 1) { lxnorm_buf[i] = path[i]; i++; }
    lxnorm_buf[i] = 0;
    return lxnorm_buf;
}

static const char* lxbasename(const char* p)
{
    const char* s = p;
    while (*p) {
        if (*p == '/') s = p + 1;
        p++;
    }
    return s;
}

/* ---- GDT extension for set_thread_area (TLS) ------------------- */
static uint8_t gdt_new[16 * 8];              /* 16 entries */
static struct { uint16_t limit; uint32_t base; } __attribute__((packed)) gdt_ptr;
static int gdt_ready = 0;
static int tls_next = 0;

static void gdt_prepare(void)
{
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) cur;
    int bytes, i;

    if (gdt_ready) return;
    asm volatile ("sgdt %0" : "=m"(cur));
    bytes = (int)cur.limit + 1;
    if (bytes > (int)sizeof(gdt_new)) bytes = (int)sizeof(gdt_new);
    for (i = 0; i < bytes; i++)
        gdt_new[i] = ((uint8_t*)cur.base)[i];

    gdt_ptr.limit = sizeof(gdt_new) - 1;
    gdt_ptr.base  = (uint32_t)gdt_new;
    asm volatile ("lgdt %0" : : "m"(gdt_ptr));
    gdt_ready = 1;
}

static uint32_t sys_set_thread_area(uint32_t* udesc)
{
    uint32_t entry_number, base_addr;
    uint8_t* d;
    uint16_t sel;

    if (!udesc) return (uint32_t)-EINVAL_L;
    gdt_prepare();
    entry_number = udesc[0];
    base_addr    = udesc[1];

    if (entry_number == 0xFFFFFFFFu) {
        if (tls_next >= 8) return (uint32_t)-ENOMEM_L;
        entry_number = (uint32_t)(6 + tls_next);
        tls_next++;
        udesc[0] = entry_number;
    }
    if (entry_number >= 16) return (uint32_t)-EINVAL_L;

    d = gdt_new + entry_number * 8;
    d[0] = 0xFF; d[1] = 0xFF;                     /* limit low     */
    d[2] = (uint8_t)(base_addr & 0xFF);            /* base 0:15     */
    d[3] = (uint8_t)((base_addr >> 8) & 0xFF);
    d[4] = (uint8_t)((base_addr >> 16) & 0xFF);
    d[5] = 0xF2;                                   /* P|DPL3|S|data */
    d[6] = 0xCF;                                   /* G|D|limit hi  */
    d[7] = (uint8_t)((base_addr >> 24) & 0xFF);    /* base 24:31    */

    sel = (uint16_t)(entry_number * 8);
    asm volatile ("mov %0, %%gs" : : "r"(sel));
    return 0;
}

/* ---- fd helpers ------------------------------------------------ */
static linux_fd_t* fd_get(uint32_t fd)
{
    if (fd < 3 || fd >= LINUX_MAX_FD || !lfd[fd].used) return 0;
    return &lfd[fd];
}

static void fd_flush(linux_fd_t* f)
{
    if (f->dirty && f->writable)
        fs_write_file(f->path, f->data, f->len);
    f->dirty = 0;
}

static void fd_flush_all(void)
{
    int i;
    for (i = 0; i < LINUX_MAX_FD; i++)
        if (lfd[i].used) fd_flush(&lfd[i]);
}

/* ---- syscalls -------------------------------------------------- */

static uint32_t sys_open(const char* path, uint32_t flags)
{
    int i;
    uint32_t size = MAX_FILE_SIZE;
    linux_fd_t* f = 0;

    if (!path || !path[0]) return (uint32_t)-ENOENT_L;
    path = lxnorm(path);
    if (!path || !path[0]) return (uint32_t)-ENOENT_L;

    for (i = 3; i < LINUX_MAX_FD; i++)
        if (!lfd[i].used) { f = &lfd[i]; break; }
    if (!f) return (uint32_t)-ENOMEM_L;

    memset(f, 0, sizeof(*f));
    f->used = 1;
    if (flags & (O_WRONLY_L | O_RDWR_L)) f->writable = 1;

    if (fs_directory_exists(path)) {
        f->is_dir = 1;
        if (fs_list_directory(path, f->data, &size) != 0)
            size = 0;

        f->len = size;
        for (i = 0; i < MAX_PATH - 1 && path[i]; i++)
            f->path[i] = path[i];
        f->path[i] = 0;
        return (uint32_t)(f - lfd);
    }

    if (!fs_file_exists(path)) {
        if (!(flags & O_CREAT_L)) {
            f->used = 0;
            return (uint32_t)-ENOENT_L;
        }
        fs_create_file(path);
        for (i = 0; i < MAX_PATH - 1 && path[i]; i++)
            f->path[i] = path[i];
        f->path[i] = 0;
        f->len = 0;
        f->dirty = 1;
        return (uint32_t)(f - lfd);
    }

    if (fs_read_file(path, f->data, &size) != 0)
        size = 0;
    f->len = size;
    for (i = 0; i < MAX_PATH - 1 && path[i]; i++)
        f->path[i] = path[i];
    f->path[i] = 0;

    if ((flags & O_TRUNC_L) && f->writable) { f->len = 0; f->dirty = 1; }
    if (flags & O_APPEND_L) { f->append = 1; f->pos = f->len; }

    return (uint32_t)(f - lfd);
}

static uint32_t sys_close(uint32_t fd)
{
    linux_fd_t* f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    fd_flush(f);
    f->used = 0;
    return 0;
}

static uint32_t sys_read(uint32_t fd, uint8_t* buf, uint32_t count)
{
    linux_fd_t* f;
    uint32_t avail, n;

    if (fd == 0) return 0;                    /* stdin: EOF */
    f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    if (f->is_dir) return (uint32_t)-EISDIR_L;
    if (f->pos >= f->len) return 0;           /* EOF */

    avail = f->len - f->pos;
    n = count < avail ? count : avail;
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    return n;
}

static uint32_t sys_write(uint32_t fd, const uint8_t* buf, uint32_t count)
{
    linux_fd_t* f;
    uint32_t room, n;

    if (fd == 1 || fd == 2) {                 /* console */
        for (n = 0; n < count; n++)
            putc((char)buf[n]);
        return count;
    }
    f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    if (!f->writable) return (uint32_t)-EBADF_L;
    if (f->is_dir) return (uint32_t)-EISDIR_L;

    if (f->append) f->pos = f->len;
    if (f->pos > f->len) {                    /* sparse gap: zero-fill */
        uint32_t gap = f->pos - f->len;
        if (f->len + gap > MAX_FILE_SIZE) return (uint32_t)-ENOMEM_L;
        memset(f->data + f->len, 0, gap);
        f->len = f->pos;
    }

    room = MAX_FILE_SIZE - f->pos;
    n = count < room ? count : room;
    memcpy(f->data + f->pos, buf, n);
    f->pos += n;
    if (f->pos > f->len) f->len = f->pos;
    f->dirty = 1;
    return n;
}

/* shared seek helper: returns new offset or -EINVAL_L */
static uint32_t seek_new_pos(linux_fd_t* f, int32_t off, uint32_t whence)
{
    int32_t base, np;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = (int32_t)f->pos; break;
    case 2: base = (int32_t)f->len; break;
    default: return (uint32_t)-EINVAL_L;
    }
    np = base + off;
    if (np < 0) return (uint32_t)-EINVAL_L;
    return (uint32_t)np;
}

static uint32_t sys_lseek(uint32_t fd, int32_t off, uint32_t whence)
{
    linux_fd_t* f = fd_get(fd);
    uint32_t np;
    if (!f) return (uint32_t)-EBADF_L;
    np = seek_new_pos(f, off, whence);
    if (np == (uint32_t)-EINVAL_L) return np;
    f->pos = np;
    if (f->is_dir) f->dir_pos = np;           /* lseek(0) rewinds listings */
    return np;
}

static uint32_t sys_llseek(uint32_t fd, uint32_t off_hi, uint32_t off_lo,
                           uint32_t* result, uint32_t whence)
{
    linux_fd_t* f = fd_get(fd);
    uint32_t np;
    if (!f) return (uint32_t)-EBADF_L;
    if (!result) return (uint32_t)-EINVAL_L;
    if (off_hi) return (uint32_t)-EINVAL_L;
    np = seek_new_pos(f, (int32_t)off_lo, whence);
    if (np == (uint32_t)-EINVAL_L) return np;
    f->pos = np;
    *result = np;
    return 0;
}

static uint32_t sys_getdents64(uint32_t fd, uint8_t* buf, uint32_t bufsize)
{
    linux_fd_t* f = fd_get(fd);
    uint32_t filled = 0;

    if (!f) return (uint32_t)-EBADF_L;
    if (!f->is_dir) return (uint32_t)-ENOTDIR_L;
    if (!buf || bufsize < 32) return (uint32_t)-EINVAL_L;

    /* The listing in f->data is "name[/]\n" per entry; emit
     * linux_dirent64 records until the buffer fills or the
     * listing is exhausted. */
    while (f->dir_pos < f->len) {
        char name[MAX_FILENAME + 2];
        int n = 0, is_dir = 0, reclen, need;
        uint8_t* rec;

        while (f->dir_pos < f->len && f->data[f->dir_pos] != '\n' &&
               n < MAX_FILENAME) {
            name[n++] = f->data[f->dir_pos++];
        }
        if (f->dir_pos < f->len) f->dir_pos++;      /* skip \n */
        if (n == 0) continue;
        name[n] = 0;
        if (name[n - 1] == '/') { is_dir = 1; name[n - 1] = 0; n--; }

        print("[dbg] dirent: <"); print(name); print(">\n");
        reclen = 19 + n + 1;
        reclen = (reclen + 7) & ~7;
        need = filled + reclen;
        if (need > (int)bufsize) break;

        rec = buf + filled;
        memset(rec, 0, (uint32_t)reclen);
        *(uint64_t*)(rec + 0)  = (uint64_t)(need);  /* d_ino  */
        *(uint64_t*)(rec + 8)  = (uint64_t)(need);  /* d_off  */
        *(uint16_t*)(rec + 16) = (uint16_t)reclen;  /* d_reclen */
        rec[18] = (uint8_t)(is_dir ? DT_DIR_L : DT_REG_L);
        lxstrcpy((char*)(rec + 19), name);

        filled = (uint32_t)need;
    }
    return filled;
}

/* Fill an i386 kernel-layout struct stat64. */
static void fill_stat64(uint8_t* s, int is_dir, uint32_t size)
{
    /* Only 96 bytes: never write past the caller's struct - the
     * extra st_ino field of the 104-byte kernel layout overlaps
     * the caller's stack and corrupts its locals. */
    memset(s, 0, 96);
    *(uint32_t*)(s + 12) = 1;                                   /* __st_ino   */
    *(uint32_t*)(s + 16) = (is_dir ? S_IFDIR_L : S_IFREG_L) |
                                  (is_dir ? 0755u : 0644u);     /* st_mode    */
    *(uint32_t*)(s + 20) = 1;                                   /* st_nlink   */
    *(uint32_t*)(s + 44) = size;                                /* st_size lo */
    *(uint32_t*)(s + 52) = 512;                                 /* st_blksize */
    *(uint64_t*)(s + 56) = (size + 511) / 512;                  /* st_blocks  */
    *(uint32_t*)(s + 80) = get_uptime_ms() / 1000;               /* st_mtime   */
}

static uint32_t sys_stat64(const char* path, uint8_t* s)
{
    if (!path || !s) return (uint32_t)-EINVAL_L;
    path = lxnorm(path);
    if (!path) path = "";
    if (fs_directory_exists(path)) { fill_stat64(s, 1, 0); return 0; }
    if (fs_file_exists(path)) {
        fill_stat64(s, 0, fs_get_file_size(path));
        return 0;
    }
    return (uint32_t)-ENOENT_L;
}

static uint32_t sys_fstat64(uint32_t fd, uint8_t* s)
{
    linux_fd_t* f;
    if (!s) return (uint32_t)-EINVAL_L;
    if (fd <= 2) { fill_stat64(s, 0, 0); return 0; }      /* console */
    f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    fill_stat64(s, f->is_dir, f->len);
    return 0;
}

static uint32_t sys_mmap(uint32_t addr, uint32_t len, uint32_t flags)
{
    uint32_t a;

    if (!len) return (uint32_t)-EINVAL_L;
    len = (len + 4095) & ~4095u;

    if (flags & 0x10) {                       /* MAP_FIXED */
        if (addr < LINUX_MMAP_BASE || addr + len > LINUX_MMAP_LIMIT)
            return (uint32_t)-ENOMEM_L;
        a = addr;
    } else {
        if (addr) return (uint32_t)-ENODEV_L; /* only anonymous supported */
        a = linux_mmap_next;
        if (a + len > LINUX_MMAP_LIMIT) return (uint32_t)-ENOMEM_L;
        linux_mmap_next += len;
    }
    memset((void*)a, 0, len);
    return a;
}

static uint32_t sys_brk(uint32_t addr)
{
    if (addr >= linux_brk_start && addr < linux_brk_start + 0x4000000u)
        linux_brk_cur = addr;
    return linux_brk_cur;
}

static uint32_t sys_uname(uint8_t* buf)
{
    static const char* fields[6] =
        { "Linux", "tanja", "2.6.39", "#1 SMP TanjaOS", "i686", "(none)" };
    int i;
    if (!buf) return (uint32_t)-EINVAL_L;
    for (i = 0; i < 6; i++) {
        memset(buf + i * 65, 0, 65);
        lxstrcpy((char*)(buf + i * 65), fields[i]);
    }
    return 0;
}

static uint32_t sys_getcwd(char* buf, uint32_t size)
{
    char path[MAX_PATH];
    int n = 0;
    if (!buf || size < 2) return (uint32_t)-EINVAL_L;
    fs_get_current_path(path);
    while (path[n] && n < (int)size - 1) { buf[n] = path[n]; n++; }
    if (path[n]) return (uint32_t)-ENOMEM_L;
    buf[n] = 0;
    return (uint32_t)n;
}

static uint32_t sys_writev(uint32_t fd, uint32_t iov, uint32_t iovcnt)
{
    uint32_t total = 0;
    uint32_t i;
    if (!iov) return (uint32_t)-EINVAL_L;
    for (i = 0; i < iovcnt; i++) {
        uint32_t base  = *(uint32_t*)(iov + i * 8);
        uint32_t len   = *(uint32_t*)(iov + i * 8 + 4);
        uint32_t w = sys_write(fd, (const uint8_t*)base, len);
        if (w == (uint32_t)-EBADF_L) return w;
        total += w;
        if (w < len) break;
    }
    return total;
}

/* ---- the dispatch table ---------------------------------------- */

void linux_syscall(linux_regs_t* r)
{
    uint32_t nr = r->eax;
    uint32_t a1 = r->ebx, a2 = r->ecx, a3 = r->edx,
             a4 = r->esi, a5 = r->edi;

    print("[linux] sys ");
    print_dec(nr);
    print("(");
    print_dec(a1);
    print(",");
    print_dec(a2);
    print(",");
    print_dec(a3);
    print(")");
    switch (nr) {

    case 1: case 252:                           /* exit / exit_group */
        linux_exit_code = (int)a1;
        fd_flush_all();
        linux_exit_restore();                   /* noreturn */

    case 3:  r->eax = sys_read(a1, (uint8_t*)a2, a3); break;
    case 4:  r->eax = sys_write(a1, (const uint8_t*)a2, a3); break;
    case 5:  r->eax = sys_open((const char*)a1, a2); break;
    case 6:  r->eax = sys_close(a1); break;
    case 12: r->eax = fs_change_directory(lxnorm((const char*)a1)) == 0
                       ? 0 : (uint32_t)-ENOENT_L; break;          /* chdir */
    case 13: r->eax = get_uptime_ms() / 1000; break;              /* time */
    case 19: r->eax = sys_lseek(a1, (int32_t)a2, a3); break;
    case 20: r->eax = 1; break;                                  /* getpid */
    case 24: case 199: r->eax = 0; break;                        /* getuid */
    case 37: r->eax = 0; break;                                  /* kill */
    case 39: r->eax = fs_create_directory((const char*)a1) == 0
                       ? 0 : (uint32_t)-1; break;                 /* mkdir */
    case 45: r->eax = sys_brk(a1); break;
    case 46: case 200: r->eax = 0; break;                        /* getgid */
    case 47: case 201: r->eax = 0; break;                        /* geteuid */
    case 49: case 202: r->eax = 0; break;                        /* getegid */
    case 54: r->eax = (uint32_t)-ENOTTY_L; break;                /* ioctl */
    case 85: r->eax = (uint32_t)-ENOENT_L; break;               /* readlink */
    case 90: r->eax = sys_mmap(a1, a2, a4); break;               /* mmap */
    case 91: r->eax = 0; break;                                 /* munmap */
    case 108: r->eax = sys_fstat64(a1, (uint8_t*)a2);
              print(" -> fstat ret="); print_dec(r->eax); print("\n"); break;
    case 119: case 173: r->eax = 0; break;                /* sigreturn(s) */
    case 122: r->eax = sys_uname((uint8_t*)a1); break;
    case 140: r->eax = sys_llseek(a1, a2, a3, (uint32_t*)a4, a5); break;
    case 146: r->eax = sys_writev(a1, a2, a3); break;
    case 163: r->eax = sys_mmap(0, a1, 0x02); break;            /* mremap */
    case 174: case 175: r->eax = 0; break;          /* rt_sigaction/mask */
    case 183: r->eax = sys_getcwd((char*)a1, a2); break;
    case 192: r->eax = sys_mmap(a1, a2, a4); break;              /* mmap2 */
    case 195: case 196:                                          /* (l)stat64 */
        r->eax = sys_stat64((const char*)a1, (uint8_t*)a2); break;
    case 197: r->eax = sys_fstat64(a1, (uint8_t*)a2); break;    /* fstat64 */
    case 220: r->eax = sys_getdents64(a1, (uint8_t*)a2, a3); break;
    case 221: r->eax = 0; break;                               /* fcntl64 */
    case 224: r->eax = 1; break;                               /* gettid */
    case 243: r->eax = sys_set_thread_area((uint32_t*)a1); break;
    case 251: r->eax = 1; break;                       /* set_tid_address */
    case 265: {                                       /* clock_gettime */
        uint32_t ms = get_uptime_ms();
        if (a2) {
            *(uint32_t*)a2 = ms / 1000;
            *((uint32_t*)a2 + 1) = (ms % 1000) * 1000000;
        }
        r->eax = 0;
        break;
    }
    default:
        print(" ???\n");
        print("linux: unimplemented syscall ");
        print_dec(nr);
        print("\n");
        r->eax = (uint32_t)-ENOSYS_L;
        break;
    }
}

/* ============================================================
 * linux_run - load a static ET_EXEC i386 image and run it
 * ============================================================ */

int linux_run(const uint8_t* image, uint32_t size, const char* path,
              const char* args)
{
    const uint8_t* e = image;
    uint32_t entry, phoff, phentsize, phnum, max_end = 0;
    int i;

    print("[linux] linux_run: path=");
    print(path);
    print(" size=");
    print_dec(size);
    print("\n");
    if (size < 52) return -1;
    entry     = *(uint32_t*)(e + 24);
    phoff     = *(uint32_t*)(e + 28);
    phentsize = *(uint16_t*)(e + 42);
    phnum     = *(uint16_t*)(e + 44);
    if (phentsize < sizeof(Elf32_Phdr) || !phnum) return -1;
    if (phoff + phnum * phentsize > size) return -1;

    /* copy PT_LOAD segments to their virtual addresses */
    for (i = 0; i < (int)phnum; i++) {
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(e + phoff + i * phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) return -1;
        if (ph->p_offset + ph->p_filesz > size) return -1;
        memcpy((void*)ph->p_vaddr, e + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void*)(ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
        if (ph->p_vaddr + ph->p_memsz > max_end)
            max_end = ph->p_vaddr + ph->p_memsz;
    }
    if (!max_end) return -1;

    linux_brk_start = (max_end + 4095) & ~4095u;
    linux_brk_cur  = linux_brk_start;
    linux_mmap_next = LINUX_MMAP_BASE;
    linux_exit_code = 0;
    for (i = 0; i < LINUX_MAX_FD; i++) lfd[i].used = 0;
    lfd[0].used = lfd[1].used = lfd[2].used = 1;   /* std fds */

    /* ---- build the initial stack (Linux process ABI) -------- */
    {
        char* sp_top = (char*)LINUX_STACK_TOP;
        char* sp;
        uint32_t argv_ptrs[32];
        uint32_t randp;
        uint32_t words[80];
        int argc = 0, w = 0, k;
        static const uint8_t randbytes[16] =
            { 0x5A,0x11,0xC3,0x7E,0x09,0x44,0xD1,0x8F,
              0x33,0xB6,0x72,0x0E,0x9C,0x18,0xF4,0x2D };

        /* argv[0] = basename of the program */
        argv_ptrs[argc++] = (uint32_t)(char*)sp_top;
        sp_top = lxstrcpy(sp_top, lxbasename(path));

        /* further argv entries split on spaces */
        if (args && args[0]) {
            static char argcopy[1024];
            int p = 0;
            while (args[p] && p < 1023) { argcopy[p] = args[p]; p++; }
            argcopy[p] = 0;
            p = 0;
            while (argcopy[p] && argc < 32) {
                while (argcopy[p] == ' ') p++;
                if (!argcopy[p]) break;
                argv_ptrs[argc++] = (uint32_t)(char*)sp_top;
                while (argcopy[p] && argcopy[p] != ' ')
                    *sp_top++ = argcopy[p++];
                *sp_top++ = 0;
            }
        }

        /* AT_RANDOM bytes, kept on the initial stack */
        randp = (uint32_t)(char*)sp_top;
        memcpy((void*)randp, randbytes, 16);
        sp_top = (char*)(((uint32_t)sp_top + 16 + 3) & ~3u);

        words[w++] = (uint32_t)argc;
        for (k = 0; k < argc; k++) words[w++] = argv_ptrs[k];
        words[w++] = 0;                                  /* argv NULL */
        words[w++] = 0;                                  /* envp NULL */
        words[w++] = AT_PAGESZ_L;  words[w++] = 4096;
        words[w++] = AT_CLKTCK_L;  words[w++] = 100;
        words[w++] = AT_HWCAP_L;   words[w++] = 0;
        words[w++] = AT_UID_L;     words[w++] = 0;
        words[w++] = AT_EUID_L;    words[w++] = 0;
        words[w++] = AT_GID_L;     words[w++] = 0;
        words[w++] = AT_EGID_L;    words[w++] = 0;
        words[w++] = AT_SECURE_L;  words[w++] = 0;
        words[w++] = AT_RANDOM_L;  words[w++] = randp;
        words[w++] = AT_NULL_L;    words[w++] = 0;

        sp = (char*)(((uint32_t)sp_top - (uint32_t)(w * 4)) & ~15u);
        memcpy(sp, words, (uint32_t)(w * 4));

        linux_enter(entry, (uint32_t)(char*)sp);
    }

    /* linux_enter "returns" here after exit(): the asm restored the
     * kernel stack, so we unwind exactly as if it never happened. */
    return linux_exit_code;
}
