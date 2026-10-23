/* This file: formatted output (printf family), formatted input
 * (scanf family), memory management (malloc family) and the
 * exit()/atexit() machinery.  FILE streams, POSIX fds, time, math
 * and the string extras live in libc2.c; the export list for both
 * is libc.h.
 *
 * Everything here is exported to ELF programs through elf.c.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "libc.h"

extern void print(const char* s);
extern void print_n(const char* s, uint32_t len);
extern int get_key(void);
extern int key_available(void);
extern void read_line(char* buf, int max_len);
extern int strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern char* strcpy(char* dst, const char* src);
extern char* strcat(char* dst, const char* src);
extern void* memset(void* dst, int value, unsigned int n);
extern void* memcpy(void* dst, const void* src, unsigned int n);
extern void* memmove(void* dst, const void* src, unsigned int n);
extern int isspace(int c);
extern int isdigit(int c);
extern int toupper(int c);
extern uint32_t get_uptime_ms(void);

// exit() support: elf_run hooks a __builtin_setjmp into these.

int tanja_exit_jmp[5];
int tanja_exit_active = 0;
int tanja_exit_code = 0;

#define ATEXIT_MAX 32
static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

/* called by elf_run after the program body finishes (normal return
 * or exit()); runs handlers LIFO and clears the list */
void tanja_run_atexit(void) {
    while (atexit_count > 0)
        atexit_funcs[--atexit_count]();
}

void exit(int code) {
    if (tanja_exit_active) {
        tanja_exit_code = code;
        {
            extern int lx_unwind_reason;
            lx_unwind_reason = 3;
            print("[dbg] kernel-libc exit called\n");
        }
        __builtin_longjmp(tanja_exit_jmp, 1);
    }
    /* exit() outside of any program: nothing to unwind to */
    print("exit() called with no program to terminate\n");
}

void _exit(int code) { exit(code); }

void abort(void) {
    print("abort() called\n");
    exit(134);
}

// malloc: first-fit free list over a static 4 MB arena

#define MALLOC_ARENA_SIZE (4u * 1024u * 1024u)
#define MALLOC_ALIGN      16u
#define MALLOC_MAGIC      0xB10CA0Fu

typedef struct malloc_block {
    uint32_t magic;
    uint32_t size;              /* total incl. header */
    struct malloc_block* next;
    struct malloc_block* prev;
    int free_;
} mblock_t;

static uint8_t malloc_arena[MALLOC_ARENA_SIZE] __attribute__((aligned(16)));
static int malloc_ready = 0;
static mblock_t* malloc_head = 0;

static void malloc_init(void) {
    malloc_head = (mblock_t*)malloc_arena;
    malloc_head->magic = MALLOC_MAGIC;
    malloc_head->size = sizeof(malloc_arena);
    malloc_head->next = 0;
    malloc_head->prev = 0;
    malloc_head->free_ = 1;
    malloc_ready = 1;
}

static uint32_t malloc_round(size_t n) {
    n += sizeof(mblock_t);
    n = (n + MALLOC_ALIGN - 1) & ~(size_t)(MALLOC_ALIGN - 1);
    if (n < sizeof(mblock_t) + MALLOC_ALIGN) n = sizeof(mblock_t) + MALLOC_ALIGN;
    return (uint32_t)n;
}

void* malloc(size_t n) {
    if (!malloc_ready) malloc_init();
    uint32_t need = malloc_round(n);
    for (mblock_t* b = malloc_head; b; b = b->next) {
        if (!b->free_ || b->size < need)
            continue;
        if (b->size >= need + sizeof(mblock_t) + MALLOC_ALIGN) {
            mblock_t* nb = (mblock_t*)((uint8_t*)b + need);
            nb->magic = MALLOC_MAGIC;
            nb->size = b->size - need;
            nb->next = b->next;
            nb->prev = b;
            nb->free_ = 1;
            if (b->next) b->next->prev = nb;
            b->next = nb;
            b->size = need;
        }
        b->free_ = 0;
        return (uint8_t*)b + sizeof(mblock_t);
    }
    print("malloc: out of memory\n");
    return 0;
}

