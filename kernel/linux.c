#include <stdint.h>
#include <stddef.h>
#include "../include/fs.h"

extern void print(const char* s);
extern void print_dec(uint32_t n);
extern void console_putc(char c);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void* memset(void* dst, int c, size_t n);
extern uint32_t get_uptime_ms(void);
extern const char* get_env(const char* name);
extern void read_line(char* buffer, int max_len);
extern int read_line_eof;
extern int kb_raw_mode;
extern int tanja_exit_jmp[5];
extern int tanja_exit_active;

/* asm helpers (arch/x86/linux_asm.asm) */
extern void linux_enter(uint32_t entry, uint32_t sp);
extern void linux_exit_restore(void);          /* noreturn */

/* register frame pushed by the int 0x80 stub (pusha order) */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
} linux_regs_t;

/* ELF32 program header */
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

/* Linux ABI constants */
#define ENOSYS_L   38
#define ENOENT_L    2
#define EBADF_L     9
#define ENOTDIR_L  20
#define ENOTTY_L   25
#define EINVAL_L   22
#define EISDIR_L   21
#define ENODEV_L   19
#define ENOMEM_L   12
#define EEXIST_L  17
#define EACCES_L  13
#define EMFILE_L  24

#define O_WRONLY_L     0x0001
#define O_RDWR_L       0x0002
#define O_CREAT_L      0x0040
#define O_TRUNC_L      0x0200
#define O_APPEND_L     0x0400
#define O_EXCL_L       0x0080
#define AT_FDCWD_L     0xFFFFFF9Cu
#define AT_EMPTY_L     0x1000

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
#define AT_EXECFN_L   31
#define AT_PHDR_L     3
#define AT_PHENT_L    4
#define AT_PHNUM_L    5
#define AT_BASE_L     7
#define AT_FLAGS_L    8
#define AT_ENTRY_L    9

/* runtime memory layout
 * The Linux ABI regions used to sit at fixed high addresses
 * (ld.so at 0x54000000 = 1.31 GiB, mmap at 0x50000000), which forced
 * every machine to have >= 1.5 GiB of RAM just to run a dynamic
 * binary. They are now placed just above the kernel image and sized
 * relative to the ACTUAL top of RAM, so a 64 MiB machine runs the
 * same dynamic binaries a 1.5 GiB machine does. linux_set_layout()
 * is called once at boot (kernel.c) with the kernel image end and the
 * RAM top from the multiboot info. */
static uint32_t lx_ld_base;       /* where PT_INTERP (ld-linux) is placed */
static uint32_t lx_mmap_base;     /* Linux mmap region [base, limit)      */
static uint32_t lx_mmap_limit;
static uint32_t lx_exe_base;      /* load base for ET_DYN (PIE) images    */
static uint32_t lx_stack_top;     /* initial program stack top            */
static uint32_t lx_brk_limit;     /* sbrk ceiling (below the stack)       */

/* The native applet/program region SHARES the linux mmap window:
 * a loaded applet and a running Linux binary never coexist (Linux
 * programs cannot execve() an applet, and every shell script runs
 * its lines strictly one at a time), so applets can load into the
 * mmap area and still leave the exe/brk/stack space untouched. */
uint32_t linux_applet_region_base(void) { return lx_mmap_base; }
uint32_t linux_applet_region_limit(void) { return lx_mmap_limit; }

void linux_set_layout(uint32_t kernel_end, uint32_t ram_top)
{
    /* 2 MiB of headroom above the kernel image for the native
 * applet/program region, then (in order): ld.so, the mmap
 * region (libs + arena), then the PIE exe area, then free
 * brk space up to the stack, which sits 512 KiB below top of
 * RAM. */
    uint32_t base = (kernel_end + (2u << 20) + 4095u) & ~4095u;
    uint32_t avail, win;
    lx_ld_base    = base;
    lx_mmap_base  = base + (1u << 20);
    if (ram_top > (512u << 10)) {
        lx_stack_top = (ram_top - (512u << 10)) & ~4095u;
        lx_brk_limit = lx_stack_top - (1u << 20);
    } else {
        lx_stack_top = 0;
        lx_brk_limit = 0;
        return;
    }

    /* The mmap/applet window is shared by the Linux libs, applets and
 * the exe. It wants 16 MiB (doom's BSS alone is ~12 MiB), but on
 * a 64 MiB machine there is only ~9 MiB to give after the kernel
 * image - so size it to what actually fits, keeping at least a
 * small exe/brk reserve above it. */
    avail = (lx_stack_top > lx_mmap_base) ? (lx_stack_top - lx_mmap_base) : 0;
    win = (avail > (4u << 20)) ? avail - (4u << 20) : 0;
    if (win > (16u << 20)) win = (16u << 20);
    if (win < (9u << 20)) win = (9u << 20);
    lx_mmap_limit = lx_mmap_base + win;
    lx_exe_base   = lx_mmap_limit;
}

static uint32_t linux_brk_start;
static uint32_t linux_brk_cur;
static uint32_t linux_mmap_next;        /* set per-run from lx_mmap_base */
static int      linux_exit_code;

/* optional debug trace (shell: `set LINUX_TRACE 1`) */
static int      lx_trace;
static char     linux_prog_path[MAX_PATH];

/* line-buffered stdin: one read_line() per refill */
static char     lx_stdin_line[256];
static uint32_t lx_stdin_pos, lx_stdin_len;

/* unwind diagnostics: set by whoever longjmps into linux_run */
int lx_unwind_reason;    /* 1=kernel fault handler, 2=Ctrl+C, 3=kernel libc exit, 4=? */
uint32_t lx_fault_vec, lx_fault_eip, lx_fault_cr2;
uint32_t lx_fault_regs[8];   /* eax ecx edx ebx esp ebp esi edi at fault */

/* ring log of the last syscalls: dumped when a program dies from a
 * fault, so the failing operation is identifiable */
#define LX_LOG_N 40
static uint32_t lx_log_nr[LX_LOG_N];
static uint32_t lx_log_a1[LX_LOG_N], lx_log_a2[LX_LOG_N], lx_log_a3[LX_LOG_N];
static uint32_t lx_log_res[LX_LOG_N];
static uint32_t lx_log_i;

void lx_log_dump(void);     /* full dump: used by the program-unwind path */

/* kernel-fault variant: only the last 12 syscalls, so the dump fits
 * on one VGA screen above the register/GOT data */
void lx_log_tail(void)
{
    uint32_t k, first = (lx_log_i > 12) ? (lx_log_i - 12) : 0;
    print("linux: last syscalls before the fault:\n");
    for (k = first; k < lx_log_i; k++) {
        uint32_t s = k % LX_LOG_N;
        print("  sys ");
        print_dec(lx_log_nr[s]);
        print("(");
        print_dec(lx_log_a1[s]);
        print(",");
        print_dec(lx_log_a2[s]);
        print(",");
        print_dec(lx_log_a3[s]);
        print(") = ");
        print_dec(lx_log_res[s]);
        print("\n");
    }
}

