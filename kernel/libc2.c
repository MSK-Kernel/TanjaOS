/* libc.c - the TanjaOS kernel C library (part 2).
 *
 * fd table + FILE streams, POSIX file calls, string extras,
 * strtol family, qsort/bsearch, environment, time, signal stubs,
 * setjmp/longjmp, the x87 math library and program argv.
 *
 * Export list: libc.h. printf/scanf/malloc/exit live in libc.c
 * (part 1).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "libc.h"
#include "../include/fs.h"

extern void print(const char* s);
extern void print_n(const char* s, uint32_t len);
extern int get_key(void);
extern void read_line(char* buf, int max_len);
extern int strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern char* strcpy(char* dst, const char* src);
extern char* strcat(char* dst, const char* src);
extern char* strchr(const char* s, int c);
extern void* memset(void* dst, int value, unsigned int n);
extern void* memcpy(void* dst, const void* src, unsigned int n);
extern void* memmove(void* dst, const void* src, unsigned int n);
extern int isspace(int c);
extern int toupper(int c);
extern uint32_t get_uptime_ms(void);
extern void execute_command(const char* cmd_line);
extern int exec_file(const char* path, const char* args);
extern const char* get_env(const char* name);
extern void set_env(const char* name, const char* value);
extern int errno;
extern void* malloc(size_t n);
extern void free(void* p);
extern void* realloc(void* p, size_t n);

/* fd table: the one true file-stream state
 */

#define TANJA_MAX_FDS 16
#define FD_CONSOLE_IN  1
#define FD_CONSOLE_OUT 2

struct tanja_fd {
    int used;
    int is_console;        /* 0 none, 1 stdin, 2 stdout/stderr */
    char* path;            /* malloc'd; 0 for console */
    uint8_t* buf;          /* malloc'd content (files) */
    uint32_t size, cap, pos;
    int readable, writable, append;
    int eof, error;
    int dirty;
    int unget, have_unget; /* one-char pushback */
};

static struct tanja_fd fdt[TANJA_MAX_FDS];

/* The three standard streams (exported to programs like VGA). */
FILE_T* stdin = 0;
FILE_T* stdout = 0;
FILE_T* stderr = 0;

int tanja_fd_is_console(struct tanja_fd* f) { return f && f->is_console; }

static void fd_console_init(void) {
    fdt[0].used = 1; fdt[0].is_console = FD_CONSOLE_IN; fdt[0].readable = 1;
    fdt[1].used = 1; fdt[1].is_console = FD_CONSOLE_OUT; fdt[1].writable = 1;
    fdt[2].used = 1; fdt[2].is_console = FD_CONSOLE_OUT; fdt[2].writable = 1;
    stdin = &fdt[0];
    stdout = &fdt[1];
    stderr = &fdt[2];
}

static struct tanja_fd* fd_alloc(void) {
    if (!fdt[0].used) fd_console_init();
    for (int i = 3; i < TANJA_MAX_FDS; i++) {
        if (!fdt[i].used) {
            memset(&fdt[i], 0, sizeof fdt[i]);
            fdt[i].used = 1;
            return &fdt[i];
        }
    }
    return 0;
}

static void fd_flush(struct tanja_fd* f) {
    if (f && f->used && !f->is_console && f->dirty && f->path && f->writable) {
        fs_write_file(f->path, (const char*)f->buf, f->size);
        f->dirty = 0;
    }
}

static int fd_load(struct tanja_fd* f, const char* path) {
    uint32_t sz = fs_get_file_size(path);
    if (fs_file_exists(path)) {
        f->buf = (uint8_t*)malloc(sz + 1);
        if (!f->buf) return -1;
        uint32_t cap = sz + 1;
        if (fs_read_file(path, (char*)f->buf, &cap) != 0) {
            free(f->buf);
            f->buf = 0;
            return -1;
        }
        f->size = cap;
        f->cap = sz + 1;
        return 0;
    }
    /* create empty content */
    f->buf = (uint8_t*)malloc(64);
    if (!f->buf) return -1;
    f->cap = 64;
    f->size = 0;
    return 0;
}

static int fd_ensure(struct tanja_fd* f, uint32_t need) {
    if (need <= f->cap) return 0;
    uint32_t nc = f->cap ? f->cap : 64;
    while (nc < need) nc *= 2;
    uint8_t* nb = (uint8_t*)realloc(f->buf, nc);
    if (!nb) return -1;
    f->buf = nb;
    f->cap = nc;
    return 0;
}

/* fcntl.h flags (see tools/tcc/std-include/fcntl.h) */
#define TANJA_O_WRONLY 00000001
#define TANJA_O_RDWR   00000002
#define TANJA_O_CREAT  00000100
#define TANJA_O_TRUNC  00001000
#define TANJA_O_APPEND 00002000

int open(const char* path, int flags) {
    if (!path || !path[0]) { errno = 2; return -1; }
    struct tanja_fd* f = fd_alloc();
    if (!f) { errno = 4; return -1; }
    int rd = !(flags & TANJA_O_WRONLY);
    int wr = (flags & (TANJA_O_WRONLY | TANJA_O_RDWR)) != 0;
    f->readable = rd;
    f->writable = wr;
    f->append = (flags & TANJA_O_APPEND) != 0;
    /* a missing file must fail unless O_CREAT was given */
    if (!(flags & TANJA_O_CREAT) && !fs_file_exists(path)) {
        f->used = 0;
        errno = 2;
        return -1;
    }
    if (fd_load(f, path) != 0) {
        if (!(flags & TANJA_O_CREAT)) {
            f->used = 0;
            errno = 2;
            return -1;
        }
        if (fs_create_file(path) != 0) {
            free(f->buf);
            f->buf = 0;
            f->used = 0;
            errno = 13;
            return -1;
        }
    } else if (flags & TANJA_O_TRUNC) {
        f->size = 0;
        f->dirty = 1;
    }
    if (f->append) f->pos = f->size;
    f->path = (char*)malloc(strlen(path) + 1);
    if (f->path) strcpy(f->path, path);
    return (int)(f - fdt);
}

int close(int fd) {
    if (fd < 0 || fd >= TANJA_MAX_FDS || !fdt[fd].used || fdt[fd].is_console)
        { errno = 9; return -1; }
    struct tanja_fd* f = &fdt[fd];
    fd_flush(f);
    if (f->path) free(f->path);
    if (f->buf) free(f->buf);
    f->used = 0;
    return 0;
}

int tanja_fd_getc(struct tanja_fd* f) {
    if (!f || !f->used) return -1;
    if (f->have_unget) { f->have_unget = 0; return f->unget; }
    if (f->is_console == FD_CONSOLE_IN)
        return get_key();
    if (f->pos >= f->size) { f->eof = 1; return -1; }
    return f->buf[f->pos++];
}

