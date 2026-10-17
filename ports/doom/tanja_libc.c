/*
 * tanja_libc.c - the small slice of libc that Doom needs, built on the
 * TanjaOS kernel exports (see include/tanja.h).
 *
 * TanjaOS programs have no malloc/stdio of their own, so this file provides:
 *   - malloc/free/calloc/realloc over a static arena in .bss
 *   - FILE I/O: the IWAD is served straight out of the kernel image
 *     (tanja_wad_base/size), every other file goes through the TanjaOS
 *     filesystem (whole-file buffered, flushed on fclose)
 *   - printf family, sscanf, atof, strdup, 64-bit division helpers
 *   - exit() that unwinds back to the shell (see tanja_platform.c)
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "fs.h"

#define EOF_    (-1)
#define ENOENT_ 2
#define EEXIST_ 17

/* ---- kernel exports we use directly ------------------------------ */
extern void print_n(const char* s, uint32_t len);
extern void* memcpy(void*, const void*, size_t);
extern void* memset(void*, int, size_t);
extern void* memmove(void*, const void*, size_t);
extern size_t strlen(const char*);
extern int strcmp(const char*, const char*);
extern int strcasecmp(const char*, const char*);
extern int isspace(int);
extern int isdigit(int);
extern int isxdigit(int);
extern int tolower(int);

/* ---- provided by tanja_platform.c --------------------------------- */
extern void tanja_gfx_leave(void);
extern int  tanja_gfx_active(void);
extern void tanja_exit(int code) __attribute__((noreturn));

/* ---- provided by the kernel (exported) ----------------------------- */
extern uint8_t* tanja_wad_base;
extern uint32_t tanja_wad_size;

int errno;

/* ================================================================== */
/* malloc: first-fit free list with coalescing, static arena           */
/* ================================================================== */
#ifndef HEAP_BYTES
#define HEAP_BYTES (11u * 1024u * 1024u)
#endif

typedef struct blk {
    uint32_t size;          /* payload bytes, multiple of 16 */
    uint32_t free;
    struct blk* next;       /* address-ordered list */
    uint32_t pad;
} blk_t;                    /* 16 bytes: payload stays 16-aligned */

static uint8_t heap[HEAP_BYTES] __attribute__((aligned(16)));
static blk_t* heap_head;

static void heap_init(void)
{
    heap_head = (blk_t*)heap;
    heap_head->size = HEAP_BYTES - sizeof(blk_t);
    heap_head->free = 1;
    heap_head->next = 0;
}

void* malloc(size_t n)
{
    blk_t* b;
    if (!heap_head) heap_init();
    if (n == 0) n = 1;
    n = (n + 15u) & ~15u;
    for (b = heap_head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(blk_t) + 16) {
            blk_t* r = (blk_t*)((uint8_t*)(b + 1) + n);
            r->size = b->size - n - sizeof(blk_t);
            r->free = 1;
            r->next = b->next;
            b->size = n;
            b->next = r;
        }
        b->free = 0;
        return b + 1;
    }
    return 0;
}

void free(void* p)
{
    blk_t *b, *it;
    if (!p) return;
    b = (blk_t*)p - 1;
    b->free = 1;
    /* coalesce forward, then backward */
    while (b->next && b->next->free) {
        b->size += sizeof(blk_t) + b->next->size;
        b->next = b->next->next;
    }
    for (it = heap_head; it && it->next != b; it = it->next) ;
    if (it && it->free) {
        it->size += sizeof(blk_t) + b->size;
        it->next = b->next;
    }
}