void lx_log_dump(void)
{
    uint32_t k, total = lx_log_i, first;
    if (total > LX_LOG_N) first = total - LX_LOG_N; else first = 0;
    print("linux: syscalls (");
    print_dec(total - first);
    print(" of ");
    print_dec(total);
    print("):\n");
    for (k = first; k < total; k++) {
        uint32_t s = k % LX_LOG_N;
        if (((k - first) & 1) == 0) print("  "); else print(" | ");
        print_dec(lx_log_nr[s]);
        print("(");
        print_dec(lx_log_a1[s]);
        print(",");
        print_dec(lx_log_a2[s]);
        print(",");
        print_dec(lx_log_a3[s]);
        print(")=");
        print_dec(lx_log_res[s]);
        if (((k - first) & 1) == 1) print("\n");
    }
    if (((total - first) & 1) == 1) print("\n");
}

/* fd emulation */
#define LINUX_MAX_FD   6

/* Writable fds stage their bytes in a small shared scratch instead of
 * an inline per-fd copy (a full MAX_FILE_SIZE buffer per fd cost 24 MiB
 * of BSS). Two concurrent writable files, 2 MiB each. Read-only fds
 * do not copy at all: data points straight into the fs file pool. */
#define LX_WRITE_SLOTS      2
#define LX_WRITE_SLOT_BYTES (2u * 1024u * 1024)
#define LX_DIR_LIST_BYTES   (64u * 1024)
#define ENOSPC_L   28

typedef struct {
    int      used;
    int      is_dir;
    int      writable;
    int      dirty;
    int      append;
    int      zero_copy;     /* data points into the fs pool (ro fd) */
    char     path[MAX_PATH];
    uint32_t pos;
    uint32_t len;
    uint32_t dir_pos;
    char*    data;           /* pool ptr (ro) or scratch slot (rw)  */
    uint32_t cap;            /* scratch capacity for writable fds    */
    int      slot;           /* scratch slot index, -1 = none       */
} linux_fd_t;

static linux_fd_t lfd[LINUX_MAX_FD];
static uint8_t lx_write_scratch[LX_WRITE_SLOTS][LX_WRITE_SLOT_BYTES];
static uint8_t lx_write_slot_used[LX_WRITE_SLOTS];
static char    lx_dir_list[LX_DIR_LIST_BYTES];

static int slot_claim(void)
{
    int i;
    for (i = 0; i < LX_WRITE_SLOTS; i++)
        if (!lx_write_slot_used[i]) {
            lx_write_slot_used[i] = 1;
            return i;
        }
    return -1;
}

static void slot_release(int s)
{
    if (s >= 0 && s < LX_WRITE_SLOTS) lx_write_slot_used[s] = 0;
}

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

/* GDT extension for set_thread_area (TLS) */
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
        /* single tasking: every run may reuse entry 6; the
 * descriptor only lives while the program runs, so
 * slots are never exhausted */
        entry_number = 6;
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

/* fd helpers */
static linux_fd_t* fd_get(uint32_t fd)
{
    if (fd < 3 || fd >= LINUX_MAX_FD || !lfd[fd].used) return 0;
    return &lfd[fd];
}

static void fd_flush(linux_fd_t* f)
{
    if (f->dirty && f->writable && !f->zero_copy)
        fs_write_file(f->path, f->data, f->len);
    f->dirty = 0;
}

static void fd_flush_all(void)
{
    int i;
    for (i = 0; i < LINUX_MAX_FD; i++)
        if (lfd[i].used) fd_flush(&lfd[i]);
}

/* syscalls */

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
    f->slot = -1;
    if (flags & (O_WRONLY_L | O_RDWR_L)) f->writable = 1;

    /* writable fds claim a staging slot up front (or the open fails:
 * a small-RAM machine cannot afford an inline copy per fd). */
    if (f->writable) {
        int s = slot_claim();
        if (s < 0) { f->used = 0; return (uint32_t)-ENOMEM_L; }
        f->slot = s;
        f->data = (char*)lx_write_scratch[s];
        f->cap  = LX_WRITE_SLOT_BYTES;
    }

    if (fs_directory_exists(path)) {
        f->is_dir = 1;
        if (fs_list_directory(path, lx_dir_list, &size) != 0) {
            /* directory exists but may not be listed (no read bit):
 * report EACCES instead of handing out an empty fd */
            f->used = 0;
            return (uint32_t)-EACCES_L;
        }

        f->len = size;
        f->data = lx_dir_list;
        for (i = 0; i < MAX_PATH - 1 && path[i]; i++)
            f->path[i] = path[i];
        f->path[i] = 0;
        return (uint32_t)(f - lfd);
    }

    if (!fs_file_exists(path)) {
        if (!(flags & O_CREAT_L)) {
            f->used = 0;
            slot_release(f->slot);
            return (uint32_t)-ENOENT_L;
        }
        if (flags & O_EXCL_L) {
            f->used = 0;
            slot_release(f->slot);
            return (uint32_t)-EEXIST_L;
        }
        {
            int cr = fs_create_file(path);
            if (cr != 0) {
                /* -2 = permission denied; anything else = bad parent
 * path (ENOENT). Without this the open pretended
 * to succeed and writes vanished on flush. */
                f->used = 0;
                slot_release(f->slot);
                return (uint32_t)(cr == -2 ? -EACCES_L : -ENOENT_L);
            }
        }
        for (i = 0; i < MAX_PATH - 1 && path[i]; i++)
            f->path[i] = path[i];
        f->path[i] = 0;
        f->len = 0;
        f->dirty = 1;
        return (uint32_t)(f - lfd);
    }

    if (!f->writable) {
        /* read-only: zero-copy pointer straight into the fs pool */
        const char* d = fs_file_data_ptr(path, &size);
        if (!d && fs_errno == 13) {
            f->used = 0;
            return (uint32_t)-EACCES_L;
        }
        f->data = (char*)d;
        f->len = size;
        f->zero_copy = 1;
    } else {
        /* writable: stage the current content in the scratch slot */
        if (fs_read_file(path, f->data, &size) != 0) {
            f->used = 0;
            slot_release(f->slot);
            return (uint32_t)-EACCES_L;
        }
        if (size > f->cap) {
            f->used = 0;
            slot_release(f->slot);
            return (uint32_t)-ENOSPC_L;
        }
        f->len = size;
    }
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
    int i, shared;
    if (!f) return (uint32_t)-EBADF_L;
    fd_flush(f);
    /* dup() makes fds share a scratch slot; only the last holder
 * releases it back to the pool. */
    shared = 0;
    for (i = 3; i < LINUX_MAX_FD; i++)
        if (lfd[i].used && &lfd[i] != f && lfd[i].slot == f->slot) {
            shared = 1;
            break;
        }
    if (!shared) slot_release(f->slot);
    f->slot = -1;
    f->used = 0;
    return 0;
}