int tanja_fd_write(struct tanja_fd* f, const char* buf, uint32_t len) {
    if (!f || !f->used || !f->writable) return -1;
    if (f->is_console == FD_CONSOLE_OUT) {
        print_n(buf, len);
        return (int)len;
    }
    if (f->append) f->pos = f->size;
    if (fd_ensure(f, f->pos + len + 1) != 0) { f->error = 1; return -1; }
    memcpy(f->buf + f->pos, buf, len);
    f->pos += len;
    if (f->pos > f->size) f->size = f->pos;
    f->dirty = 1;
    return (int)len;
}

ssize_t read(int fd, void* buf, size_t count) {
    if (fd < 0 || fd >= TANJA_MAX_FDS || !fdt[fd].used || !fdt[fd].readable)
        { errno = 9; return -1; }
    struct tanja_fd* f = &fdt[fd];
    uint8_t* dst = (uint8_t*)buf;
    size_t n = 0;
    while (n < count) {
        int c = tanja_fd_getc(f);
        if (c < 0) break;
        dst[n++] = (uint8_t)c;
        if (f->is_console && c == '\n') break;   /* tty: line read */
    }
    return (ssize_t)(n ? n : (count ? -1 : 0));
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (fd < 0 || fd >= TANJA_MAX_FDS || !fdt[fd].used)
        { errno = 9; return -1; }
    struct tanja_fd* f = &fdt[fd];
    int r = tanja_fd_write(f, (const char*)buf, (uint32_t)count);
    if (r < 0) { errno = 9; return -1; }
    return (ssize_t)r;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= TANJA_MAX_FDS || !fdt[fd].used || fdt[fd].is_console)
        { errno = 29; return -1; }
    struct tanja_fd* f = &fdt[fd];
    if (whence == 0) f->pos = (uint32_t)offset;
    else if (whence == 1) f->pos += (uint32_t)offset;
    else if (whence == 2) f->pos = f->size + (uint32_t)offset;
    else { errno = 22; return -1; }
    if (f->pos > f->size) f->pos = f->size;
    f->eof = 0;
    return (off_t)f->pos;
}

int unlink(const char* path) { return fs_delete_file(path); }
int remove(const char* path) {
    if (fs_file_exists(path)) return fs_delete_file(path);
    if (fs_directory_exists(path)) return fs_delete_directory(path);
    errno = 2;
    return -1;
}

int rename(const char* oldpath, const char* newpath) {
    if (!fs_file_exists(oldpath)) { errno = 2; return -1; }
    uint32_t sz = fs_get_file_size(oldpath);
    char* tmp = (char*)malloc(sz + 1);
    if (!tmp) { errno = 12; return -1; }
    uint32_t cap = sz + 1;
    if (fs_read_file(oldpath, tmp, &cap) != 0) { free(tmp); errno = 2; return -1; }
    if (!fs_file_exists(newpath) && fs_create_file(newpath) != 0) {
        free(tmp); errno = 13; return -1;
    }
    int w = fs_write_file(newpath, tmp, cap);
    free(tmp);
    if (w != 0) { errno = 13; return -1; }
    return fs_delete_file(oldpath);
}

char* getcwd(char* buf, size_t size) {
    fs_get_current_path(buf);
    (void)size;
    return buf;
}

char* realpath(const char* path, char* resolved) {
    /* no symlinks in TanjaOS: a path is its own real path */
    char* dst = resolved;
    if (!dst) dst = (char*)malloc(strlen(path) + 1);
    if (!dst) return 0;
    strcpy(dst, path);
    return dst;
}

int execv(const char* path, char* const argv[]) {
    static char joined[2048];
    int n = 0;
    joined[0] = 0;
    if (argv) {
        for (int i = 1; argv[i] && n < (int)sizeof joined - 2; i++) {
            int l = strlen(argv[i]);
            if (n + l + 2 >= (int)sizeof joined - 1) break;
            if (n) joined[n++] = ' ';
            for (int j = 0; j < l; j++) joined[n++] = argv[i][j];
            joined[n] = 0;
        }
    }
    return exec_file(path, joined);
}

int execvp(const char* file, char* const argv[]) {
    int r = execv(file, argv);
    static char pbuf[512];
    if (file[0] != '/' && r != 0) {
        strcpy(pbuf, "/bin/");
        strcat(pbuf, file);
        r = execv(pbuf, argv);
    }
    return r;
}

/* FILE streams (thin wrappers over the fd table)
 */

typedef struct tanja_fd FILE;

static struct tanja_fd* file_from_fd(int fd) {
    if (fd < 0 || fd >= TANJA_MAX_FDS || !fdt[fd].used) return 0;
    return &fdt[fd];
}

FILE* fdopen(int fd, const char* mode) { return file_from_fd(fd); }

FILE* fopen(const char* path, const char* mode) {
    if (!mode || !mode[0]) { errno = 22; return 0; }
    int flags = 0;
    int w = mode[0] == 'w';
    int a = mode[0] == 'a';
    if (w) flags = TANJA_O_WRONLY | TANJA_O_CREAT | TANJA_O_TRUNC;
    else if (a) flags = TANJA_O_WRONLY | TANJA_O_CREAT | TANJA_O_APPEND;
    else flags = 1 /* RDONLY */;
    /* r+ etc. */
    for (const char* m = mode; *m; m++) if (*m == '+') { flags |= TANJA_O_RDWR | (w||a ? TANJA_O_CREAT : 0); }
    int fd = open(path, flags);
    if (fd < 0) return 0;
    return &fdt[fd];
}

FILE* freopen(const char* path, const char* mode, FILE* f) {
    if (f && f->used && !f->is_console) {
        fd_flush(f);
        if (f->path) free(f->path);
        if (f->buf) free(f->buf);
        f->used = 0;
    }
    if (!path) return 0;
    return fopen(path, mode);
}

int fclose(FILE* f) {
    if (!f || !f->used) return -1;
    if (f->is_console) return 0;   /* console streams never close */
    int r = close((int)(f - fdt));
    return r;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!f || !f->used || !f->readable) return 0;
    uint8_t* dst = (uint8_t*)ptr;
    size_t want = size * nmemb, got = 0;
    while (got < want) {
        int c = tanja_fd_getc(f);
        if (c < 0) break;
        dst[got++] = (uint8_t)c;
    }
    return size ? got / size : 0;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!f || !f->used || !f->writable) return 0;
    size_t n = size * nmemb;
    int w = tanja_fd_write(f, (const char*)ptr, (uint32_t)n);
    if (w < 0) return 0;
    return size ? (size_t)w / size : 0;
}