void* calloc(size_t a, size_t b)
{
    size_t n = a * b;
    void* p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void* realloc(void* p, size_t n)
{
    blk_t* b;
    void* q;
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    b = (blk_t*)p - 1;
    if (b->size >= n) return p;
    q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

char* strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char* strerror(int e) { (void)e; return "error"; }
char* getenv(const char* n) { (void)n; return 0; }
int system(const char* c) { (void)c; return -1; }
int atexit(void (*f)(void)) { (void)f; return 0; }
double fabs(double x) { return x < 0 ? -x : x; }
long time(long* t) { if (t) *t = 0; return 0; }

void exit(int code) { tanja_exit(code); }
void abort(void) { tanja_exit(-1); }

/* ================================================================== */
/* stdio                                                               */
/* ================================================================== */
typedef struct T_FILE {
    uint8_t* buf;
    uint32_t len, cap, pos;
    char     path[MAX_PATH];
    uint8_t  writing, owns_buf, console, eof, err;
} FILE;

static FILE f_stdin  = { 0, 0, 0, 0, "", 0, 0, 0, 0, 0 };
static FILE f_stdout = { 0, 0, 0, 0, "", 1, 0, 1, 0, 0 };
static FILE f_stderr = { 0, 0, 0, 0, "", 1, 0, 2, 0, 0 };
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;

static int ends_with(const char* s, const char* suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* Console output.  Once Doom owns the screen (mode 13h) stdout is dropped
 * (nobody can see it); stderr flips back to text mode so I_Error messages
 * are readable. */
static void console_write(int which, const char* s, uint32_t n)
{
    if (tanja_gfx_active()) {
        if (which == 1) return;
        tanja_gfx_leave();
    }
    print_n(s, n);
}

FILE* fopen(const char* path, const char* mode)
{
    FILE* f;
    int w = (mode[0] == 'w' || mode[0] == 'a');

    if (!path || !path[0]) return 0;

    /* Config file persistence is not supported: behave as if absent. */
    if (ends_with(path, ".cfg")) return 0;

    f = (FILE*)calloc(1, sizeof(FILE));
    if (!f) return 0;
    {
        size_t i;
        for (i = 0; path[i] && i < MAX_PATH - 1; i++) f->path[i] = path[i];
        f->path[i] = 0;
    }

    if (w) {
        f->writing = 1;
        f->owns_buf = 1;
        f->cap = 4096;
        f->buf = (uint8_t*)malloc(f->cap);
        if (!f->buf) { free(f); return 0; }
        return f;
    }

    /* the IWAD lives in the kernel image, not in the filesystem */
    if (ends_with(path, ".wad") && tanja_wad_base && tanja_wad_size) {
        f->buf = tanja_wad_base;
        f->len = tanja_wad_size;
        return f;
    }

    {
        uint32_t sz;
        if (!fs_file_exists(path)) { errno = ENOENT_; free(f); return 0; }
        sz = fs_get_file_size(path);
        f->cap = sz + 1;
        f->buf = (uint8_t*)malloc(f->cap);
        if (!f->buf) { free(f); return 0; }
        f->owns_buf = 1;
        f->len = f->cap;
        if (fs_read_file(path, (char*)f->buf, &f->len) != 0) {
            free(f->buf); free(f); return 0;
        }
        return f;
    }
}

int fflush(FILE* f) { (void)f; return 0; }

int fclose(FILE* f)
{
    int rc = 0;
    if (!f || f->console) return 0;
    if (f->writing)
        rc = fs_write_file(f->path, (const char*)f->buf, f->len) == 0 ? 0 : EOF_;
    if (f->owns_buf) free(f->buf);
    free(f);
    return rc;
}

size_t fread(void* dst, size_t sz, size_t n, FILE* f)
{
    size_t want = sz * n, have;
    if (!f || f->console || f->writing || !sz) return 0;
    have = f->len - f->pos;
    if (want > have) { want = have; f->eof = 1; }
    memcpy(dst, f->buf + f->pos, want);
    f->pos += want;
    return want / sz;
}

size_t fwrite(const void* src, size_t sz, size_t n, FILE* f)
{
    size_t bytes = sz * n;
    if (!f || !bytes) return 0;
    if (f->console) { console_write(f->console, (const char*)src, bytes); return n; }
    if (!f->writing) return 0;
    if (f->pos + bytes > f->cap) {
        uint32_t nc = f->cap * 2;
        uint8_t* nb;
        while (nc < f->pos + bytes) nc *= 2;
        nb = (uint8_t*)realloc(f->buf, nc);
        if (!nb) { f->err = 1; return 0; }
        f->buf = nb; f->cap = nc;
    }
    memcpy(f->buf + f->pos, src, bytes);
    f->pos += bytes;
    if (f->pos > f->len) f->len = f->pos;
    return n;
}

int fseek(FILE* f, long off, int whence)
{
    long np;
    if (!f || f->console) return -1;
    np = whence == 0 ? off : whence == 1 ? (long)f->pos + off : (long)f->len + off;
    if (np < 0 || (uint32_t)np > f->len) return -1;
    f->pos = (uint32_t)np;
    f->eof = 0;
    return 0;
}

long ftell(FILE* f) { return f ? (long)f->pos : -1; }
int feof(FILE* f) { return f ? f->eof : 1; }

int fgetc(FILE* f)
{
    if (!f || f->console || f->writing) return EOF_;
    if (f->pos >= f->len) { f->eof = 1; return EOF_; }
    return f->buf[f->pos++];
}

char* fgets(char* s, int n, FILE* f)
{
    int i = 0, c;
    while (i < n - 1 && (c = fgetc(f)) != EOF_) {
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    s[i] = 0;
    return s;
}

int fputc(int c, FILE* f) { char ch = (char)c; return fwrite(&ch, 1, 1, f) ? c : EOF_; }
int putc(int c, FILE* f) { return fputc(c, f); }
int fputs(const char* s, FILE* f) { return fwrite(s, 1, strlen(s), f) ? 0 : EOF_; }

int remove(const char* path) { return fs_delete_file(path); }

int rename(const char* from, const char* to)
{
    uint32_t sz = fs_get_file_size(from);
    uint32_t cap = sz + 1;
    char* tmp = (char*)malloc(cap);
    int rc = -1;
    if (!tmp) return -1;
    if (fs_read_file(from, tmp, &cap) == 0 &&
        fs_write_file(to, tmp, cap) == 0) {
        fs_delete_file(from);
        rc = 0;
    }
    free(tmp);
    return rc;
}

int mkdir(const char* path, int mode)
{
    (void)mode;
    if (fs_directory_exists(path)) { errno = EEXIST_; return -1; }
    return fs_create_directory(path);
}

/* ================================================================== */
/* printf family                                                       */
/* ================================================================== */
typedef struct { char* buf; size_t cap, len; } out_t;

static void out_ch(out_t* o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void out_pad(out_t* o, int n, char c)
{
    while (n-- > 0) out_ch(o, c);
}

static void out_num(out_t* o, uint32_t v, int neg, int base, int upper,
                    int width, int prec, int left, int zero, const char* pfx)
{
    char tmp[34];
    int n = 0, i, total, pl = pfx ? (int)strlen(pfx) : 0;
    const char* dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dg[v % (uint32_t)base]; v /= (uint32_t)base; }
    if (prec < 0) prec = 1;
    while (n < prec) tmp[n++] = '0';
    total = n + (neg ? 1 : 0) + pl;
    if (!left && !zero) out_pad(o, width - total, ' ');
    if (neg) out_ch(o, '-');
    for (i = 0; i < pl; i++) out_ch(o, pfx[i]);
    if (!left && zero) out_pad(o, width - total, '0');
    while (n) out_ch(o, tmp[--n]);
    if (left) out_pad(o, width - total, ' ');
}

static void out_dbl(out_t* o, double v, int prec, int width, int left, int zero)
{
    char tmp[64];
    int n = 0, neg = 0, i;
    uint32_t ip;
    double frac, scale = 1.0, rnd = 0.5;

    if (prec < 0) prec = 6;
    if (v < 0) { neg = 1; v = -v; }
    for (i = 0; i < prec; i++) { scale *= 10.0; rnd /= 10.0; }
    v += rnd;                       /* round to prec digits */
    ip = (v >= 4294967295.0) ? 0xFFFFFFFFu : (uint32_t)v;
    frac = v - (double)ip;
    if (ip == 0) tmp[n++] = '0';
    while (ip) { tmp[n++] = (char)('0' + ip % 10); ip /= 10; }
    {
        int total = n + (neg ? 1 : 0) + (prec ? prec + 1 : 0);
        if (!left && !zero) out_pad(o, width - total, ' ');
        if (neg) out_ch(o, '-');
        if (!left && zero) out_pad(o, width - total, '0');
        while (n) out_ch(o, tmp[--n]);
        if (prec) {
            uint32_t fd = (uint32_t)(frac * scale);
            char ft[16];
            int k = 0;
            out_ch(o, '.');
            for (i = 0; i < prec && k < 15; i++) { ft[k++] = (char)('0' + fd % 10); fd /= 10; }
            while (prec > k) { out_ch(o, '0'); prec--; }
            while (k) out_ch(o, ft[--k]);
        }
        if (left) out_pad(o, width - total, ' ');
    }
}

int vsnprintf(char* buf, size_t cap, const char* fmt, va_list ap)
{
    out_t o;
    o.buf = buf; o.cap = cap; o.len = 0;

    for (; *fmt; fmt++) {
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        int width = 0, prec = -1, lng = 0;
        char c;

        if (*fmt != '%') { out_ch(&o, *fmt); continue; }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 't' || *fmt == 'j') { lng++; fmt++; }
        c = *fmt;
        if (!c) break;
        if (left) zero = 0;

        switch (c) {
        case 'd': case 'i': {
            int v = va_arg(ap, int);
            uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
            (void)lng;
            if (plus && v >= 0) { out_ch(&o, '+'); if (width) width--; }
            else if (space && v >= 0) { out_ch(&o, ' '); if (width) width--; }
            out_num(&o, u, v < 0, 10, 0, width, prec, left, zero && prec < 0, 0);
            break; }
        case 'u':
            out_num(&o, va_arg(ap, unsigned), 0, 10, 0, width, prec, left, zero && prec < 0, 0);
            break;
        case 'o':
            out_num(&o, va_arg(ap, unsigned), 0, 8, 0, width, prec, left, zero && prec < 0, alt ? "0" : 0);
            break;
        case 'x': case 'X':
            out_num(&o, va_arg(ap, unsigned), 0, 16, c == 'X', width, prec, left, zero && prec < 0,
                    alt ? (c == 'X' ? "0X" : "0x") : 0);
            break;
        case 'p':
            out_num(&o, (uint32_t)(uintptr_t)va_arg(ap, void*), 0, 16, 0, width, prec, left, 0, "0x");
            break;
        case 'c':
            if (!left) out_pad(&o, width - 1, ' ');
            out_ch(&o, (char)va_arg(ap, int));
            if (left) out_pad(&o, width - 1, ' ');
            break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            int n = 0;
            if (!s) s = "(null)";
            while (s[n] && (prec < 0 || n < prec)) n++;
            if (!left) out_pad(&o, width - n, ' ');
            { int i; for (i = 0; i < n; i++) out_ch(&o, s[i]); }
            if (left) out_pad(&o, width - n, ' ');
            break; }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
            out_dbl(&o, va_arg(ap, double), prec, width, left, zero);
            break;
        case '%':
            out_ch(&o, '%');
            break;
        default:
            out_ch(&o, '%'); out_ch(&o, c);
            break;
        }
    }
    if (cap) o.buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int snprintf(char* b, size_t n, const char* fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt); r = vsnprintf(b, n, fmt, ap); va_end(ap);
    return r;
}

int sprintf(char* b, const char* fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt); r = vsnprintf(b, 0x7fffffff, fmt, ap); va_end(ap);
    return r;
}

int vfprintf(FILE* f, const char* fmt, va_list ap)
{
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    fwrite(tmp, 1, (size_t)n, f);
    return n;
}

int fprintf(FILE* f, const char* fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap);
    return r;
}