static uint32_t sys_read(uint32_t fd, uint8_t* buf, uint32_t count)
{
    linux_fd_t* f;
    uint32_t avail, n;

    if (fd == 0 && !(lfd[0].used && lfd[0].path[0] && !lfd[0].is_dir)) {
        /* stdin: line-buffered keyboard, one tty line per refill */
        if (!buf || !count) return 0;
        if (lx_stdin_pos >= lx_stdin_len) {
            int i = 0;
            read_line(lx_stdin_line, (int)sizeof lx_stdin_line - 1);
            if (!read_line_eof) {
                while (lx_stdin_line[i]) i++;
                lx_stdin_line[i++] = '\n';
            }
            lx_stdin_len = (uint32_t)i;
            lx_stdin_pos = 0;
            if (!i) return 0;                 /* EOF (Ctrl+D) */
        }
        n = lx_stdin_len - lx_stdin_pos;
        if (n > count) n = count;
        memcpy(buf, lx_stdin_line + lx_stdin_pos, n);
        lx_stdin_pos += n;
        return n;
    }
    f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    if (f->is_dir) return (uint32_t)-EISDIR_L;

    if (f->zero_copy) {
        /* re-resolve: the pool never moves an allocation, but the
 * file may have been rewritten (or deleted) since open */
        uint32_t size = 0;
        const char* d = fs_file_data_ptr(f->path, &size);
        if (!d && fs_errno == 13) return (uint32_t)-EACCES_L;
        if (!d) { f->len = 0; return 0; }     /* deleted or empty */
        f->data = (char*)d;
        f->len = size;
    }
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

    if (fd == 1 || fd == 2) {
        /* dup2() may have redirected the console onto a file */
        if (!(lfd[fd].used && lfd[fd].writable && lfd[fd].path[0])) {
            for (n = 0; n < count; n++)
                console_putc((char)buf[n]);
            return count;
        }
    }
    f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    if (!f->writable) return (uint32_t)-EBADF_L;
    if (f->is_dir) return (uint32_t)-EISDIR_L;

    if (f->append) f->pos = f->len;
    if (f->pos > f->len) {                    /* sparse gap: zero-fill */
        uint32_t gap = f->pos - f->len;
        if (f->len + gap > f->cap) return (uint32_t)-ENOSPC_L;
        memset(f->data + f->len, 0, gap);
        f->len = f->pos;
    }

    room = f->cap - f->pos;
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
    /* result is a loff_t* (64-bit): high dword must be zeroed or
 * glibc sees a huge offset and lseek() reports EOVERFLOW */
    result[0] = np;
    result[1] = 0;
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

        reclen = 19 + n + 1;
        reclen = (reclen + 7) & ~7;
        need = filled + reclen;
        if (need > (int)bufsize) break;

        rec = buf + filled;
        memset(rec, 0, (uint32_t)reclen);
 * (uint64_t*)(rec + 0) = (uint64_t)(need); /* d_ino */
 * (uint64_t*)(rec + 8) = (uint64_t)(need); /* d_off */
 * (uint16_t*)(rec + 16) = (uint16_t)reclen; /* d_reclen */
        rec[18] = (uint8_t)(is_dir ? DT_DIR_L : DT_REG_L);
        lxstrcpy((char*)(rec + 19), name);

        filled = (uint32_t)need;
    }
    return filled;
}

/* Stable, unique, non-zero pseudo-inode for a path. MUST be
 * identical for every stat of the same file and different across
 * files: glibc's dynamic loader identifies already-loaded objects
 * by (st_dev, st_ino) and will alias a second library to the first
 * one if both stats report the same inode. */
static uint32_t lxino(const char* path)
{
    uint32_t h = 2166136261u;                 /* FNV-1a */
    if (!path) return 1;
    while (*path) { h ^= (uint8_t)*path++; h *= 16777619u; }
    return h | 1u;                            /* never 0 (0 = unset) */
}

/* Fill an i386 kernel-layout struct stat64. */
static void fill_stat64m(uint8_t* s, int is_dir, uint32_t size, int fmode,
                         const char* path)
{
    /* Only 96 bytes: never write past the caller's struct - the
 * extra st_ino field of the 104-byte kernel layout overlaps
 * the caller's stack and corrupts its locals. */
    memset(s, 0, 96);
 * (uint32_t*)(s + 12) = lxino(path); /* __st_ino */
    uint32_t perm = (fmode > 0) ? ((uint32_t)fmode & 0777u)
                                 : (is_dir ? 0755u : 0644u);
    *(uint32_t*)(s + 16) = (is_dir ? S_IFDIR_L : S_IFREG_L) | perm; /* st_mode */
 * (uint32_t*)(s + 20) = 1; /* st_nlink */
 * (uint32_t*)(s + 44) = size; /* st_size lo */
 * (uint32_t*)(s + 52) = 512; /* st_blksize */
 * (uint64_t*)(s + 56) = (size + 511) / 512; /* st_blocks */
 * (uint32_t*)(s + 80) = get_uptime_ms() / 1000; /* st_mtime */
}

/* callers without a known mode keep the old defaults */
static void fill_stat64(uint8_t* s, int is_dir, uint32_t size,
                        const char* path)
{
    fill_stat64m(s, is_dir, size, 0, path);
}

static uint32_t sys_stat64(const char* path, uint8_t* s)
{
    if (!path || !s) return (uint32_t)-EINVAL_L;
    path = lxnorm(path);
    if (!path) path = "";
    if (fs_directory_exists(path)) {
        fill_stat64m(s, 1, 0, fs_get_mode(path), path);
        return 0;
    }
    if (fs_file_exists(path)) {
        fill_stat64m(s, 0, fs_get_file_size(path), fs_get_mode(path), path);
        return 0;
    }
    return (uint32_t)-ENOENT_L;
}

static uint32_t sys_fstat64(uint32_t fd, uint8_t* s)
{
    linux_fd_t* f;
    if (!s) return (uint32_t)-EINVAL_L;
    if (fd <= 2) { fill_stat64(s, 0, 0, 0); return 0; }   /* console */
    f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    fill_stat64m(s, f->is_dir, f->len,
                 f->path[0] ? fs_get_mode(f->path) : 0, f->path);
    return 0;
}

/* fstatat64(dirfd, path, stat64*, flags): modern glibc's stat entry
 * point (fstat64(fd) is fstatat64(fd, "", AT_EMPTY_PATH)). */
static uint32_t sys_fstatat64(int32_t dirfd, const char* path, uint8_t* s,
                              uint32_t flags)
{
    static char pbuf[MAX_PATH];
    const char* target = 0;
    linux_fd_t* f;

    if (!s) return (uint32_t)-EINVAL_L;

    /* empty path + AT_EMPTY_PATH: stat the fd itself */
    if ((!path || !path[0]) && (flags & AT_EMPTY_L)) {
        if (dirfd == (int32_t)AT_FDCWD_L) return (uint32_t)-ENOENT_L;
        if (dirfd >= 0 && dirfd <= 2) {        /* console fds */
            fill_stat64(s, 0, 0, 0);
            return 0;
        }
        f = fd_get((uint32_t)dirfd);
        if (!f) return (uint32_t)-EBADF_L;
        fill_stat64m(s, f->is_dir, f->len,
                     f->path[0] ? fs_get_mode(f->path) : 0, f->path);
        return 0;
    }

    if (!path) return (uint32_t)-EINVAL_L;

    if (path[0] == '/') {
        target = path;                          /* absolute */
    } else {
        const char* base = 0;
        if (dirfd == (int32_t)AT_FDCWD_L) {
            fs_get_current_path(lxnorm_buf);
            base = lxnorm_buf;
        } else if (dirfd >= 3) {
            f = fd_get((uint32_t)dirfd);
            if (!f) return (uint32_t)-EBADF_L;
            if (!f->is_dir) return (uint32_t)-ENOTDIR_L;
            base = f->path;
        } else {
            return (uint32_t)-EBADF_L;
        }
        {
            int i = 0;
            while (base[i] && i < MAX_PATH - 2) { pbuf[i] = base[i]; i++; }
            if (i && pbuf[i-1] != '/' && i < MAX_PATH - 1) pbuf[i++] = '/';
            { int j = 0; while (path[j] && i < MAX_PATH - 1) pbuf[i++] = path[j++]; }
            pbuf[i] = 0;
        }
        target = pbuf;
    }

    target = lxnorm(target);
    if (fs_directory_exists(target)) {
        fill_stat64m(s, 1, 0, fs_get_mode(target), target);
        return 0;
    }
    if (fs_file_exists(target)) {
        fill_stat64m(s, 0, fs_get_file_size(target), fs_get_mode(target), target);
        return 0;
    }
    return (uint32_t)-ENOENT_L;
}