void free(void* p) {
    if (!p) return;
    mblock_t* b = (mblock_t*)((uint8_t*)p - sizeof(mblock_t));
    if (b->magic != MALLOC_MAGIC) {
        print("free: invalid pointer\n");
        return;
    }
    if (b->free_) {
        print("free: double free\n");
        return;
    }
    b->free_ = 1;
    if (b->next && b->next->free_) {
        b->size += b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free_) {
        b->prev->size += b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)0xFFFFFFFFu / nmemb) return 0;
    void* p = malloc(nmemb * size);
    if (p) memset(p, 0, (unsigned int)(nmemb * size));
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    mblock_t* b = (mblock_t*)((uint8_t*)p - sizeof(mblock_t));
    if (b->magic != MALLOC_MAGIC) { print("realloc: invalid pointer\n"); return 0; }
    uint32_t have = b->size - sizeof(mblock_t);
    if (have >= n) return p;
    if (b->next && b->next->free_ &&
        b->size + b->next->size >= malloc_round(n)) {
        b->size += b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
        return p;
    }
    void* np = malloc(n);
    if (!np) return 0;
    memcpy(np, p, have);
    free(p);
    return np;
}

/* Formatted output: one engine, many sinks.
 *
 * Supports flags - + 0 # space, width and precision (with *),
 * length modifiers h hh l ll L, and conversions
 * d i u o x X c s p n % f e E g G.
 */

typedef struct {
    int minus, plus, zero, hash, space;
    int width, precision;        /* precision -1 = unset */
    int lmod;                    /* 0 none, 1 h, 2 hh, 3 l, 4 ll, 5 L */
} spec_t;

typedef struct {
    void (*emit)(void* ctx, char c);
    void* ctx;
    int count;
} fmtsink_t;

static void fmt_putc(fmtsink_t* s, char c) { s->emit(s->ctx, c); s->count++; }

static void fmt_puts(fmtsink_t* s, const char* str) {
    if (!str) str = "(null)";
    while (*str) fmt_putc(s, *str++);
}

static void pad_out(fmtsink_t* s, int width, int len, int zero) {
    while (width-- > len) fmt_putc(s, zero ? '0' : ' ');
}