int printf(const char* fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap);
    return r;
}

int puts(const char* s) { fputs(s, stdout); fputc('\n', stdout); return 0; }
int putchar(int c) { return fputc(c, stdout); }
void perror(const char* s) { fprintf(stderr, "%s: error\n", s); }

/* ================================================================== */
/* sscanf (%d %i %u %x %o %s %c %[set] %f, widths, %*)                  */
/* ================================================================== */
static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

int vsscanf(const char* s, const char* fmt, va_list ap)
{
    int count = 0;
    const char* start = s;

    for (; *fmt; fmt++) {
        if (isspace(*fmt)) { while (isspace(*s)) s++; continue; }
        if (*fmt != '%') {
            if (*s != *fmt) return count ? count : (*s ? 0 : EOF_);
            s++; continue;
        }
        fmt++;
        {
            int suppress = 0, width = 0, base = 10;
            if (*fmt == '*') { suppress = 1; fmt++; }
            while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
            while (*fmt == 'l' || *fmt == 'h') fmt++;
            if (*fmt != 'c' && *fmt != '[') while (isspace(*s)) s++;
            if (!*s && *fmt != '%') return count ? count : EOF_;

            switch (*fmt) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                long v = 0; int neg = 0, any = 0, w = width ? width : 1000;
                if (*fmt == 'x' || *fmt == 'X') base = 16;
                else if (*fmt == 'o') base = 8;
                if (*s == '-' || *s == '+') { neg = *s == '-'; s++; w--; }
                if (*fmt == 'i' || *fmt == 'x' || *fmt == 'X') {
                    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; w -= 2; }
                    else if (*fmt == 'i' && s[0] == '0') base = 8;
                }
                while (w-- > 0 && digit_val(*s) < base) { v = v * base + digit_val(*s); s++; any = 1; }
                if (!any) return count;
                if (!suppress) { *va_arg(ap, int*) = (int)(neg ? -v : v); count++; }
                break; }
            case 's': {
                char* d = suppress ? 0 : va_arg(ap, char*);
                int w = width ? width : 0x7fffffff;
                while (*s && !isspace(*s) && w-- > 0) { if (d) *d++ = *s; s++; }
                if (d) { *d = 0; count++; }
                break; }
            case 'c': {
                char* d = suppress ? 0 : va_arg(ap, char*);
                if (d) { *d = *s; count++; }
                s++;
                break; }
            case '[': {
                char set[256]; int inv = 0, i;
                char* d = suppress ? 0 : va_arg(ap, char*);
                int w = width ? width : 0x7fffffff, any = 0;
                memset(set, 0, sizeof set);
                fmt++;
                if (*fmt == '^') { inv = 1; fmt++; }
                if (*fmt == ']') { set[(int)']'] = 1; fmt++; }
                while (*fmt && *fmt != ']') {
                    if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                        for (i = (unsigned char)fmt[0]; i <= (unsigned char)fmt[2]; i++) set[i] = 1;
                        fmt += 3;
                    } else set[(unsigned char)*fmt++] = 1;
                }
                while (*s && w-- > 0 && (set[(unsigned char)*s] != 0) != inv) { if (d) *d++ = *s; s++; any = 1; }
                if (!any) return count;
                if (d) { *d = 0; count++; }
                break; }
            case '%':
                if (*s != '%') return count;
                s++;
                break;
            default:
                return count;
            }
        }
    }
    (void)start;
    return count;
}