/* statx(dirfd, path, flags, mask, struct statx*): the first stat call
 * modern glibc makes. Fills a 256-byte struct statx. */
static void fill_statxm(uint8_t* x, int is_dir, uint32_t size, int fmode,
                        const char* path)
{
    uint32_t perm = (fmode > 0) ? ((uint32_t)fmode & 0777u)
                                : (is_dir ? 0755u : 0644u);
    memset(x, 0, 256);
 * (uint32_t*)(x + 0) = 0x000007ff; /* stx_mask: BASIC_STATS */
 * (uint32_t*)(x + 4) = 512; /* stx_blksize */
 * (uint32_t*)(x + 16) = 1; /* stx_nlink */
    *(uint16_t*)(x + 28) = (uint16_t)((is_dir ? S_IFDIR_L : S_IFREG_L) | perm);
 * (uint64_t*)(x + 32) = lxino(path); /* stx_ino */
 * (uint64_t*)(x + 40) = size; /* stx_size */
 * (uint64_t*)(x + 48) = (size + 511) / 512; /* stx_blocks */
 * (uint64_t*)(x + 56) = 0x000007ff; /* attributes_mask */
 * (uint32_t*)(x + 112) = get_uptime_ms() / 1000; /* mtime sec lo */
}

/* callers without a known mode keep the old defaults */
static void fill_statx(uint8_t* x, int is_dir, uint32_t size,
                       const char* path)
{
    fill_statxm(x, is_dir, size, 0, path);
}

static uint32_t sys_statx(int32_t dirfd, const char* path, uint32_t flags,
                          uint32_t mask, uint8_t* x)
{
    static char pbuf2[MAX_PATH];
    const char* target = 0;
    linux_fd_t* f;

    if (!x) return (uint32_t)-EINVAL_L;
    (void)mask;

    if ((!path || !path[0]) && (flags & AT_EMPTY_L)) {
        if (dirfd >= 0 && dirfd <= 2) { fill_statx(x, 0, 0, 0); return 0; }
        f = fd_get((uint32_t)dirfd);
        if (!f) return (uint32_t)-EBADF_L;
        fill_statx(x, f->is_dir, f->len, f->path);
        return 0;
    }
    if (!path) return (uint32_t)-EINVAL_L;

    if (path[0] == '/') {
        target = path;
    } else {
        const char* base;
        if (dirfd == (int32_t)AT_FDCWD_L) {
            fs_get_current_path(lxnorm_buf);
            base = lxnorm_buf;
        } else if (dirfd >= 3) {
            f = fd_get((uint32_t)dirfd);
            if (!f) return (uint32_t)-EBADF_L;
            base = f->path;
        } else {
            return (uint32_t)-EBADF_L;
        }
        { int i = 0, j = 0;
          while (base[i] && i < MAX_PATH - 2) { pbuf2[i] = base[i]; i++; }
          if (i && pbuf2[i-1] != '/' && i < MAX_PATH - 1) pbuf2[i++] = '/';
          while (path[j] && i < MAX_PATH - 1) pbuf2[i++] = path[j++];
          pbuf2[i] = 0; }
        target = pbuf2;
    }

    target = lxnorm(target);
    if (fs_directory_exists(target)) {
        fill_statxm(x, 1, 0, fs_get_mode(target), target);
        return 0;
    }
    if (fs_file_exists(target)) {
        fill_statxm(x, 0, fs_get_file_size(target), fs_get_mode(target), target);
        return 0;
    }
    return (uint32_t)-ENOENT_L;
}


static uint32_t sys_mmap(uint32_t addr, uint32_t len, uint32_t flags,
                         int32_t fd, uint32_t off)
{
    uint32_t a;
    int file_backed = (fd >= 0 && !(flags & 0x20)); /* not MAP_ANONYMOUS */

    if (!len) return (uint32_t)-EINVAL_L;
    len = (len + 4095) & ~4095u;

    if (flags & 0x10) {                       /* MAP_FIXED */
        if (addr < lx_mmap_base || addr + len > lx_mmap_limit)
            return (uint32_t)-ENOMEM_L;
        a = addr;
    } else {
        if (addr) return (uint32_t)-ENODEV_L; /* only anonymous supported */
        a = linux_mmap_next;
        if (a + len > lx_mmap_limit) return (uint32_t)-ENOMEM_L;
        linux_mmap_next += len;
    }
    memset((void*)a, 0, len);

    /* File-backed mapping: the file lives whole in RAM (the fs
 * keeps it in its pool), so "mapping" is a plain copy of the
 * requested window into the freshly zeroed area. MAP_PRIVATE
 * semantics are free: no other user shares the pages. */
    if (file_backed) {
        linux_fd_t* f = fd_get((uint32_t)fd);
        if (!f) return (uint32_t)-EBADF_L;
        if (f->is_dir) return (uint32_t)-EISDIR_L;
        {
            uint32_t have = (f->len > off) ? (f->len - off) : 0;
            uint32_t got = (have < len) ? have : len;
            if (got) memcpy((void*)a, f->data + off, got);
        }
    }
    return a;
}

static uint32_t sys_brk(uint32_t addr)
{
    if (addr >= linux_brk_start && addr < lx_brk_limit) {
        /* Linux guarantees fresh anonymous memory is zero-filled:
 * every byte the program newly extends its heap by arrives
 * zeroed, or a next-run malloc reads the previous run's
 * stale chunks. */
        if (addr > linux_brk_cur)
            memset((void*)linux_brk_cur, 0, addr - linux_brk_cur);
        linux_brk_cur = addr;
    }
    return linux_brk_cur;
}