static int ulltoa_rev(unsigned long long v, char* out, unsigned base, int upper) {
    int n = 0;
    if (v == 0) { out[n++] = '0'; return n; }
    while (v) {
        unsigned d = (unsigned)(v % base);
        out[n++] = (char)(d < 10 ? ('0' + d) : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    }
    return n;
}

static void fmt_integer(fmtsink_t* s, unsigned long long v, int negative,
                        unsigned base, int upper, const spec_t* sp) {
    char digits[70];
    int nd = ulltoa_rev(v, digits, base, upper);
    int zeros = 0;
    if (sp->precision > nd) zeros = sp->precision - nd;
    const char* prefix = "";
    int plen = 0;
    if (sp->hash && base == 16 && v) { prefix = upper ? "0X" : "0x"; plen = 2; }
    if (base == 8 && sp->hash && zeros == 0 && digits[nd - 1] != '0')
        { zeros = 1; }   /* C octal # form */
    char sign = 0;
    if (negative) sign = '-';
    else if (sp->plus) sign = '+';
    else if (sp->space) sign = ' ';
    int total = nd + zeros + plen + (sign ? 1 : 0);
    if (!sp->minus && !(sp->zero && sp->precision < 0)) pad_out(s, sp->width, total, 0);
    if (sign) fmt_putc(s, sign);
    if (plen) fmt_puts(s, prefix);
    if (sp->zero && sp->precision < 0 && !sp->minus) pad_out(s, sp->width, total, 1);
    while (zeros-- > 0) fmt_putc(s, '0');
    for (int i = nd - 1; i >= 0; i--) fmt_putc(s, digits[i]);
    if (sp->minus) pad_out(s, sp->width, total, 0);
}

/* --- floating point helpers --- */

static void fmt_double(fmtsink_t* s, double v, int prec, char conv,
                       const spec_t* sp, char forced_sign);
static void str_emit(void* ctx, char c);

/* the string sink used by (v)s(n)printf */
struct strctx { char* b; int n; int cap; };

static double ipow10(int e) {
    if (e == 0) return 1.0;
    int neg = e < 0;
    if (neg) e = -e;
    double r = 1.0, base = 10.0;
    while (e) { if (e & 1) r *= base; base *= base; e >>= 1; }
    return neg ? 1.0 / r : r;
}

/* decimal exponent of v (floor(log10(|v|))), v != 0, finite */
static int dec_exp(double v) {
    int e = 0;
    v = v < 0 ? -v : v;
    if (v >= 1.0) {
        while (v >= 1e16) { v /= 1e16; e += 16; }
        while (v >= 10.0) { v /= 10.0; e++; }
    } else {
        while (v < 1e-16 && v > 0) { v *= 1e16; e -= 16; }
        while (v < 1.0) { v *= 10.0; e--; }
    }
    return e;
}

/* integer part as unsigned long long, clamped */
static unsigned long long to_ull(double v) {
    unsigned long long r = 0;
    while (v >= 1.0) {
        double chunk = v;
        if (chunk > 4e18) chunk = 4e18;
        v -= (double)(unsigned long long)chunk;
        if (v < 0) v = 0;
        if (r > 18446744073709551615ULL - (unsigned long long)chunk)
            return 18446744073709551615ULL;
        r += (unsigned long long)chunk;
    }
    return r;
}

/* Print v in %f style with prec digits after the point.
 * Handles |v| up to 1e18 exactly-ish; beyond that falls back to
 * e-notation (bounded output, nonstandard but sane). */
static void fmt_fixed(fmtsink_t* s, double v, int prec, const spec_t* sp,
                      char forced_sign) {
    int neg = v < 0;
    if (neg) v = -v;
    /* round: add 1/2 ulp of the last printed digit */
    v += 0.5 * ipow10(-prec);
    char sign = forced_sign;
    if (neg) sign = '-';
    if (v >= 1e18) {   /* too wide: scientific fallback */
        spec_t e = *sp;
        fmt_double(s, v, prec, 'e', &e, sign);
        return;
    }
    unsigned long long ip = to_ull(v);
    double frac = v - (double)ip;
    char digits[70];
    int nd = ulltoa_rev(ip, digits, 10, 0);
    int prec_digits = prec;
    char fracbuf[32];
    if (prec > 30) prec_digits = 30;
    for (int i = 0; i < prec_digits; i++) {
        frac *= 10.0;
        int d = (int)frac;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        fracbuf[i] = (char)('0' + d);
        frac -= d;
    }
    /* total width: sign + ip digits + '.' + prec (unless ".#") */
    int total = (sign ? 1 : 0) + nd + ((prec > 0) ? 1 + prec : 0);
    if (!sp->minus && !(sp->zero)) pad_out(s, sp->width, total, 0);
    if (sign) fmt_putc(s, sign);
    if (sp->zero && !sp->minus) pad_out(s, sp->width, total, 1);
    for (int i = nd - 1; i >= 0; i--) fmt_putc(s, digits[i]);
    if (prec > 0) {
        fmt_putc(s, '.');
        for (int i = 0; i < prec_digits; i++) fmt_putc(s, fracbuf[i]);
        for (int i = prec_digits; i < prec; i++) fmt_putc(s, '0');
    } else if (sp->hash) {
        fmt_putc(s, '.');
    }
    if (sp->minus) pad_out(s, sp->width, total, 0);
}

/* %e / %g / %f dispatcher for a double value. */
static void fmt_double(fmtsink_t* s, double v, int prec, char conv,
                       const spec_t* sp, char forced_sign) {
    int neg = v < 0;
    if (neg) v = -v;
    char sign = forced_sign;
    if (neg) sign = '-';
    /* NaN / infinity */
    if (v != v) {
        const char* t = (conv == 'E' || conv == 'G') ? "NAN" : "nan";
        int total = (sign ? 1 : 0) + 3;
        if (!sp->minus) pad_out(s, sp->width, total, 0);
        if (sign) fmt_putc(s, sign);
        fmt_puts(s, t);
        if (sp->minus) pad_out(s, sp->width, total, 0);
        return;
    }
    if (v > 1e308 * 10) {
        const char* t = (conv == 'E' || conv == 'G') ? "INF" : "inf";
        int total = (sign ? 1 : 0) + 3;
        if (!sp->minus) pad_out(s, sp->width, total, 0);
        if (sign) fmt_putc(s, sign);
        fmt_puts(s, t);
        if (sp->minus) pad_out(s, sp->width, total, 0);
        return;
    }

    if (conv == 'f' || conv == 'F') {
        fmt_fixed(s, v, prec, sp, sign);
        return;
    }

    if (conv == 'e' || conv == 'E') {
        int exp;
        double mant;
        if (v == 0.0) { mant = 0.0; exp = 0; }
        else {
            exp = dec_exp(v);
            mant = v / ipow10(exp);
            if (mant >= 10.0) { mant /= 10.0; exp++; }
            if (mant < 1.0) { mant *= 10.0; exp--; }
        }
        mant += 0.5 * ipow10(-prec);
        if (mant >= 10.0) { mant /= 10.0; exp++; }
        unsigned long long ip = (unsigned long long)mant;
        double frac = mant - (double)ip;
        char fracbuf[32];
        int pd = prec > 30 ? 30 : prec;
        for (int i = 0; i < pd; i++) {
            frac *= 10.0;
            int d = (int)frac;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            fracbuf[i] = (char)('0' + d);
            frac -= d;
        }
        char ebuf[8];
        int ea = exp < 0 ? -exp : exp;
        ebuf[0] = (char)((ea / 100) ? (ea / 100) + '0' : ' ');
        ebuf[1] = (char)(((ea / 10) % 10) + '0');
        ebuf[2] = (char)((ea % 10) + '0');
        int en = 3;
        if (ebuf[0] == ' ') { ebuf[0] = ebuf[1]; ebuf[1] = ebuf[2]; en = 2; }
        int total = (sign ? 1 : 0) + 1 + (prec ? prec + 1 : (sp->hash ? 1 : 0)) +
                    en + 2;
        if (!sp->minus) pad_out(s, sp->width, total, 0);
        if (sign) fmt_putc(s, sign);
        fmt_putc(s, (char)('0' + (int)ip));
        if (prec > 0) {
            fmt_putc(s, '.');
            for (int i = 0; i < pd; i++) fmt_putc(s, fracbuf[i]);
            for (int i = pd; i < prec; i++) fmt_putc(s, '0');
        } else if (sp->hash) {
            fmt_putc(s, '.');
        }
        fmt_putc(s, conv == 'E' ? 'E' : 'e');
        fmt_putc(s, exp < 0 ? '-' : '+');
        for (int i = 0; i < en; i++) fmt_putc(s, ebuf[i]);
        if (sp->minus) pad_out(s, sp->width, total, 0);
        return;
    }

    /* %g / %G: significant digits, strip trailing zeros */
    if (prec == 0) prec = 1;
    int X;
    if (v == 0.0) X = 0;
    else X = dec_exp(v);
    int use_fixed = (X >= -4 && X < prec);
    spec_t sp2 = *sp;
    int fprec = use_fixed ? prec - 1 - X : prec - 1;
    if (fprec < 0) fprec = 0;
    char c2 = use_fixed ? 'f' : 'e';
    if (!sp->hash) {
        /* format into a scratch buffer to strip trailing zeros */
        char buf[128];
        struct strctx sk;
        sk.b = buf; sk.n = 0; sk.cap = (int)sizeof buf;
        fmtsink_t s2;
        s2.ctx = &sk;
        s2.emit = str_emit;
        s2.count = 0;
        spec_t sp3 = *sp;
        sp3.width = 0; sp3.zero = 0; sp3.minus = 0;
        fmt_double(&s2, v, fprec, c2, &sp3, sign);
        int n = sk.n;
        /* find the exponent tail (e-notation), if any */
        int epos = -1;
        for (int i = 0; i < n; i++)
            if (buf[i] == 'e' || buf[i] == 'E') { epos = i; break; }
        int mend = epos < 0 ? n : epos;
        /* strip trailing zeros of the mantissa, and a bare point */
        int last = -1;
        for (int i = mend - 1; i >= 0; i--) {
            if (buf[i] == '.') { last = i - 1; break; }
            if (buf[i] != '0') { last = i; break; }
        }
        if (last >= 0 && last + 1 < mend) {
            int newn = last + 1;
            if (epos >= 0) {
                for (int i = 0; i < n - epos; i++)
                    buf[newn + i] = buf[epos + i];
                n = newn + (n - epos);
            } else {
                n = newn;
            }
        }
        /* re-emit with padding */
        int total = n;
        if (!sp->minus) pad_out(s, sp->width, total, 0);
        for (int i = 0; i < n; i++) fmt_putc(s, buf[i]);
        if (sp->minus) pad_out(s, sp->width, total, 0);
        return;
    }
    fmt_double(s, v, fprec, c2, &sp2, sign);
}

static void str_emit(void* ctx, char c) {
    struct strctx* k = (struct strctx*)ctx;
    if (k->n + 1 < k->cap) k->b[k->n] = c;
    k->n++;
}

/* core engine */
static int fmt_core(fmtsink_t* s, const char* fmt, va_list ap) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { fmt_putc(s, *p); continue; }
        p++;
        if (!*p) break;
        if (*p == '%') { fmt_putc(s, '%'); continue; }

        spec_t sp;
        sp.minus = sp.plus = sp.zero = sp.hash = sp.space = 0;
        sp.width = 0; sp.precision = -1; sp.lmod = 0;

        int done = 0;
        while (!done) {
            switch (*p) {
            case '-': sp.minus = 1; p++; break;
            case '+': sp.plus = 1; p++; break;
            case '0': sp.zero = 1; p++; break;
            case '#': sp.hash = 1; p++; break;
            case ' ': sp.space = 1; p++; break;
            default: done = 1;
            }
        }
        if (*p == '*') {
            sp.width = va_arg(ap, int);
            if (sp.width < 0) { sp.minus = 1; sp.width = -sp.width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') sp.width = sp.width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            sp.precision = 0;
            if (*p == '*') { sp.precision = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') sp.precision = sp.precision * 10 + (*p++ - '0');
        }
        while (*p == 'l') { sp.lmod = sp.lmod == 3 ? 4 : 3; p++; }
        if (*p == 'h') { sp.lmod = 1; p++; if (*p == 'h') { sp.lmod = 2; p++; } }
        if (*p == 'L') { sp.lmod = 5; p++; }
        if (*p == 'z' || *p == 't') { sp.lmod = 3; p++; }

        char conv = *p;
        int isupper = (conv >= 'A' && conv <= 'Z');
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (sp.lmod == 4) v = va_arg(ap, long long);
            else if (sp.lmod == 3) v = va_arg(ap, long);
            else if (sp.lmod == 1) v = (short)va_arg(ap, int);
            else if (sp.lmod == 2) v = (signed char)va_arg(ap, int);
            else v = va_arg(ap, int);
            int neg = v < 0;
            unsigned long long uv = neg ? -(unsigned long long)v : (unsigned long long)v;
            fmt_integer(s, uv, neg, 10, 0, &sp);
            break;
        }
        case 'u': {
            unsigned long long v;
            if (sp.lmod == 4) v = va_arg(ap, unsigned long long);
            else if (sp.lmod == 3) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            fmt_integer(s, v, 0, 10, 0, &sp);
            break;
        }
        case 'o': {
            unsigned long long v;
            if (sp.lmod == 4) v = va_arg(ap, unsigned long long);
            else if (sp.lmod == 3) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            fmt_integer(s, v, 0, 8, 0, &sp);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v;
            if (sp.lmod == 4) v = va_arg(ap, unsigned long long);
            else if (sp.lmod == 3) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            fmt_integer(s, v, 0, 16, conv == 'X', &sp);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!sp.minus) pad_out(s, sp.width, 1, 0);
            fmt_putc(s, c);
            if (sp.minus) pad_out(s, sp.width, 1, 0);
            break;
        }
        case 's': {
            const char* str = va_arg(ap, const char*);
            int len = 0;
            if (str) {
                if (sp.precision >= 0) {
                    while (len < sp.precision && str[len]) len++;
                } else len = strlen(str);
            } else len = 6;   /* "(null)" */
            if (!str) {
                if (!sp.minus) pad_out(s, sp.width, len, 0);
                fmt_puts(s, "(null)");
                if (sp.minus) pad_out(s, sp.width, len, 0);
                break;
            }
            if (!sp.minus) pad_out(s, sp.width, len, 0);
            for (int i = 0; i < len; i++) fmt_putc(s, str[i]);
            if (sp.minus) pad_out(s, sp.width, len, 0);
            break;
        }
        case 'p': {
            void* v = va_arg(ap, void*);
            unsigned long long uv = (unsigned long long)(uintptr_t)v;
            spec_t sp2 = sp; sp2.hash = 0;
            const char* px = "0x";
            int nd = 0;
            { unsigned long long t = uv; if (!t) nd = 1; else while (t) { t /= 16; nd++; } }
            int total = 2 + nd;
            if (!sp2.minus) pad_out(s, sp2.width, total, 0);
            fmt_puts(s, px);
            { char d[70]; int n = ulltoa_rev(uv, d, 16, 0);
              for (int i = n - 1; i >= 0; i--) fmt_putc(s, d[i]); }
            if (sp2.minus) pad_out(s, sp2.width, total, 0);
            break;
        }
        case 'n': {
            int* np = va_arg(ap, int*);
            *np = s->count;
            break;
        }
        case 'f': case 'F': {
            double v;
            if (sp.lmod == 5) { long double lv = va_arg(ap, long double); v = (double)lv; }
            else v = va_arg(ap, double);
            int prec = sp.precision >= 0 ? sp.precision : 6;
            fmt_double(s, v, prec, 'f', &sp, 0);
            break;
        }
        case 'e': case 'E': {
            double v;
            if (sp.lmod == 5) { long double lv = va_arg(ap, long double); v = (double)lv; }
            else v = va_arg(ap, double);
            int prec = sp.precision >= 0 ? sp.precision : 6;
            fmt_double(s, v, prec, conv, &sp, 0);
            break;
        }
        case 'g': case 'G': {
            double v;
            if (sp.lmod == 5) { long double lv = va_arg(ap, long double); v = (double)lv; }
            else v = va_arg(ap, double);
            int prec = sp.precision >= 0 ? sp.precision : 6;
            fmt_double(s, v, prec, conv, &sp, 0);
            break;
        }
        default:
            fmt_putc(s, '%');
            fmt_putc(s, conv);
            break;
        }
        (void)isupper;
    }
    return s->count;
}