int fseek(FILE* f, long offset, int whence) {
    return lseek((int)(f - fdt), (off_t)offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE* f) { return (long)f->pos; }
int feof(FILE* f) { return f ? f->eof : 0; }
int ferror(FILE* f) { return f ? f->error : 0; }

int fflush(FILE* f) {
    if (!f) {
        for (int i = 0; i < TANJA_MAX_FDS; i++) fd_flush(&fdt[i]);
        return 0;
    }
    fd_flush(f);
    return 0;
}

int fgetc(FILE* f) { return tanja_fd_getc(f); }
int getc(FILE* f) { return tanja_fd_getc(f); }

int fputc(int c, FILE* f) {
    char b = (char)c;
    return tanja_fd_write(f, &b, 1) == 1 ? (unsigned char)b : -1;
}

int putc(int c, FILE* f) { return fputc(c, f); }

int fputs(const char* s, FILE* f) {
    if (!s) return -1;
    int n = strlen(s);
    return tanja_fd_write(f, s, (uint32_t)n) == n ? 0 : -1;
}

int ungetc(int c, FILE* f) {
    if (!f || !f->used) return -1;
    f->unget = c;
    f->have_unget = 1;
    f->eof = 0;
    return c;
}

char* fgets(char* s, int size, FILE* f) {
    if (!f || !f->used || size <= 0) return 0;
    /* console input: line-based, with local echo */
    if (f->is_console == FD_CONSOLE_IN) {
        static char cline[2048];
        read_line(cline, (int)sizeof cline - 1);
        int i = 0;
        while (i < size - 1 && cline[i]) { s[i] = cline[i]; i++; }
        if (i < size) s[i] = '\n';   /* fgets keeps the newline */
        if (i + 1 < size) s[i + 1] = 0; else s[size - 1] = 0;
        return s;
    }
    int i = 0;
    for (;;) {
        int c = tanja_fd_getc(f);
        if (c < 0) { s[i] = 0; return i ? s : 0; }
        if (i < size - 1) s[i++] = (char)c;
        if (c == '\n' || i >= size - 1) { s[i] = 0; return s; }
    }
}

/* string extras
 */

char* strpbrk(const char* s, const char* accept) {
    for (; *s; s++)
        for (const char* a = accept; *a; a++)
            if (*s == *a) return (char*)s;
    return 0;
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    for (; *s; s++) {
        int found = 0;
        for (const char* a = accept; *a; a++) if (*s == *a) { found = 1; break; }
        if (!found) break;
        n++;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    for (; *s; s++) {
        for (const char* a = reject; *a; a++) if (*s == *a) return n;
        n++;
    }
    return n;
}

static char* strtok_save = 0;

char* strtok(char* s, const char* delim) {
    if (!s) {
        s = strtok_save;
        if (!s) return 0;
    }
    s += strspn(s, delim);
    if (!*s) { strtok_save = 0; return 0; }
    char* end = s + strcspn(s, delim);
    if (*end) { *end = 0; strtok_save = end + 1; }
    else strtok_save = 0;
    return s;
}

char* strdup(const char* s) {
    if (!s) return 0;
    uint32_t n = (uint32_t)strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (uint8_t)c) return (void*)(p + i);
    return 0;
}

char* strerror(int errnum) {
    static char msgbuf[8];
    switch (errnum) {
    case 0:  return "no error";
    case 2:  return "no such file or directory";
    case 4:  return "too many open files";
    case 9:  return "bad file descriptor";
    case 12: return "out of memory";
    case 13: return "permission denied";
    case 22: return "invalid argument";
    case 28: return "no space left on device";
    default:
        msgbuf[0] = 'e';
        msgbuf[1] = 'r';
        msgbuf[2] = 'r';
        msgbuf[3] = 'n';
        msgbuf[4] = 'o';
        msgbuf[5] = (char)('0' + (errnum / 10) % 10);
        msgbuf[6] = (char)('0' + errnum % 10);
        msgbuf[7] = 0;
        return msgbuf;
    }
}

/* strtol family
 */

static unsigned long long strto_ull(const char* nptr, char** endptr,
                                   int base, int* neg_out) {
    const char* s = nptr;
    int neg = 0;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X') &&
        ((s[2] >= '0' && s[2] <= '9') || (s[2] >= 'a' && s[2] <= 'f') ||
         (s[2] >= 'A' && s[2] <= 'F'))) {
        base = 16; s += 2;
    } else if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) { if (endptr) *endptr = (char*)nptr; *neg_out = 0; return 0; }
    unsigned long long v = 0;
    const char* start = s;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (endptr) *endptr = (char*)(s == start ? nptr : s);
    *neg_out = neg;
    return v;
}

long strtol(const char* nptr, char** endptr, int base) {
    int neg;
    unsigned long long v = strto_ull(nptr, endptr, base, &neg);
    if (neg) return (long)-(unsigned long long)(v > 0x80000000ULL ? 0x80000000ULL : v);
    return (long)(v > 0x7FFFFFFFULL ? 0x7FFFFFFFULL : v);
}

unsigned long strtoul(const char* nptr, char** endptr, int base) {
    int neg;
    unsigned long long v = strto_ull(nptr, endptr, base, &neg);
    return neg ? (unsigned long)(0UL - v) : (unsigned long)v;
}

long long strtoll(const char* nptr, char** endptr, int base) {
    int neg;
    unsigned long long v = strto_ull(nptr, endptr, base, &neg);
    if (neg) return (long long)(v > 0x8000000000000000ULL ? 0x8000000000000000ULL : -v);
    return (long long)(v > 0x7FFFFFFFFFFFFFFFULL ? 0x7FFFFFFFFFFFFFFFULL : v);
}

unsigned long long strtoull(const char* nptr, char** endptr, int base) {
    int neg;
    unsigned long long v = strto_ull(nptr, endptr, base, &neg);
    return neg ? 0ULL - v : v;
}

double strtod(const char* nptr, char** endptr) {
    const char* s = nptr;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    double v = 0;
    const char* start = s;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * scale; scale *= 0.1; s++; }
    }
    if (s != start && (*s == 'e' || *s == 'E')) {
        const char* esave = s;
        s++;
        int eneg = 0;
        if (*s == '+') s++;
        else if (*s == '-') { eneg = 1; s++; }
        if (*s >= '0' && *s <= '9') {
            int ev = 0;
            while (*s >= '0' && *s <= '9') { ev = ev * 10 + (*s - '0'); s++; }
            if (ev > 300) ev = 300;
            double m = 1.0;
            for (int i = 0; i < ev; i++) m *= 10.0;
            if (eneg) v /= m; else v *= m;
        } else {
            s = esave;
        }
    }
    if (endptr) *endptr = (char*)(s == start ? nptr : s);
    return neg ? -v : v;
}

float strtof(const char* nptr, char** endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char* nptr, char** endptr) {
    return (long double)strtod(nptr, endptr);
}

double atof(const char* s) { return strtod(s, 0); }

/* qsort / bsearch
 */

static void memswap(void* a, void* b, size_t n) {
    uint8_t* x = (uint8_t*)a;
    uint8_t* y = (uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        uint8_t t = x[i]; x[i] = y[i]; y[i] = t;
    }
}