static uint32_t sys_uname(uint8_t* buf)
{
    static const char* fields[6] =
        { "Linux", "tanja", "5.15.0-tanja", "#1 SMP TanjaOS", "i686", "(none)" };
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


/* misc syscall helpers */

static uint32_t lxstrlen(const char* s)
{
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* getrandom: xorshift seeded by the uptime tick. */
static uint32_t sys_getrandom(uint8_t* buf, uint32_t count)
{
    uint32_t x = get_uptime_ms() | 1;
    uint32_t i;
    if (!buf) return (uint32_t)-EINVAL_L;
    for (i = 0; i < count; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (uint8_t)(x >> 24);
    }
    return count;
}

static void lx_fill_timespec64(uint8_t* p)
{
    uint32_t ms = get_uptime_ms();
 * (uint32_t*)(p + 0) = ms / 1000; /* tv_sec lo */
 * (uint32_t*)(p + 4) = 0; /* tv_sec hi */
 * (uint32_t*)(p + 8) = (ms % 1000) * 1000000; /* tv_nsec lo */
    *(uint32_t*)(p + 12) = 0;
}

static uint32_t sys_clock_gettime64(uint8_t* ts)
{
    if (!ts) return (uint32_t)-EINVAL_L;
    lx_fill_timespec64(ts);
    return 0;
}

static uint32_t sys_gettimeofday(uint8_t* tv)
{
    uint32_t ms = get_uptime_ms();
    if (!tv) return 0;
    *(uint32_t*)(tv + 0) = ms / 1000;
    *(uint32_t*)(tv + 4) = (ms % 1000) * 1000;
    return 0;
}

static uint32_t sys_getrlimit(uint8_t* rlim)
{
    if (!rlim) return (uint32_t)-EINVAL_L;
 * (uint32_t*)(rlim + 0) = 0xFFFFFFFFu; /* rlim_cur = inf */
 * (uint32_t*)(rlim + 4) = 0xFFFFFFFFu; /* rlim_max */
    return 0;
}

static uint32_t sys_prlimit64(uint8_t* oldp)
{
    if (oldp) {
 * (uint32_t*)(oldp + 0) = 0xFFFFFFFFu;
 * (uint32_t*)(oldp + 4) = 0xFFFFFFFFu;
 * (uint32_t*)(oldp + 8) = 0xFFFFFFFFu;
        *(uint32_t*)(oldp + 12) = 0xFFFFFFFFu;
    }
    return 0;
}

static uint32_t sys_access(const char* path)
{
    path = lxnorm(path);
    if (!path) return (uint32_t)-ENOENT_L;
    if (fs_file_exists(path) || fs_directory_exists(path)) return 0;
    return (uint32_t)-ENOENT_L;
}

static uint32_t sys_readlink(const char* path, uint8_t* buf, uint32_t sz)
{
    static const char exe[] = "/proc/self/exe";
    const char* s;
    uint32_t n, i;
    if (!path || !buf) return (uint32_t)-EINVAL_L;
    /* Only /proc/self/exe is emulated (glibc startup + backtraces). */
    for (i = 0; exe[i]; i++)
        if (!path[i] || path[i] != exe[i]) return (uint32_t)-ENOENT_L;
    if (path[i]) return (uint32_t)-ENOENT_L;
    s = linux_prog_path;
    n = lxstrlen(s);
    if (n > sz) n = sz;
    for (i = 0; i < n; i++) buf[i] = (uint8_t)s[i];
    return n;
}

static uint32_t sys_dup(uint32_t oldfd)
{
    linux_fd_t* f = fd_get(oldfd);
    linux_fd_t* n = 0;
    int i;
    if (!f) return (uint32_t)-EBADF_L;
    for (i = 3; i < LINUX_MAX_FD; i++)
        if (!lfd[i].used) { n = &lfd[i]; break; }
    if (!n) return (uint32_t)-EMFILE_L;
    memcpy(n, f, sizeof(*n));
    return (uint32_t)(n - lfd);
}

static uint32_t sys_dup2(uint32_t oldfd, uint32_t newfd)
{
    linux_fd_t* f = fd_get(oldfd);
    if (!f) return (uint32_t)-EBADF_L;
    if (newfd >= LINUX_MAX_FD) return (uint32_t)-EBADF_L;
    if (newfd == oldfd) return newfd;
    if (lfd[newfd].used) fd_flush(&lfd[newfd]);
    memcpy(&lfd[newfd], f, sizeof(*f));
    lfd[newfd].used = 1;
    return newfd;
}

static uint32_t sys_unlink(const char* path)
{
    path = lxnorm(path);
    if (!path || !fs_file_exists(path)) return (uint32_t)-ENOENT_L;
    return fs_delete_file(path) == 0 ? 0 : (uint32_t)-1;
}

static uint32_t sys_rename(const char* oldp, const char* newp)
{
    static char data[MAX_FILE_SIZE];
    char obuf[MAX_PATH], nbuf[MAX_PATH];
    const char* p;
    uint32_t size = MAX_FILE_SIZE;
    int i;

    if (!oldp || !newp) return (uint32_t)-ENOENT_L;
    p = lxnorm(oldp);
    if (!p) return (uint32_t)-ENOENT_L;
    for (i = 0; i < MAX_PATH - 1 && p[i]; i++) obuf[i] = p[i];
    obuf[i] = 0;
    p = lxnorm(newp);
    if (!p) return (uint32_t)-ENOENT_L;
    for (i = 0; i < MAX_PATH - 1 && p[i]; i++) nbuf[i] = p[i];
    nbuf[i] = 0;

    if (!fs_file_exists(obuf)) return (uint32_t)-ENOENT_L;
    if (fs_file_exists(nbuf)) return (uint32_t)-EEXIST_L;
    if (fs_read_file(obuf, data, &size) != 0) return (uint32_t)-1;
    if (fs_create_file(nbuf) != 0) return (uint32_t)-1;
    if (size && fs_write_file(nbuf, data, size) != 0) return (uint32_t)-1;
    fs_delete_file(obuf);
    return 0;
}

static uint32_t sys_ftruncate(uint32_t fd, uint32_t len)
{
    linux_fd_t* f = fd_get(fd);
    if (!f) return (uint32_t)-EBADF_L;
    if (f->is_dir) return (uint32_t)-EISDIR_L;
    if (len > MAX_FILE_SIZE) return (uint32_t)-EINVAL_L;
    if (len < f->len) memset(f->data + len, 0, f->len - len);
    f->len = len;
    if (f->pos > len) f->pos = len;
    f->dirty = 1;
    return 0;
}

static uint32_t sys_nanosleep(uint32_t req)
{
    uint32_t until, ms = 0;
    if (req) {
        ms  = *(uint32_t*)req * 1000u;              /* tv_sec  */
        ms += (*(uint32_t*)(req + 4)) / 1000000u;   /* tv_nsec */
    }
    until = get_uptime_ms() + ms;
    while ((int32_t)(until - get_uptime_ms()) > 0)
        ;                                          /* busy wait */
    return 0;
}

static uint32_t sys_times(uint8_t* tms)
{
    if (tms) memset(tms, 0, 16);
    return get_uptime_ms() / 10;
}

static uint32_t sys_getrusage(uint8_t* ru)
{
    uint32_t ms = get_uptime_ms();
    if (!ru) return 0;
    memset(ru, 0, 72);
 * (uint32_t*)(ru + 0) = ms / 1000; /* ru_utime.tv_sec */
 * (uint32_t*)(ru + 8) = ms / 1000; /* ru_stime.tv_sec */
    return 0;
}

static uint32_t sys_sysinfo(uint8_t* si)
{
    if (!si) return (uint32_t)-EINVAL_L;
    memset(si, 0, 64);
 * (uint32_t*)(si + 0) = get_uptime_ms() / 1000; /* uptime */
 * (uint32_t*)(si + 16) = 0x40000000; /* totalram */
 * (uint32_t*)(si + 20) = 0x10000000; /* freeram */
 * (uint32_t*)(si + 40) = 1; /* procs */
    return 0;
}

static uint32_t sys_mremap(uint32_t old, uint32_t oldsz, uint32_t newsz)
{
    uint32_t n;
    if (!newsz) return (uint32_t)-EINVAL_L;
    n = sys_mmap(0, newsz, 0x32, -1, 0); /* MAP_PRIVATE|MAP_ANONYMOUS */
    if ((int32_t)n < 0) return n;
    memcpy((void*)n, (const void*)old, oldsz < newsz ? oldsz : newsz);
    return n;
}

/* the dispatch table */

void linux_syscall(linux_regs_t* r)
{
    uint32_t nr = r->eax;
    uint32_t a1 = r->ebx, a2 = r->ecx, a3 = r->edx,
             a4 = r->esi, a5 = r->edi, a6 = r->ebp;

    {
        uint32_t slot = lx_log_i % LX_LOG_N;
        lx_log_nr[slot] = nr;
        lx_log_a1[slot] = a1; lx_log_a2[slot] = a2; lx_log_a3[slot] = a3;
        lx_log_res[slot] = 0;
        lx_log_i++;
    }
    if (lx_trace) {
        print("[linux] sys ");
        print_dec(nr);
        print("(");
        print_dec(a1); print(",");
        print_dec(a2); print(",");
        print_dec(a3);
        print(")\n");
    }

    switch (nr) {

    case 1: case 252:                           /* exit / exit_group */
        linux_exit_code = (int)a1;
        fd_flush_all();
        linux_exit_restore();                   /* noreturn */
        break;                                  /* not reached */

    case 3:  r->eax = sys_read(a1, (uint8_t*)a2, a3); break;
    case 4:  r->eax = sys_write(a1, (const uint8_t*)a2, a3); break;
    case 5:  r->eax = sys_open((const char*)a1, a2); break;
    case 6:  r->eax = sys_close(a1); break;
    case 8:  r->eax = sys_open((const char*)a1,
                    O_WRONLY_L | O_CREAT_L | O_TRUNC_L); break;   /* creat */
    case 10: r->eax = sys_unlink((const char*)a1); break;
    case 12: r->eax = fs_change_directory(lxnorm((const char*)a1)) == 0
                       ? 0 : (uint32_t)-ENOENT_L; break;
    case 13: r->eax = get_uptime_ms() / 1000; break;              /* time */
    case 15: r->eax = fs_chmod(lxnorm((const char*)a1), (int)a2) == 0
                       ? 0 : (uint32_t)-1; break;                 /* chmod */
    case 19: r->eax = sys_lseek(a1, (int32_t)a2, a3); break;
    case 20: r->eax = 1; break;                                   /* getpid */
    case 64: r->eax = 0; break;                                   /* getppid: 0 = init's parent */
    case 24: case 46: case 47: case 49:                          /* uid/gid */
    case 199: case 200: case 201: case 202:
        r->eax = 0; break;
    case 33: r->eax = sys_access((const char*)a1); break;
    case 38: r->eax = sys_rename((const char*)a1, (const char*)a2); break;
    case 39: r->eax = fs_create_directory(lxnorm((const char*)a1)) == 0
                       ? 0 : (uint32_t)-1; break;                 /* mkdir */
    case 40: r->eax = fs_delete_directory(lxnorm((const char*)a1)) == 0
                       ? 0 : (uint32_t)-1; break;                 /* rmdir */
    case 41: r->eax = sys_dup(a1); break;
    case 43: r->eax = sys_times((uint8_t*)a1); break;
    case 45: r->eax = sys_brk(a1); break;
    case 54:                                                 /* ioctl */
        /* TCGETS on a console fd: report it as a tty so glibc's
 * isatty() returns 1 for fd 0/1/2; everything else ENOTTY */
        if (a2 == 0x5401u && a1 <= 2) {
            if (a3) memset((void*)a3, 0, 36);    /* struct termios */
            r->eax = 0;
        } else {
            r->eax = (uint32_t)-ENOTTY_L;
        }
        break;
    case 55: case 221: r->eax = 0; break;                   /* fcntl(64) */
    case 63: r->eax = sys_dup2(a1, a2); break;
    case 76: case 191: r->eax = sys_getrlimit((uint8_t*)a2); break;
    case 77: r->eax = sys_getrusage((uint8_t*)a2); break;
    case 78: r->eax = sys_gettimeofday((uint8_t*)a1); break;
    case 85: r->eax = sys_readlink((const char*)a1, (uint8_t*)a2, a3); break;
    case 192: r->eax = sys_mmap(a1, a2, a4, (int32_t)a5, a6 << 12); break;
    case 90: {  /* old mmap: pointer to struct {addr,len,prot,flags,fd,off} */
        uint32_t* ms = (uint32_t*)a1;
        if (!ms) { r->eax = (uint32_t)-EINVAL_L; break; }
        r->eax = sys_mmap(ms[0], ms[1], ms[3], (int32_t)ms[4], ms[5]);
        break;
    }
    case 91: r->eax = 0; break;                                   /* munmap */
    case 93: case 194: r->eax = sys_ftruncate(a1, a2); break;
    case 108: r->eax = sys_fstat64(a1, (uint8_t*)a2); break;
    case 116: r->eax = sys_sysinfo((uint8_t*)a1); break;
    case 119: case 173: r->eax = 0; break;              /* sigreturn(s) */
    case 122: r->eax = sys_uname((uint8_t*)a1); break;
    case 125: r->eax = 0; break;              /* mprotect: no paging */
    case 140: r->eax = sys_llseek(a1, a2, a3, (uint32_t*)a4, a5); break;
    case 145: {                                          /* readv */
        uint32_t total = 0, i;
        if (!a2) return;
        for (i = 0; i < a3; i++) {
            uint32_t base = *(uint32_t*)(a2 + i * 8);
            uint32_t len  = *(uint32_t*)(a2 + i * 8 + 4);
            uint32_t rd = sys_read(a1, (uint8_t*)base, len);
            if ((int32_t)rd < 0) { r->eax = rd; return; }
            total += rd;
            if (rd < len) break;
        }
        r->eax = total;
        break;
    }
    case 146: r->eax = sys_writev(a1, a2, a3); break;
    case 162: r->eax = sys_nanosleep(a1); break;
    case 163: r->eax = sys_mremap(a1, a2, a3); break;            /* mremap */
    case 174: case 175: r->eax = 0; break;      /* rt_sigaction/mask */
    case 180: { /* pread64: (fd, buf, count, pad, off_lo, off_hi) - i386
 * passes a 32-bit pad then the 64-bit offset; glibc
 * puts lo in the stack slot right after the pad */
        linux_fd_t* f = fd_get(a1);
        if (!f || f->is_dir) { r->eax = (uint32_t)-EBADF_L; break; }
        {
            uint64_t o = ((uint64_t)a5 << 32) | a6;
            uint32_t have = (o < f->len) ? ((uint64_t)f->len - o) : 0;
            uint32_t n = (have < a3) ? have : a3;
            if (n) memcpy((void*)a2, f->data + (uint32_t)o, n);
            r->eax = n;
        }
        break;
    }
    case 186: r->eax = 0; break;             /* sigaltstack */
    case 183: r->eax = sys_getcwd((char*)a1, a2); break;
    case 195: case 196: r->eax = sys_stat64((const char*)a1,
                                            (uint8_t*)a2); break;
    case 197: r->eax = sys_fstat64(a1, (uint8_t*)a2); break;
    case 300: r->eax = sys_fstatat64((int32_t)a1, (const char*)a2,
                                     (uint8_t*)a3, a5); break;
    case 383: r->eax = sys_statx((int32_t)a1, (const char*)a2, a3,
                                 a4, (uint8_t*)a5); break;
    case 219: r->eax = 0; break;                       /* madvise */
    case 220: r->eax = sys_getdents64(a1, (uint8_t*)a2, a3); break;
    case 224: r->eax = 1; break;                       /* gettid */
    case 240: /* futex: single thread - WAKE reports 1 woken, WAIT
 * never actually blocks (nothing else can hold the
 * lock; a real wait would deadlock the program) */
        r->eax = ((a2 & 0x7fu) == 1) ? 1 : 0;
        break;
    case 243: r->eax = sys_set_thread_area((uint32_t*)a1); break;
    case 251: case 258: r->eax = 1; break;         /* set_tid_address */
    case 265: {                                  /* clock_gettime */
        uint32_t ms = get_uptime_ms();
        if (a2) {
            *(uint32_t*)a2 = ms / 1000;
            *((uint32_t*)a2 + 1) = (ms % 1000) * 1000000;
        }
        r->eax = 0;
        break;
    }
    case 266: if (a2) { *(uint32_t*)a2 = 0; *((uint32_t*)a2 + 1) = 1; }
              r->eax = 0; break;                       /* clock_getres */
    case 295: r->eax = sys_open((const char*)a2, a3); break;  /* openat */
    case 307: r->eax = sys_access((const char*)a2); break;   /* faccessat */
    case 340: r->eax = sys_prlimit64((uint8_t*)a4); break;
    case 355: r->eax = sys_getrandom((uint8_t*)a1, a2); break;
    case 172: r->eax = 0; break;                         /* prctl */
    case 37:  case 270: {                    /* kill / tgkill */
        /* glibc abort()/raise() end here with SIGABRT. Honour the
 * signal as program termination and unwind to the shell,
 * exactly like exit(), so a failed program cannot spin. */
        int sig = (nr == 270) ? (int)a3 : (int)a2;
        if (sig == 6 || sig == 9) {              /* ABRT / KILL */
            print("program aborted (signal ");
            print_dec(sig);
            print(") - program terminated\n");
            linux_exit_code = 128 + sig;
            fd_flush_all();
            linux_exit_restore();               /* noreturn */
        }
        r->eax = 0;                              /* ignored signal */
        break;
    }
    case 403: r->eax = sys_clock_gettime64((uint8_t*)a2); break;
    case 406: if (a2) { memset((void*)a2, 0, 16);
                        *(uint32_t*)(a2 + 8) = 1; }
              r->eax = 0; break;               /* clock_getres_time64 */

    /* Known-benign syscalls a static binary probes but this single
 * tasking, no-network model cannot honour. glibc/real programs
 * tolerate ENOSYS for all of these - staying QUIET avoids
 * misleading error spam on a successful run. */
    case 2:   case 7:   case 9:   case 11:  case 42:  case 67:  /* fork.. */
    case 82:  case 114: case 117: case 120: case 126: case 141:
    case 168: case 298: case 301: case 302: case 305:
    case 306: case 311: case 331: case 353: case 356:
    case 386: case 422: case 435: case 439:
        r->eax = (uint32_t)-ENOSYS_L;
        break;

    default:
        print("linux: unimplemented syscall ");
        print_dec(nr);
        print("\n");
        r->eax = (uint32_t)-ENOSYS_L;
        break;
    }
    lx_log_res[(lx_log_i - 1) % LX_LOG_N] = r->eax;
}

/* linux_run - load a static ET_EXEC i386 image and run it
 */

int linux_run(const uint8_t* image, uint32_t size, const char* path,
              const char* args)
{
    const uint8_t* e = image;
    uint32_t entry, phoff, phentsize, phnum, max_end = 0;
    uint32_t hdr_vaddr = 0;      /* vaddr the ELF header itself maps to */
    uint16_t e_type;
    uint32_t load_bias = 0;
    uint32_t interp_off = 0;     /* file offset of the PT_INTERP string */
    const char* interp_path = 0;
    uint32_t ld_base = 0;        /* nonzero once the interpreter is loaded */
    int i;

    if (size < 52) return -1;
    e_type     = *(uint16_t*)(e + 16);
    entry      = *(uint32_t*)(e + 24);
    phoff      = *(uint32_t*)(e + 28);
    phentsize  = *(uint16_t*)(e + 42);
    phnum      = *(uint16_t*)(e + 44);
    if (phentsize < sizeof(Elf32_Phdr) || !phnum) return -1;
    if (phoff + phnum * phentsize > size) return -1;

    /* ET_DYN executables (PIE) carry relative addresses; they get a
 * fixed load bias so their absolute code lives below the static
 * ET_EXEC area. Everything else is mapped verbatim as before. */
    if (e_type == 3 /* ET_DYN */)
        load_bias = lx_exe_base;

    /* find PT_INTERP (a dynamic executable names its interpreter) */
    for (i = 0; i < (int)phnum; i++) {
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(e + phoff + i * phentsize);
        if (ph->p_type == 3 /* PT_INTERP */ && ph->p_filesz > 1 &&
            ph->p_offset + ph->p_filesz <= size) {
            interp_off = ph->p_offset;
            interp_path = (const char*)(e + ph->p_offset);
        }
    }

    /* copy PT_LOAD segments to their virtual addresses */
    for (i = 0; i < (int)phnum; i++) {
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(e + phoff + i * phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) return -1;
        if (ph->p_offset + ph->p_filesz > size) return -1;
        memcpy((void*)(load_bias + ph->p_vaddr), e + ph->p_offset,
               ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void*)(load_bias + ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
        if (load_bias + ph->p_vaddr + ph->p_memsz > max_end)
            max_end = load_bias + ph->p_vaddr + ph->p_memsz;
        /* the segment that contains file offset 0 holds the ELF
 * header and the program-header table: remember where it
 * landed so AT_PHDR can point at the mapped phdrs */
        if (ph->p_offset == 0)
            hdr_vaddr = load_bias + ph->p_vaddr;
    }
    if (!max_end) return -1;

    /* Zero the tail of the last page past p_memsz: ld.so's minimal
 * malloc arena starts at _end and glibc's __minimal_calloc
 * assumes bump-allocated memory is already zero. Without this,
 * a second dynamic run reuses stale link-map bytes from the
 * previous run and dl_main skips the main map's relocation. */
    {
        uint32_t tail_end = (max_end + 4095) & ~4095u;
        if (tail_end > max_end)
            memset((void*)max_end, 0, tail_end - max_end);
    }

    /* dynamic: load the ELF interpreter (ld-linux) */
    if (interp_path) {
        static uint8_t ldimg[512 * 1024];
        uint32_t ldsize = sizeof ldimg;
        uint32_t lde, ldph, ldphent, ldphnum, ldmax = 0;

        if (fs_read_file(interp_path, (char*)ldimg, &ldsize) != 0) {
            print("elf: interpreter not found: ");
            print(interp_path);
            print("\n");
            return -1;
        }
        if (ldsize < 52) return -1;
        lde     = *(uint32_t*)(ldimg + 24);
        ldph    = *(uint32_t*)(ldimg + 28);
        ldphent = *(uint16_t*)(ldimg + 42);
        ldphnum = *(uint16_t*)(ldimg + 44);
        if (ldphent < sizeof(Elf32_Phdr) || !ldphnum) return -1;
        if (ldph + ldphnum * ldphent > ldsize) return -1;
        for (i = 0; i < (int)ldphnum; i++) {
            const Elf32_Phdr* ph =
                (const Elf32_Phdr*)(ldimg + ldph + i * ldphent);
            if (ph->p_type != PT_LOAD) continue;
            if (ph->p_offset + ph->p_filesz > ldsize) return -1;
            memcpy((void*)(lx_ld_base + ph->p_vaddr),
                   ldimg + ph->p_offset, ph->p_filesz);
            if (ph->p_memsz > ph->p_filesz)
                memset((void*)(lx_ld_base + ph->p_vaddr + ph->p_filesz), 0,
                       ph->p_memsz - ph->p_filesz);
            if (lx_ld_base + ph->p_vaddr + ph->p_memsz > ldmax)
                ldmax = lx_ld_base + ph->p_vaddr + ph->p_memsz;
        }
        if (!ldmax) return -1;
        {
            uint32_t tail_end = (ldmax + 4095) & ~4095u;
            if (tail_end > ldmax)
                memset((void*)ldmax, 0, tail_end - ldmax);
        }
        ld_base = lx_ld_base;
        /* execution begins in the interpreter; it relocates itself
 * against ld_base, maps libc, then jumps to AT_ENTRY */
        entry = lx_ld_base + lde;
    } else if (load_bias) {
        entry = load_bias + entry;   /* static-PIE: bias the entry too */
    }

    /* Per-run state: debug trace, /proc/self/exe, fds, stdin buffer. */
    {
        const char* t = get_env("LTRACE");
        if (!t || !t[0]) t = get_env("LINUX_TRACE");
        lx_trace = (t && t[0] && t[0] != '0');
    }
    for (i = 0; i < MAX_PATH - 1 && path[i]; i++)
        linux_prog_path[i] = path[i];
    linux_prog_path[i] = 0;

    /* The image must fit between the mmap region and the brk limit
 * (which sits below the stack). On small-RAM machines a very
 * large PIE can be too big to place - refuse with a clear error
 * instead of overwriting the stack or mmap region. */
    if (max_end > lx_brk_limit) {
        print("linux: executable too large for this machine's RAM");
        print(" (needs more than ");
        print_dec((max_end - load_bias) / (1024u * 1024u));
        print(" MiB above ");
        print_dec((lx_exe_base) / (1024u * 1024u));
        print(" MiB)\n");
        return -1;
    }

    linux_brk_start = (max_end + 4095) & ~4095u;
    linux_brk_cur  = linux_brk_start;
    linux_mmap_next = lx_mmap_base;
    linux_exit_code = 0;
    lx_log_i = 0;                       /* fresh syscall trace per run */

    /* Fresh RAM per run: a previous dynamic run leaves stale bytes
 * in the program stack area (below the initial sp) and in the
 * mmap region; zero both so every run starts from identical RAM. */
    memset((void*)(lx_stack_top - 0x00050000u), 0, 0x00050000u); /* stack */
    memset((void*)lx_mmap_base, 0,
           lx_mmap_limit - lx_mmap_base);           /* mmap region   */
    memset(lx_write_slot_used, 0, sizeof lx_write_slot_used);
    for (i = 0; i < LINUX_MAX_FD; i++) {
        lfd[i].used = 0;
        memset(&lfd[i], 0, sizeof lfd[i]);
    }
    lfd[0].used = lfd[1].used = lfd[2].used = 1;   /* std fds */
    lx_stdin_pos = lx_stdin_len = 0;

    /* build the initial stack (Linux process ABI) */
    {
        char* sp_top = (char*)lx_stack_top;
        char* sp;
        uint32_t argv_ptrs[32];
        uint32_t randp, execfnp;
        uint32_t words[96];
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

        /* AT_EXECFN: the full path as given (glibc's dl prints it
 * in loader diagnostics and reads it for /proc/self/exe) */
        execfnp = (uint32_t)(char*)sp_top;
        {
            const char* p2 = path;
            while (*p2) *sp_top++ = *p2++;
            *sp_top++ = 0;
        }

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
        words[w++] = AT_EXECFN_L;  words[w++] = execfnp;
        if (hdr_vaddr && hdr_vaddr + phoff + phnum * phentsize
                              <= hdr_vaddr + max_end) {
            /* phdrs are inside the loaded image: point dl at them */
            words[w++] = AT_PHDR_L;   words[w++] = hdr_vaddr + phoff;
            words[w++] = AT_PHENT_L;  words[w++] = phentsize;
            words[w++] = AT_PHNUM_L;  words[w++] = phnum;
        }
        words[w++] = AT_BASE_L;   words[w++] = ld_base;
        words[w++] = AT_FLAGS_L;  words[w++] = 0;
        /* AT_ENTRY is the USER program's entry even though control
 * starts in the interpreter - ld.so jumps there when done */
        words[w++] = AT_ENTRY_L;
        words[w++] = load_bias + *(uint32_t*)(e + 24);
        words[w++] = AT_NULL_L;   words[w++] = 0;

        /* The word block must end AT OR BELOW the string area's
 * start (LINUX_STACK_TOP), never inside the strings: sp_top
 * has already advanced past the strings + AT_RANDOM bytes. */
        sp = (char*)(((uint32_t)lx_stack_top - (uint32_t)(w * 4)) & ~15u);
        memcpy(sp, words, (uint32_t)(w * 4));

        
        /* Crash isolation: a fault or Ctrl+C inside the program
 * longjmps back here, exactly as exit() does through
 * linux_exit_restore. The jmp buffer is saved/restored so
 * nested programs unwind to their own linux_run/elf_run. */
        {
            int save_active = tanja_exit_active;
            int save_jmp[5];
            memcpy(save_jmp, tanja_exit_jmp, sizeof save_jmp);
            tanja_exit_active = 1;
            if (__builtin_setjmp(tanja_exit_jmp)) {
                /* fault / Ctrl+C: the kernel already printed why */
                print("linux: unwind reason ");
                print_dec(lx_unwind_reason);
                print(", vec ");
                print_dec(lx_fault_vec);
                print(", eip ");
                print_dec(lx_fault_eip);
                print("\n");
                lx_log_dump();
            } else {
                linux_enter(entry, (uint32_t)(char*)sp);
                /* exit()/exit_group(): asm resumed us right here */
            }
            memcpy(tanja_exit_jmp, save_jmp, sizeof save_jmp);
            tanja_exit_active = save_active;
            kb_raw_mode = 0;          /* keyboard belongs to the shell */
            fd_flush_all();
        }
    }

    /* linux_enter "returns" here after exit(): the asm restored the
 * kernel stack, so we unwind exactly as if it never happened. */
    return linux_exit_code;
}