/* --- sinks --- */

static void console_emit(void* ctx, char c) { (void)ctx; extern void console_putc(char); console_putc(c); }

int vprintf(const char* fmt, va_list ap) {
    fmtsink_t s;
    s.emit = console_emit;
    s.ctx = 0;
    s.count = 0;
    return fmt_core(&s, fmt, ap);
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char* str, size_t size, const char* fmt, va_list ap) {
    struct strctx k;
    k.b = str; k.n = 0; k.cap = size > 0 ? (int)size : 0;
    fmtsink_t s;
    s.emit = str_emit;
    s.ctx = &k;
    s.count = 0;
    fmt_core(&s, fmt, ap);
    if (size > 0) {
        int end = k.n < (int)size - 1 ? k.n : (int)size - 1;
        str[end] = 0;
    }
    return k.n;
}

int snprintf(char* str, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* str, const char* fmt, va_list ap) {
    return vsnprintf(str, (size_t)0x7FFFFFFF, fmt, ap);
}

int sprintf(char* str, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, (size_t)0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

/* vfprintf/fprintf: console streams go straight to the console;
 * files are formatted into a buffer and written (fd machinery is
 * in libc2.c). */
struct tanja_fd;
extern int tanja_fd_write(struct tanja_fd* f, const char* buf, uint32_t len);
extern int tanja_fd_is_console(struct tanja_fd* f);

int vfprintf(struct tanja_fd* f, const char* fmt, va_list ap) {
    if (!f) return -1;
    if (tanja_fd_is_console(f)) return vprintf(fmt, ap);
    char* buf = (char*)malloc(8192);
    if (!buf) return -1;
    int n = vsnprintf(buf, 8192, fmt, ap);
    if (n > 0) {
        int w = tanja_fd_write(f, buf, (uint32_t)(n > 8191 ? 8191 : n));
        (void)w;
    }
    free(buf);
    return n;
}

int fprintf(struct tanja_fd* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

// scanf family

struct scanctx {
    int (*next)(void*);     /* returns next char, or -1 at end */
    void* ctx;
    int unget;
    int ungotten;
    long consumed;
};

static int sc_getc(struct scanctx* c) {
    if (c->ungotten) {
        c->ungotten = 0;
        c->consumed++;
        return c->unget;
    }
    int ch = c->next(c->ctx);
    if (ch >= 0) c->consumed++;
    return ch;
}

static void sc_ungetc(struct scanctx* c, int ch) {
    c->unget = ch;
    c->ungotten = 1;
    c->consumed--;
}

static int str_next(void* ctx) {
    const char** sp = (const char**)ctx;
    if (!**sp) return -1;
    return (unsigned char)*(*sp)++;
}

static int vscan_core(struct scanctx* c, const char* fmt, va_list ap) {
    int assigned = 0;
    const char* p = fmt;
    for (; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '%') {
            int ch = sc_getc(c);
            if (ch < 0) return assigned ? assigned : -1;   /* EOF */
            if (ch != *p) { sc_ungetc(c, ch); return assigned; }
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\n') {
            /* skip whitespace */
            for (;;) {
                int ch = sc_getc(c);
                if (ch < 0) break;
                if (ch != ' ' && ch != '\t' && ch != '\n') { sc_ungetc(c, ch); break; }
            }
            continue;
        }
        /* % conversion */
        p++;
        if (*p == '%') {
            int ch = sc_getc(c);
            if (ch < 0) return assigned ? assigned : -1;
            if (ch != '%') { sc_ungetc(c, ch); return assigned; }
            continue;
        }
        int suppress = 0, width = 0;
        while (*p == '*') { suppress = 1; p++; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int lmod = 0;
        while (*p == 'l') { lmod = lmod == 3 ? 4 : 3; p++; }
        if (*p == 'h') { lmod = 1; p++; if (*p == 'h') { lmod = 2; p++; } }
        if (*p == 'L') { lmod = 5; p++; }
        if (*p == 'z') { lmod = 3; p++; }
        char conv = *p;
        if (!conv) break;

        if (conv == 'n') {
            int* np = va_arg(ap, int*);
            *np = (int)c->consumed;
            continue;
        }
        if (conv == 'c') {
            char* dst = suppress ? 0 : va_arg(ap, char*);
            int n = width ? width : 1;
            for (int i = 0; i < n; i++) {
                int ch = sc_getc(c);
                if (ch < 0) {
                    if (i == 0) return assigned ? assigned : -1;
                    return assigned;
                }
                if (dst) dst[i] = (char)ch;
            }
            if (!suppress) assigned++;
            continue;
        }
        if (conv == 's') {
            /* skip ws */
            int ch = sc_getc(c);
            while (ch >= 0 && (ch == ' ' || ch == '\t' || ch == '\n')) ch = sc_getc(c);
            if (ch < 0) return assigned ? assigned : -1;
            char* dst = suppress ? 0 : va_arg(ap, char*);
            int i = 0;
            int lim = width ? width : 1023;
            while (ch >= 0 && ch != ' ' && ch != '\t' && ch != '\n' && i < lim) {
                if (dst) dst[i] = (char)ch;
                i++;
                if (i >= lim) break;
                ch = sc_getc(c);
            }
            if (ch >= 0 && ch != ' ' && ch != '\t' && ch != '\n') sc_ungetc(c, ch);
            if (dst) dst[i] = 0;
            if (i == 0) return assigned;
            if (!suppress) assigned++;
            continue;
        }
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' ||
            conv == 'X' || conv == 'o') {
            int base = conv == 'd' ? 10 : conv == 'o' ? 8 :
                       conv == 'u' ? 10 : 16;
            int ch = sc_getc(c);
            while (ch >= 0 && (ch == ' ' || ch == '\t' || ch == '\n')) ch = sc_getc(c);
            if (ch < 0) return assigned ? assigned : -1;
            int neg = 0;
            if (ch == '+' || ch == '-') { neg = (ch == '-'); ch = sc_getc(c); }
            if (conv == 'i' && ch == '0') {
                int c2 = sc_getc(c);
                if (c2 == 'x' || c2 == 'X') { base = 16; ch = sc_getc(c); }
                else { base = 8; sc_ungetc(c, c2); }
            }
            unsigned long long val = 0;
            int nd = 0;
            int lim = width ? width : 1024;
            while (ch >= 0 && nd < lim) {
                int d;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                else break;
                if (d >= base) break;
                val = val * (unsigned)base + (unsigned)d;
                nd++;
                ch = sc_getc(c);
            }
            if (ch >= 0) sc_ungetc(c, ch);
            if (nd == 0) return assigned;
            if (!suppress) {
                if (neg) val = (unsigned long long)(-(long long)val);
                switch (lmod) {
                case 4: { long long* v = va_arg(ap, long long*); *v = (long long)val; break; }
                case 3: { long* v = va_arg(ap, long*); *v = (long)val; break; }
                case 1: { short* v = va_arg(ap, short*); *v = (short)val; break; }
                case 2: { signed char* v = va_arg(ap, signed char*); *v = (signed char)val; break; }
                default: { int* v = va_arg(ap, int*); *v = (int)val; break; }
                }
                assigned++;
            }
            continue;
        }
        if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' ||
            conv == 'G' || conv == 'a' || conv == 'A') {
            int ch = sc_getc(c);
            while (ch >= 0 && (ch == ' ' || ch == '\t' || ch == '\n')) ch = sc_getc(c);
            if (ch < 0) return assigned ? assigned : -1;
            int neg = 0;
            if (ch == '+' || ch == '-') { neg = (ch == '-'); ch = sc_getc(c); }
            double val = 0;
            int nd = 0;
            while (ch >= '0' && ch <= '9') { val = val * 10 + (ch - '0'); nd++; ch = sc_getc(c); }
            if (ch == '.') {
                ch = sc_getc(c);
                double scale = 0.1;
                while (ch >= '0' && ch <= '9') { val += (ch - '0') * scale; scale *= 0.1; nd++; ch = sc_getc(c); }
            }
            if ((ch == 'e' || ch == 'E') && nd) {
                ch = sc_getc(c);
                int eneg = 0;
                if (ch == '+' || ch == '-') { eneg = (ch == '-'); ch = sc_getc(c); }
                int ev = 0, ed = 0;
                while (ch >= '0' && ch <= '9') { ev = ev * 10 + ch - '0'; ed++; ch = sc_getc(c); }
                if (ed) {
                    double m = 1.0;
                    for (int i = 0; i < ev && i < 300; i++) m *= 10.0;
                    if (eneg) val /= m; else val *= m;
                } else {
                    sc_ungetc(c, ch); sc_ungetc(c, eneg ? '-' : '+');
                }
            } else if (ch >= 0) {
                sc_ungetc(c, ch);
            }
            if (neg) val = -val;
            if (nd == 0) return assigned;
            if (!suppress) {
                switch (lmod) {
                case 5: { long double* v = va_arg(ap, long double*); *v = (long double)val; break; }
                case 3: { double* v = va_arg(ap, double*); *v = val; break; }
                default: { float* v = va_arg(ap, float*); *v = (float)val; break; }
                }
                assigned++;
            }
            continue;
        }
        /* unknown conversion: stop */
        return assigned;
    }
    return assigned;
}