void qsort(void* base, size_t nmemb, size_t size,
           int (*cmp)(const void*, const void*)) {
    if (nmemb < 2 || size == 0) return;
    uint8_t* b = (uint8_t*)base;
    /* insertion sort for small ranges */
    if (nmemb < 8) {
        for (size_t i = 1; i < nmemb; i++)
            for (size_t j = i; j > 0 &&
                 cmp(b + (j - 1) * size, b + j * size) > 0; j--)
                memswap(b + (j - 1) * size, b + j * size, size);
        return;
    }
    /* quicksort, middle pivot */
    size_t mid = nmemb / 2;
    size_t i = 0, j = nmemb - 1;
    for (;;) {
        while (cmp(b + i * size, b + mid * size) < 0) i++;
        while (cmp(b + j * size, b + mid * size) > 0) j--;
        if (i >= j) break;
        memswap(b + i * size, b + j * size, size);
        if (mid == i) mid = j;
        else if (mid == j) mid = i;
        i++; if (j) j--;
    }
    qsort(b, j + 1, size, cmp);
    qsort(b + (j + 1) * size, nmemb - j - 1, size, cmp);
}

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*cmp)(const void*, const void*)) {
    const uint8_t* b = (const uint8_t*)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, b + mid * size);
        if (r == 0) return (void*)(b + mid * size);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

/* environment / system
 */

char* getenv(const char* name) { return (char*)get_env(name); }

int setenv(const char* name, const char* value, int overwrite) {
    (void)overwrite;
    set_env(name, value);
    return 0;
}

int system(const char* command) {
    if (!command) return -1;
    execute_command(command);
    return 0;
}

/* time (monotonic since boot; no RTC)
 */

time_t time(time_t* t) {
    time_t r = (time_t)(get_uptime_ms() / 1000u);
    if (t) *t = r;
    return r;
}

clock_t clock(void) {
    return (clock_t)(get_uptime_ms() / 1000u * 1000000u);
}

double difftime(time_t a, time_t b) { return (double)a - (double)b; }

int gettimeofday(struct timeval* tv, struct timezone* tz) {
    uint32_t ms = get_uptime_ms();
    if (tv) {
        tv->tv_sec = (long)(ms / 1000u);
        tv->tv_usec = (long)((ms % 1000u) * 1000u);
    }
    if (tz) { tz->tz_minuteswest = 0; tz->tz_dsttime = 0; }
    return 0;
}

/* days -> (y, m, d) civil calendar (Howard Hinnant's algorithm) */
static void civil_from_days(long z, int* y, int* m, int* d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    unsigned long dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned long mm = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

static struct tm tm_buf;

static struct tm* tm_fill(const time_t* t) {
    long secs = (long)*t;
    long days = secs / 86400;
    long rem = secs % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    tm_buf.tm_sec = (int)(rem % 60);
    tm_buf.tm_min = (int)((rem / 60) % 60);
    tm_buf.tm_hour = (int)(rem / 3600);
    tm_buf.tm_mday = d;
    tm_buf.tm_mon = m - 1;
    tm_buf.tm_year = y - 1900;
    tm_buf.tm_wday = (int)((days + 4) % 7);
    long jan1 = days - (rem / 86400) * 0;
    (void)jan1;
    tm_buf.tm_yday = 0;   /* cheap: not tracked precisely */
    tm_buf.tm_isdst = 0;
    return &tm_buf;
}

struct tm* localtime(const time_t* t) { return t ? tm_fill(t) : 0; }
struct tm* gmtime(const time_t* t) { return t ? tm_fill(t) : 0; }

/* signals (stubs: TanjaOS has no signal delivery)
 */

static sighandler_t sig_handlers[32];

sighandler_t signal(int signum, sighandler_t handler) {
    if (signum < 0 || signum >= 32) return (sighandler_t)-1;
    sighandler_t old = sig_handlers[signum];
    sig_handlers[signum] = handler;
    return old;
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* old) {
    (void)signum; (void)act; (void)old;
    return 0;
}

int sigemptyset(sigset_t* set) { if (set) set->val = 0; return 0; }
int sigfillset(sigset_t* set) { if (set) set->val = 0xFFFFFFFFu; return 0; }
int sigaddset(sigset_t* set, int signum) {
    if (set && signum >= 0 && signum < 32) set->val |= (1u << signum);
    return 0;
}
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    (void)how; (void)set; (void)oldset;
    return 0;
}

int raise(int signum) {
    /* no delivery mechanism; a raised signal terminates the program */
    print("raise: unhandled signal ");
    extern int printf(const char* fmt, ...);
    printf("%d\n", signum);
    extern void exit(int);
    exit(128 + signum);
    return 0;
}

/* setjmp / longjmp
 */

/* setjmp/longjmp live in kernel/setjmp_asm.asm (the GCC builtins are
 * too fragile across compilation units); exported via elf.c. */

/* math library (x87 FPU)
 */

double sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
double fabs(double x) { double r; __asm__("fabs" : "=t"(r) : "0"(x)); return r; }

double sin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
double cos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
double tan(double x) {
    double one, r;
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"(x));
    return r;
}

double atan(double x) {
    double r;
    __asm__("fld1\n\tfpatan" : "=t"(r) : "0"(x));
    return r;
}

double atan2(double y, double x) {
    double r;
    __asm__("fpatan" : "=t"(r) : "u"(y), "0"(x));
    return r;
}

double fmod(double x, double y) {
    double r;
    short sw;
    __asm__ volatile(
        "1: fprem\n\t"
        "fnstsw %%ax\n\t"
        "testw $0x400, %%ax\n\t"
        "jne 1b"
        : "=t"(r), "=a"(sw)
        : "0"(x), "u"(y));
    return r;
}

double ldexp(double x, int e) {
    double r;
    __asm__ volatile(
        "fildl %1\n\t"
        "fldl %2\n\t"
        "fscale\n\t"
        "fstpl %0\n\t"
        "fstp %%st(0)"
        : "=m"(r)
        : "m"(e), "m"(x));
    return r;
}

double floor(double x) {
    double r;
    unsigned short cws, cwd;
    __asm__ volatile("fnstcw %0" : "=m"(cws));
    cwd = (cws & 0xF3FFu) | 0x0400u;   /* round down */
    __asm__ volatile("fldcw %0" :: "m"(cwd));
    __asm__ volatile("fldl %1\n\tfrndint\n\tfstpl %0" : "=m"(r) : "m"(x));
    __asm__ volatile("fldcw %0" :: "m"(cws));
    return r;
}

double ceil(double x) { return -floor(-x); }

static double x87_f2xm1(double f) {
    double r;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    return r;
}

static double exp2_split(double t) {
    /* 2^t via integer + fraction split */
    double ti = floor(t);
    double f = t - ti;
    if (ti > 1023.0) ti = 1023.0;
    if (ti < -1074.0) ti = -1074.0;
    return ldexp(x87_f2xm1(f) + 1.0, (int)ti);
}

double exp(double x) {
    if (x != x) return x;
    return exp2_split(x * 1.4426950408889634074);
}

double exp2(double x) { return exp2_split(x); }

double log2(double x) {
    if (x <= 0) return (x == 0) ? -1e308 * 10 : (0.0 / 0.0);
    double r;
    __asm__ volatile("fld1\n\tfldl %1\n\tfyl2x\n\tfstpl %0"
                     : "=m"(r) : "m"(x));
    return r;
}

double log(double x) {
    if (x <= 0) return (x == 0) ? -1e308 * 10 : (0.0 / 0.0);
    double r;
    __asm__ volatile("fldln2\n\tfldl %1\n\tfyl2x\n\tfstpl %0"
                     : "=m"(r) : "m"(x));
    return r;
}