int sscanf(const char* s, const char* fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt); r = vsscanf(s, fmt, ap); va_end(ap);
    return r;
}

/* Config-file parsing is disabled (fopen refuses *.cfg), so nothing
 * legitimate ever reaches fscanf. */
int fscanf(FILE* f, const char* fmt, ...) { (void)f; (void)fmt; return EOF_; }

double atof(const char* s)
{
    double v = 0, scale = 0.1;
    int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        while (isdigit(*s)) { v += (*s++ - '0') * scale; scale /= 10; }
    }
    return neg ? -v : v;
}

/* ================================================================== */
/* 64-bit division helpers (gcc emits these on i386)                    */
/* ================================================================== */
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t* rem)
{
    uint64_t q = 0, r = 0;
    int i;

    if ((d >> 32) == 0) {
        /* fast path: 64 / 32 via two hardware divides */
        uint32_t dl = (uint32_t)d, nh = (uint32_t)(n >> 32), nl = (uint32_t)n;
        uint32_t qh = nh / dl, rh = nh % dl, ql, rl;
        __asm__("divl %4" : "=a"(ql), "=d"(rl) : "a"(nl), "d"(rh), "r"(dl));
        if (rem) *rem = rl;
        return ((uint64_t)qh << 32) | ql;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; __udivmoddi4(n, d, &r); return r; }

int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = 0;
    uint64_t un = (uint64_t)n, ud = (uint64_t)d;
    if (n < 0) { un = 0 - un; neg ^= 1; }
    if (d < 0) { ud = 0 - ud; neg ^= 1; }
    un = __udivmoddi4(un, ud, 0);
    return neg ? -(int64_t)un : (int64_t)un;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t un = (uint64_t)n, ud = (uint64_t)d, r;
    if (n < 0) un = 0 - un;
    if (d < 0) ud = 0 - ud;
    __udivmoddi4(un, ud, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