int vsscanf(const char* str, const char* fmt, va_list ap) {
    const char* s = str;
    struct scanctx c;
    c.next = str_next;
    c.ctx = &s;
    c.ungotten = 0;
    c.consumed = 0;
    return vscan_core(&c, fmt, ap);
}

int sscanf(const char* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int vscanf(const char* fmt, va_list ap) {
    /* line-buffered from the keyboard: read one line, then scan it */
    static char line[2048];
    read_line(line, (int)sizeof line - 1);
    const char* s = line;
    struct scanctx c;
    c.next = str_next;
    c.ctx = &s;
    c.ungotten = 0;
    c.consumed = 0;
    return vscan_core(&c, fmt, ap);
}

int scanf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vscanf(fmt, ap);
    va_end(ap);
    return r;
}

/* fscanf/vfscanf: character-at-a-time from a FILE (libc2.c owns the
 * fd machinery; tanja_fd_getc returns -1 at EOF). */
extern int tanja_fd_getc(struct tanja_fd* f);

struct scan_file_ctx { struct tanja_fd* f; };

static int file_next(void* ctx) {
    struct scan_file_ctx* k = (struct scan_file_ctx*)ctx;
    return tanja_fd_getc(k->f);
}

int vfscanf(struct tanja_fd* f, const char* fmt, va_list ap) {
    struct scan_file_ctx k;
    k.f = f;
    struct scanctx c;
    c.next = file_next;
    c.ctx = &k;
    c.ungotten = 0;
    c.consumed = 0;
    return vscan_core(&c, fmt, ap);
}

int fscanf(struct tanja_fd* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}