double log10(double x) {
    if (x <= 0) return (x == 0) ? -1e308 * 10 : (0.0 / 0.0);
    double r;
    __asm__ volatile("fldlg2\n\tfldl %1\n\tfyl2x\n\tfstpl %0"
                     : "=m"(r) : "m"(x));
    return r;
}

double pow(double x, double y) {
    if (x == 0.0) return y > 0 ? 0.0 : (0.0 / 0.0);
    if (x < 0) {
        /* integer exponent: defined */
        double yi = floor(y);
        if (yi == y) {
            double v = exp2_split(yi * log2(-x));
            long long n = (long long)fmod(yi, 2);
            if (n != 0) v = -v;
            return v;
        }
        return 0.0 / 0.0;
    }
    return exp2_split(y * log2(x));
}

double cbrt(double x) {
    if (x == 0) return 0;
    int neg = x < 0;
    if (neg) x = -x;
    double v = exp2_split(log2(x) / 3.0);
    return neg ? -v : v;
}

double expm1(double x) { return exp(x) - 1.0; }
double log1p(double x) { return x > -1 ? log(1 + x) : (0.0 / 0.0); }

double asin(double x) {
    if (x < -1 || x > 1) return 0.0 / 0.0;
    return atan2(x, sqrt(1 - x * x));
}

double acos(double x) {
    if (x < -1 || x > 1) return 0.0 / 0.0;
    return atan2(sqrt(1 - x * x), x);
}

double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) { double e = exp(2 * x); return (e - 1) / (e + 1); }

double asinh(double x) { return log(x + sqrt(x * x + 1)); }
double acosh(double x) { return x >= 1 ? log(x + sqrt(x * x - 1)) : (0.0/0.0); }
double atanh(double x) { return (x > -1 && x < 1) ? 0.5 * log((1 + x) / (1 - x)) : (0.0/0.0); }

double hypot(double x, double y) { return sqrt(x * x + y * y); }

double trunc(double x) { return x < 0 ? -floor(-x) : floor(x); }
double round(double x) {
    double r = floor(fabs(x) + 0.5);
    return x < 0 ? -r : r;
}

double rint(double x) {
    double r;
    __asm__ volatile("fldl %1\n\tfrndint\n\tfstpl %0" : "=m"(r) : "m"(x));
    return r;
}
double nearbyint(double x) { return rint(x); }

long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }
long lrint(double x) { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }

double copysign(double x, double y) {
    union { double d; uint64_t u; } ux, uy;
    ux.d = x; uy.d = y;
    ux.u = (ux.u & 0x7FFFFFFFFFFFFFFFULL) | (uy.u & 0x8000000000000000ULL);
    return ux.d;
}

double fdim(double x, double y) { return x > y ? x - y : 0; }
double fmax(double x, double y) { return x > y ? x : y; }
double fmin(double x, double y) { return x < y ? x : y; }

double modf(double x, double* iptr) {
    double i = (x < 0) ? ceil(x) : floor(x);
    *iptr = i;
    return x - i;
}

double frexp(double x, int* e) {
    if (x == 0) { *e = 0; return 0; }
    int ee = (int)floor(log2(fabs(x)));
    double m = x / ldexp(1.0, ee);
    while (fabs(m) >= 1.0) { m /= 2.0; ee++; }
    while (fabs(m) < 0.5) { m *= 2.0; ee--; }
    *e = ee;
    return m;
}

double scalbn(double x, int e) { return ldexp(x, e); }
double scalbln(double x, int e) { return ldexp(x, e); }

double nextafter(double x, double y) {
    union { double d; uint64_t u; } ux, uy;
    ux.d = x; uy.d = y;
    if (x == y) return y;
    if (x != x || y != y) return 0.0 / 0.0;
    if (x == 0) { ux.u = 1; return y > 0 ? ux.d : -ux.d; }
    if ((x > 0 && y > x) || (x < 0 && y < x)) {
        ux.u++;
    } else {
        ux.u--;
    }
    return ux.d;
}

int ilogb(double x) {
    if (x == 0) return -2147483647;
    if (x != x) return -2147483647;
    return (int)floor(log2(fabs(x)));
}

double logb(double x) {
    if (x == 0) return -1e308 * 10;
    return (double)floor(log2(fabs(x)));
}

double remainder(double x, double y) {
    double q = round(x / y);
    return x - q * y;
}

double remquo(double x, double y, int* quo) {
    double r = remainder(x, y);
    if (quo) {
        double q = round(x / y);
        *quo = ((int)q >= 0 ? 1 : -1) * (int)fmod(fabs(q), 8.0);
    }
    return r;
}

double fma(double x, double y, double z) {
    double r;
    __asm__ volatile("fldl %1\n\tfldl %2\n\tfmulp\n\tfaddl %3\n\tfstpl %0"
                     : "=m"(r) : "m"(x), "m"(y), "m"(z));
    return r;
}

double erf(double x) {
    /* Abramowitz & Stegun 7.1.26 (|err| <= 1.5e-7) */
    int neg = x < 0;
    if (neg) x = -x;
    double t = 1.0 / (1.0 + 0.3275911 * x);
    double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t)
                 + 1.421413741) * t - 0.284496736) * t + 0.254829592)
 * t * exp(-x * x);
    return neg ? -y : y;
}

double erfc(double x) { return 1.0 - erf(x); }

double tgamma(double x) {
    /* Lanczos approximation, g=7, n=9 */
    static const double g[9] = {
        0.99999999999980993,
        676.5203681218851,
        -1259.1392167224028,
        771.32342877765313,
        -176.61502916214059,
        12.507343278686905,
        -0.13857109526572012,
        9.9843695780195716e-6,
        1.5056327351493116e-7
    };
    if (x == floor(x) && x <= 0) return 0.0 / 0.0;   /* poles */
    if (x < 0.5) {
        /* reflection formula */
        return 3.14159265358979323846 /
               (sin(3.14159265358979323846 * x) * tgamma(1 - x));
    }
    x -= 1;
    double a = g[0];
    double t = x + 7.5;
    for (int i = 1; i < 9; i++) a += g[i] / (x + (double)i);
    return sqrt(2 * 3.14159265358979323846) *
           pow(t, x + 0.5) * exp(-t) * a;
}

double lgamma(double x) {
    double v = tgamma(x);
    return v < 0 ? log(-v) : log(v);
}

/* float / long double variants */

float sqrtf(float x) { return (float)sqrt((double)x); }
long double sqrtl(long double x) { return sqrt((double)x); }
float fabsf(float x) { return (float)fabs((double)x); }
long double fabsl(long double x) { return fabs((double)x); }
float sinf(float x) { return (float)sin((double)x); }
long double sinl(long double x) { return sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
long double cosl(long double x) { return cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
long double tanl(long double x) { return tan((double)x); }
float asinf(float x) { return (float)asin((double)x); }
long double asinl(long double x) { return asin((double)x); }
float acosf(float x) { return (float)acos((double)x); }
long double acosl(long double x) { return acos((double)x); }
float atanf(float x) { return (float)atan((double)x); }
long double atanl(long double x) { return atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
long double atan2l(long double y, long double x) { return atan2((double)y, (double)x); }
float sinhf(float x) { return (float)sinh((double)x); }
long double sinhl(long double x) { return sinh((double)x); }
float coshf(float x) { return (float)cosh((double)x); }
long double coshl(long double x) { return cosh((double)x); }
float tanhf(float x) { return (float)tanh((double)x); }
long double tanhl(long double x) { return tanh((double)x); }
float asinhf(float x) { return (float)asinh((double)x); }
long double asinhl(long double x) { return asinh((double)x); }
float acoshf(float x) { return (float)acosh((double)x); }
long double acoshl(long double x) { return acosh((double)x); }
float atanhf(float x) { return (float)atanh((double)x); }
long double atanhl(long double x) { return atanh((double)x); }
float expf(float x) { return (float)exp((double)x); }
long double expl(long double x) { return exp((double)x); }
float logf(float x) { return (float)log((double)x); }
long double logl(long double x) { return log((double)x); }
float log10f(float x) { return (float)log10((double)x); }
long double log10l(long double x) { return log10((double)x); }
float log2f(float x) { return (float)log2((double)x); }
long double log2l(long double x) { return log2((double)x); }
float log1pf(float x) { return (float)log1p((double)x); }
long double log1pl(long double x) { return log1p((double)x); }
float exp2f(float x) { return (float)exp2((double)x); }
long double exp2l(long double x) { return exp2((double)x); }
float expm1f(float x) { return (float)expm1((double)x); }
long double expm1l(long double x) { return expm1((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
long double powl(long double x, long double y) { return pow((double)x, (double)y); }
float floorf(float x) { return (float)floor((double)x); }
long double floorl(long double x) { return floor((double)x); }
float ceilf(float x) { return (float)ceil((double)x); }
long double ceill(long double x) { return ceil((double)x); }
float truncf(float x) { return (float)trunc((double)x); }
long double truncl(long double x) { return trunc((double)x); }
float roundf(float x) { return (float)round((double)x); }
long double roundl(long double x) { return round((double)x); }
float rintf(float x) { return (float)rint((double)x); }
long double rintl(long double x) { return rint((double)x); }
float nearbyintf(float x) { return (float)nearbyint((double)x); }
long double nearbyintl(long double x) { return nearbyint((double)x); }
long lroundf(float x) { return lround((double)x); }
long long llroundf(float x) { return llround((double)x); }
long lrintf(float x) { return lrint((double)x); }
long long llrintf(float x) { return llrint((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
long double fmodl(long double x, long double y) { return fmod((double)x, (double)y); }
float remainderf(float x, float y) { return (float)remainder((double)x, (double)y); }
long double remainderl(long double x, long double y) { return remainder((double)x, (double)y); }
float modff(float x, float* ip) { double d; float r = (float)modf((double)x, &d); *ip = (float)d; return r; }
long double modfl(long double x, long double* ip) { double d; long double r = modf((double)x, &d); *ip = (long double)d; return r; }
float frexpf(float x, int* e) { return (float)frexp((double)x, e); }
long double frexpl(long double x, int* e) { return frexp((double)x, e); }
float ldexpf(float x, int e) { return (float)ldexp((double)x, e); }
float scalbnf(float x, int e) { return (float)ldexp((double)x, e); }
long double scalbnl(long double x, int e) { return ldexp((double)x, e); }
float scalblnf(float x, int e) { return (float)ldexp((double)x, e); }
long double scalblnl(long double x, int e) { return ldexp((double)x, e); }
float cbrtf(float x) { return (float)cbrt((double)x); }
long double cbrtl(long double x) { return cbrt((double)x); }
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
long double hypotl(long double x, long double y) { return hypot((double)x, (double)y); }
float copysignf(float x, float y) { return (float)copysign((double)x, (double)y); }
long double copysignl(long double x, long double y) { return copysign((double)x, (double)y); }
float fdimf(float x, float y) { return (float)fdim((double)x, (double)y); }
long double fdiml(long double x, long double y) { return fdim((double)x, (double)y); }
float fmaxf(float x, float y) { return (float)fmax((double)x, (double)y); }
long double fmaxl(long double x, long double y) { return fmax((double)x, (double)y); }
float fminf(float x, float y) { return (float)fmin((double)x, (double)y); }
long double fminl(long double x, long double y) { return fmin((double)x, (double)y); }
float nextafterf(float x, float y) { return (float)nextafter((double)x, (double)y); }
long double nextafterl(long double x, long double y) { return nextafter((double)x, (double)y); }
float ilogbf(float x) { return (float)ilogb((double)x); }
float logbf(float x) { return (float)logb((double)x); }
float erff(float x) { return (float)erf((double)x); }
long double erfl(long double x) { return erf((double)x); }
float erfcf(float x) { return (float)erfc((double)x); }
long double erfcl(long double x) { return erfc((double)x); }
float tgammaf(float x) { return (float)tgamma((double)x); }
long double tgammal(long double x) { return tgamma((double)x); }
float lgammaf(float x) { return (float)lgamma((double)x); }
long double lgammal(long double x) { return lgamma((double)x); }
float fmaf(float x, float y, float z) { return (float)fma((double)x, (double)y, (double)z); }
float remquof(float x, float y, int* q) { return (float)remquo((double)x, (double)y, q); }
long double remquol(long double x, long double y, int* q) { return remquo((double)x, (double)y, q); }
double nexttoward(double x, long double y) { return nextafter(x, (double)y); }
float nexttowardf(float x, long double y) { return (float)nextafter((double)x, (double)y); }
long double nexttowardl(long double x, long double y) { return nextafter((double)x, (double)y); }
long double fmal(long double x, long double y, long double z) { return fma((double)x, (double)y, (double)z); }

/* program argv
 */

int tanja_argc = 1;
static char* tanja_argv_store[64];
char** tanja_argv = tanja_argv_store;
static char tanja_args_buf[1024];

void tanja_setargs(const char* path, const char* args) {
    tanja_argv_store[0] = (char*)path;
    int argc = 1;
    tanja_args_buf[0] = 0;
    int n = 0;
    if (args) {
        const char* s = args;
        while (*s == ' ') s++;
        while (*s && argc < 63) {
            int start = n;
            while (*s && *s != ' ' && n < (int)sizeof tanja_args_buf - 1)
                tanja_args_buf[n++] = *s++;
            if (n > start) {
                tanja_args_buf[n++] = 0;
                tanja_argv_store[argc++] = &tanja_args_buf[start];
            }
            while (*s == ' ') s++;
        }
    }
    tanja_argv_store[argc] = 0;
    tanja_argc = argc;
}

long double lroundl(long double x) { return lround((double)x); }
long double llroundl(long double x) { return llround((double)x); }
long double lrintl(long double x) { return lrint((double)x); }
long double llrintl(long double x) { return llrint((double)x); }
long double ldexpl(long double x, int e) { return ldexp((double)x, e); }
int ilogbl(long double x) { return ilogb((double)x); }
long double logbl(long double x) { return logb((double)x); }

/* stat(2)
 */

int stat(const char* path, struct stat* st)
{
    if (!path || !st) { errno = 22; return -1; }
    memset(st, 0, sizeof *st);
    if (fs_directory_exists(path)) {
        st->st_mode = 0040755;      /* S_IFDIR | 0755 */
        st->st_mtime = st->st_atime = st->st_ctime = time(0);
        return 0;
    }
    if (!fs_file_exists(path)) { errno = 2; return -1; }
    st->st_mode = 0100644;          /* S_IFREG | 0644 */
    st->st_size = (off_t)fs_get_file_size(path);
    st->st_mtime = st->st_atime = st->st_ctime = time(0);
    return 0;
}

int fstat(int fd, struct stat* st)
{
    if (fd < 0 || fd >= TANJA_MAX_FDS || !fdt[fd].used || !st) {
        errno = 9; return -1;
    }
    memset(st, 0, sizeof *st);
    if (fdt[fd].is_console) {
        st->st_mode = 0020666;      /* S_IFCHR | 0666 */
        return 0;
    }
    st->st_mode = 0100644;
    st->st_size = (off_t)fdt[fd].size;
    st->st_mtime = st->st_atime = st->st_ctime = time(0);
    return 0;
}

/* stdio extras
 */

void perror(const char* s)
{
    if (s && s[0]) {
        print_n(s, strlen(s));
        print(": ");
    }
    print(strerror(errno));
    print("\n");
}

void rewind(FILE_T* f)
{
    if (!f) return;
    fseek(f, 0, 0);
    f->error = 0;
}

void clearerr(FILE_T* f)
{
    if (!f) return;
    f->eof = 0;
    f->error = 0;
}

/* TanjaOS FILE streams are fully buffered by design; the buffers
 * are owned by the fd table, so setvbuf/setbuf accept and ignore. */
int setvbuf(FILE_T* f, char* buf, int mode, size_t size)
{
    (void)f; (void)buf; (void)mode; (void)size;
    return 0;
}

void setbuf(FILE_T* f, char* buf)
{
    setvbuf(f, buf, buf ? 0 : 1, 0);
}

/* locale (always the "C" locale)
 */

static const char tanja_loc_C[] = "C";

char* setlocale(int category, const char* locale)
{
    if (locale == 0) return (char*)tanja_loc_C;
    if (locale[0] == 'C' && locale[1] == 0) return (char*)tanja_loc_C;
    if (locale[0] == 0) return (char*)tanja_loc_C;  /* query only */
    (void)category;
    return 0;                                       /* unsupported locale */
}

/* time extras
 */

static const char wdays[7][4] = {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
};
static const char months[12][4] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int mdays(int y, int m)
{
    static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 1 && is_leap(y)) return 29;
    if (m < 0 || m > 11) return 30;
    return d[m];
}

time_t mktime(struct tm* tm)
{
    if (!tm) return (time_t)-1;
    long days = 0;
    int y = tm->tm_year + 1900;
    if (y < 1970) return (time_t)-1;
    for (int yy = 1970; yy < y; yy++)
        days += is_leap(yy) ? 366 : 365;
    for (int mm = 0; mm < tm->tm_mon; mm++)
        days += mdays(y, mm);
    days += tm->tm_mday - 1;
    /* C standard: mktime normalizes tm_wday and tm_yday */
    tm->tm_wday = (int)((4 + days) % 7);
    {
        int yd = tm->tm_mday - 1;
        for (int mm = 0; mm < tm->tm_mon; mm++)
            yd += mdays(y, mm);
        tm->tm_yday = yd;
    }
    return (time_t)(days * 86400L +
                    tm->tm_hour * 3600L +
                    tm->tm_min * 60L + tm->tm_sec);
}

char* asctime(const struct tm* tm)
{
    /* "Www Mmm dd hh:mm:ss yyyy\n" + NUL = 26 bytes */
    static char buf[26];
    if (!tm) return 0;
    buf[0] = 0;
    char* p = buf;
    const char* w = wdays[tm->tm_wday & 7];
    const char* m = months[tm->tm_mon % 12];
    int day = tm->tm_mday, hour = tm->tm_hour, min = tm->tm_min,
        sec = tm->tm_sec, year = 1900 + tm->tm_year;
    *p++ = w[0]; *p++ = w[1]; *p++ = w[2]; *p++ = ' ';
    *p++ = m[0]; *p++ = m[1]; *p++ = m[2]; *p++ = ' ';
    *p++ = '0' + (day / 10) % 10; *p++ = '0' + day % 10; *p++ = ' ';
    *p++ = '0' + (hour / 10) % 10; *p++ = '0' + hour % 10; *p++ = ':';
    *p++ = '0' + (min / 10) % 10; *p++ = '0' + min % 10; *p++ = ':';
    *p++ = '0' + (sec / 10) % 10; *p++ = '0' + sec % 10; *p++ = ' ';
    *p++ = '0' + (year / 1000) % 10; *p++ = '0' + (year / 100) % 10;
    *p++ = '0' + (year / 10) % 10; *p++ = '0' + year % 10;
    *p++ = '\n';
    *p = 0;
    return buf;
}

char* ctime(const time_t* t)
{
    if (!t) return 0;
    time_t v = *t;
    return asctime(localtime(&v));
}

size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm)
{
    if (!s || !fmt || !tm || max == 0) return 0;
    size_t n = 0;
#define PUTC(c) do { if (n + 1 >= max) return 0; s[n++] = (c); } while (0)
#define PUTS(str) do { \
        const char* q_ = (str); \
        while (*q_) PUTC(*q_++); \
    } while (0)
#define PUT2(v) do { int x_ = (v); PUTC('0' + (x_ / 10) % 10); PUTC('0' + x_ % 10); } while (0)
#define PUTI(v) do { \
        int x_ = (v), i_ = 0; char nb_[16]; \
        if (x_ == 0) nb_[i_++] = '0'; \
        while (x_ > 0 && i_ < 10) { nb_[i_++] = '0' + x_ % 10; x_ /= 10; } \
        while (i_ > 0) PUTC(nb_[--i_]); \
    } while (0)
    for (const char* f = fmt; *f; f++) {
        if (*f != '%') { PUTC(*f); continue; }
        f++;
        switch (*f) {
        case 'Y': PUTI(1900 + tm->tm_year); break;
        case 'y': PUT2((1900 + tm->tm_year) % 100); break;
        case 'm': PUT2(tm->tm_mon + 1); break;
        case 'd': PUT2(tm->tm_mday); break;
        case 'e': PUTC(' '); PUT2(tm->tm_mday); break;
        case 'H': PUT2(tm->tm_hour); break;
        case 'I': PUT2(tm->tm_hour % 12 == 0 ? 12 : tm->tm_hour % 12); break;
        case 'M': PUT2(tm->tm_min); break;
        case 'S': PUT2(tm->tm_sec); break;
        case 'j': PUTI(tm->tm_yday + 1); break;
        case 'w': PUTI(tm->tm_wday); break;
        case 'a': PUTS(wdays[tm->tm_wday & 7]); break;
        case 'A': PUTS(wdays[tm->tm_wday & 7]); break;
        case 'b': case 'h': PUTS(months[tm->tm_mon % 12]); break;
        case 'B': PUTS(months[tm->tm_mon % 12]); break;
        case 'p': PUTS(tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'n': PUTC('\n'); break;
        case 't': PUTC('\t'); break;
        case '%': PUTC('%'); break;
        case 'F': PUTI(1900 + tm->tm_year); PUTC('-'); PUT2(tm->tm_mon + 1); PUTC('-'); PUT2(tm->tm_mday); break;
        case 'T': case 'X': PUT2(tm->tm_hour); PUTC(':'); PUT2(tm->tm_min); PUTC(':'); PUT2(tm->tm_sec); break;
        case 'D': PUT2(tm->tm_mon + 1); PUTC('/'); PUT2(tm->tm_mday); PUTC('/'); PUT2((1900 + tm->tm_year) % 100); break;
        case 'R': PUT2(tm->tm_hour); PUTC(':'); PUT2(tm->tm_min); break;
        case 'c': PUTS(wdays[tm->tm_wday & 7]); PUTC(' '); PUTS(months[tm->tm_mon % 12]);
                  PUTC(' '); PUT2(tm->tm_mday); PUTC(' '); PUT2(tm->tm_hour); PUTC(':');
                  PUT2(tm->tm_min); PUTC(':'); PUT2(tm->tm_sec); PUTC(' '); PUTI(1900 + tm->tm_year); break;
        default: PUTC('%'); PUTC(*f); break;
        }
    }
    s[n] = 0;
    return n;
#undef PUTC
#undef PUTS
#undef PUT2
#undef PUTI
}

/* stdlib / string extras
 */

long labs(long n) { return n < 0 ? -n : n; }
long long llabs(long long n) { return n < 0 ? -n : n; }

void* aligned_alloc(size_t alignment, size_t size)
{
    /* malloc already returns 8-aligned; anything up to 16 we can
 * satisfy by over-allocating and returning an aligned slice,
 * storing the base offset just before the returned pointer. */
    if (alignment <= 8) return malloc(size);
    if (alignment > 4096 || (alignment & (alignment - 1)) != 0) {
        errno = 22;
        return 0;
    }
    size_t extra = alignment + sizeof(size_t);
    char* raw = (char*)malloc(size + extra);
    if (!raw) return 0;
    uintptr_t p = ((uintptr_t)raw + sizeof(size_t) + alignment - 1) &
                  ~(uintptr_t)(alignment - 1);
    ((size_t*)p)[-1] = (size_t)(p - (uintptr_t)raw);
    return (void*)p;
}

char* strndup(const char* s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len]) len++;
    char* r = (char*)malloc(len + 1);
    if (!r) { errno = 12; return 0; }
    memcpy(r, s, len);
    r[len] = 0;
    return r;
}

char* strtok_r(char* s, const char* delim, char** saveptr)
{
    if (!delim || !saveptr) return 0;
    char* p = s ? s : *saveptr;
    if (!p) return 0;
    while (*p && strchr(delim, *p)) p++;
    if (!*p) { *saveptr = p; return 0; }
    char* start = p;
    while (*p && !strchr(delim, *p)) p++;
    if (*p) { *p = 0; p++; }
    *saveptr = p;
    return start;
}

int isblank(int c)
{
    return c == ' ' || c == '\t';
}

struct lconv* localeconv(void)
{
    /* The "C" locale: dot decimals, no grouping, no currency. */
    static struct lconv lc;
    static char dot[2] = ".";
    static char empty[1] = "";
    static char minus[2] = "-";
    static char int_curr[4] = "";
    lc.decimal_point = dot;
    lc.thousands_sep = empty;
    lc.grouping = empty;
    lc.int_curr_symbol = int_curr;
    lc.currency_symbol = empty;
    lc.mon_decimal_point = dot;
    lc.mon_thousands_sep = empty;
    lc.mon_grouping = empty;
    lc.positive_sign = empty;
    lc.negative_sign = minus;
    lc.int_frac_digits = 127;   /* CHAR_MAX = unavailable */
    lc.frac_digits = 127;
    lc.p_cs_precedes = 127; lc.p_sep_by_space = 127;
    lc.n_cs_precedes = 127; lc.n_sep_by_space = 127;
    lc.p_sign_posn = 127; lc.n_sign_posn = 127;
    return &lc;
}

/* fenv (x87 environment; user programs start with a clean state)
 */

/* read the live x87 control/status words */
static void fenv_now(unsigned short* cw, unsigned short* sw)
{
    __asm__ volatile("fnstcw %0" : "=m"(*cw));
    __asm__ volatile("fnstsw %0" : "=m"(*sw));
}

int feclearexcept(int excepts)
{
    unsigned short sw;
    (void)excepts;
    __asm__ volatile("fnclex");
    fenv_now(0, &sw);
    return 0;
}

int feraiseexcept(int excepts)
{
    /* x87 exception flags are sticky status bits; TanjaOS always
 * keeps FP exceptions MASKED, so raising just sets the bits
 * (which is what C requires) and never traps. */
    static const double one = 1.0, zero = 0.0;
    if (excepts & FE_INVALID)      /* 0.0 / 0.0 -> IE */
        __asm__ volatile("fldl %0; fldl %0; fdivp" : : "m"(zero) : "st");
    else if (excepts & FE_DIVBYZERO) /* 1.0 / 0.0 -> ZE */
        __asm__ volatile("fldl %1; fldl %0; fdivp" : : "m"(zero), "m"(one) : "st");
    return 0;
}

int fetestexcept(int excepts)
{
    unsigned short sw;
    fenv_now(0, &sw);
    return (int)sw & excepts;
}

int fegetround(void)
{
    unsigned short cw;
    fenv_now(&cw, 0);
    return (int)(cw & 0x0C00);
}

int fesetround(int mode)
{
    unsigned short cw;
    fenv_now(&cw, 0);
    cw = (unsigned short)((cw & ~0x0C00) | (mode & 0x0C00));
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return 0;
}

int fegetenv(fenv_t* envp)
{
    if (!envp) return 1;
    fenv_now(&envp->cw, &envp->sw);
    return 0;
}

int fesetenv(const fenv_t* envp)
{
    if (envp && envp->cw) {
        unsigned short cw = envp->cw;
        __asm__ volatile("fldcw %0" : : "m"(cw));
    } else {
        /* FE_DFL_ENV (0): restore the default environment */
        unsigned short cw = 0x037F;
        __asm__ volatile("fninit");
        __asm__ volatile("fldcw %0" : : "m"(cw));
    }
    return 0;
}

int feholdexcept(fenv_t* envp)
{
    if (!envp) return 1;
    fegetenv(envp);
    __asm__ volatile("fnclex");
    /* mask all exceptions */
    unsigned short cw = 0x1F7F;
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return 0;
}

int feupdateenv(const fenv_t* envp)
{
    if (!envp) return 1;
    fesetenv(envp);
    __asm__ volatile("fnclex");
    return 0;
}
