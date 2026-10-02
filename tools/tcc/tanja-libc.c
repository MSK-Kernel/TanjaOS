#include "tanja-libc.h"

int errno = 0;

/* File Descriptors */
typedef struct {
    int used;
    char path[256];
    int flags;
    uint32_t pos;
    uint32_t size;
    uint32_t capacity;
    char* buffer;
    int dirty;
    int is_read;
} TanjaFD;

#define TANJA_MAX_FD 32
static TanjaFD g_fds[TANJA_MAX_FD];

static FILE g_stdin_file = { .fd = 0, .eof = 0, .error = 0, .unget_buf = -1 };
static FILE g_stdout_file = { .fd = 1, .eof = 0, .error = 0, .unget_buf = -1 };
static FILE g_stderr_file = { .fd = 2, .eof = 0, .error = 0, .unget_buf = -1 };

FILE *stdin = &g_stdin_file;
FILE *stdout = &g_stdout_file;
FILE *stderr = &g_stderr_file;

int open(const char *path, int flags, ...) {
    if (!path) return -1;
    if (path[0] == '.' && path[1] == '/') path += 2;

    int slot = -1;
    for (int i = 3; i < TANJA_MAX_FD; i++) {
        if (!g_fds[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    TanjaFD *fd = &g_fds[slot];
    memset(fd, 0, sizeof(TanjaFD));
    
    /* Copy path */
    strncpy(fd->path, path, sizeof(fd->path) - 1);
    fd->flags = flags;
    fd->used = 1;

    if ((flags & O_WRONLY) || (flags & O_RDWR) || (flags & O_CREAT)) {
        if ((flags & O_TRUNC) || !fs_file_exists(path)) {
            fd->capacity = 16384;
            fd->buffer = (char*)malloc(fd->capacity);
            if (!fd->buffer) { fd->used = 0; return -1; }
            fd->size = 0;
            fd->pos = 0;
            fd->dirty = 1;
        } else {
            /* Existing file to modify/append */
            uint32_t sz = fs_get_file_size(path);
            fd->capacity = sz + 16384;
            fd->buffer = (char*)malloc(fd->capacity);
            if (!fd->buffer) { fd->used = 0; return -1; }
            if (sz > 0) {
                fs_read_file(path, fd->buffer, &sz);
            }
            fd->size = sz;
            fd->pos = (flags & O_APPEND) ? sz : 0;
            fd->dirty = 0;
        }
    } else {
        /* Read-only */
        if (!fs_file_exists(path)) { fd->used = 0; return -1; }
        uint32_t sz = fs_get_file_size(path);
        fd->capacity = sz + 16;
        fd->buffer = (char*)malloc(fd->capacity);
        if (!fd->buffer) { fd->used = 0; return -1; }
        if (sz > 0) {
            fs_read_file(path, fd->buffer, &sz);
        }
        fd->size = sz;
        fd->pos = 0;
        fd->is_read = 1;
        fd->dirty = 0;
    }

    return slot;
}

int close(int fd) {
    if (fd < 0 || fd >= TANJA_MAX_FD || !g_fds[fd].used) return -1;
    if (fd <= 2) return 0;

    TanjaFD *f = &g_fds[fd];
    if (f->dirty) {
        fs_write_file(f->path, f->buffer, f->size);
    }
    f->used = 0;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (fd == 0) return 0; /* stdin stub (no interactivity here) */
    if (fd == 1 || fd == 2) return 0;
    if (fd < 0 || fd >= TANJA_MAX_FD || !g_fds[fd].used) return -1;

    TanjaFD *f = &g_fds[fd];
    uint32_t avail = (f->pos < f->size) ? (f->size - f->pos) : 0;
    uint32_t to_read = (count < avail) ? count : avail;
    if (to_read > 0) {
        memcpy(buf, f->buffer + f->pos, to_read);
        f->pos += to_read;
    }
    return to_read;
}

ssize_t write(int fd, const void *buf, size_t count) {
    /* fd 1 / fd 2 are the console and always "open" - check them
       before the file-table test below (std streams have no table
       entry, so the old order silently dropped every stderr write) */
    if (fd == 1 || fd == 2) {
        print_n((const char*)buf, count);
        return count;
    }
    if (fd < 0 || fd >= TANJA_MAX_FD || !g_fds[fd].used) return -1;

    TanjaFD *f = &g_fds[fd];
    if (f->pos + count > f->capacity) {
        uint32_t new_cap = (f->pos + count + 16383) & ~16383u;
        char *new_buf = (char*)realloc(f->buffer, new_cap);
        if (!new_buf) return -1;
        f->buffer = new_buf;
        f->capacity = new_cap;
    }

    memcpy(f->buffer + f->pos, buf, count);
    f->pos += count;
    if (f->pos > f->size) f->size = f->pos;
    f->dirty = 1;
    return count;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= TANJA_MAX_FD || !g_fds[fd].used) return (off_t)-1;
    TanjaFD *f = &g_fds[fd];

    int32_t new_pos = f->pos;
    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_CUR) new_pos += offset;
    else if (whence == SEEK_END) new_pos = f->size + offset;

    if (new_pos < 0) new_pos = 0;
    f->pos = new_pos;
    return f->pos;
}

int unlink(const char *path) {
    if (!path) return -1;
    if (path[0] == '.' && path[1] == '/') path += 2;
    return fs_delete_file(path) == 0 ? 0 : -1;
}

int remove(const char *path) {
    return unlink(path);
}

int rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -1;
    if (oldpath[0] == '.' && oldpath[1] == '/') oldpath += 2;
    if (newpath[0] == '.' && newpath[1] == '/') newpath += 2;

    uint32_t sz = fs_get_file_size(oldpath);
    if (sz == 0 && !fs_file_exists(oldpath)) return -1;

    char *buf = (char*)malloc(sz + 1);
    if (!buf) return -1;

    if (fs_read_file(oldpath, buf, &sz) == 0) {
        fs_write_file(newpath, buf, sz);
        fs_delete_file(oldpath);
        return 0;
    }
    return -1;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    int flags = O_RDONLY;
    if (strchr(mode, 'w')) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strchr(mode, 'a')) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (strchr(mode, '+')) flags = O_RDWR | O_CREAT;

    int fd = open(path, flags, 0666);
    if (fd < 0) return NULL;

    FILE *fp = (FILE*)malloc(sizeof(FILE));
    if (!fp) { close(fd); return NULL; }
    fp->fd = fd;
    fp->eof = 0;
    fp->error = 0;
    fp->unget_buf = -1;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp) return EOF;
    int fd = fp->fd;
    int res = close(fd);
    if (fp != stdout && fp != stderr && fp != stdin) {
        /* free(fp) */
    }
    return res;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    char *out = (char*)ptr;
    size_t read_bytes = 0;

    if (fp->unget_buf != -1) {
        *out++ = (char)fp->unget_buf;
        fp->unget_buf = -1;
        read_bytes++;
        total--;
    }

    if (total > 0) {
        ssize_t res = read(fp->fd, out, total);
        if (res > 0) read_bytes += res;
    }

    if (read_bytes < size * nmemb) fp->eof = 1;
    return read_bytes / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t res = write(fp->fd, ptr, total);
    if (res < 0) { fp->error = 1; return 0; }
    return res / size;
}

int fseek(FILE *fp, long offset, int whence) {
    if (!fp) return -1;
    fp->eof = 0;
    fp->unget_buf = -1;
    off_t res = lseek(fp->fd, offset, whence);
    return (res == (off_t)-1) ? -1 : 0;
}

long ftell(FILE *fp) {
    if (!fp) return -1;
    return (long)lseek(fp->fd, 0, SEEK_CUR);
}

int feof(FILE *fp) { return fp ? fp->eof : 1; }
int ferror(FILE *fp) { return fp ? fp->error : 1; }

int fgetc(FILE *fp) {
    unsigned char c;
    if (fread(&c, 1, 1, fp) == 1) return c;
    return EOF;
}

int getc(FILE *fp) { return fgetc(fp); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *fp) {
    if (!fp || c == EOF) return EOF;
    fp->unget_buf = c;
    fp->eof = 0;
    return c;
}

char *fgets(char *s, int size, FILE *fp) {
    if (!fp || size <= 1) return NULL;
    int idx = 0;
    while (idx < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) {
            if (idx == 0) return NULL;
            break;
        }
        s[idx++] = (char)c;
        if (c == '\n') break;
    }
    s[idx] = '\0';
    return s;
}

int fputs(const char *s, FILE *fp) {
    if (!s || !fp) return EOF;
    size_t len = strlen(s);
    return (fwrite(s, 1, len, fp) == len) ? 0 : EOF;
}

int fputc(int c, FILE *fp) {
    unsigned char ch = (unsigned char)c;
    return (fwrite(&ch, 1, 1, fp) == 1) ? ch : EOF;
}

int putc(int c, FILE *fp) { return fputc(c, fp); }

/* vsnprintf & Printf implementation */

static void format_num(char **out, size_t *left, size_t *total, uint64_t val, int base, int is_signed, int width, int prec, int flags, int upper) {
    char buf[65];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int neg = 0;
    if (is_signed && (int64_t)val < 0) {
        neg = 1;
        val = (uint64_t)(-(int64_t)val);
    }

    int len = 0;
    if (val == 0) buf[len++] = '0';
    while (val > 0) {
        buf[len++] = digits[val % base];
        val /= base;
    }

    int pad_prec = (prec > len) ? (prec - len) : 0;
    int core_len = len + pad_prec + (neg ? 1 : 0);
    int pad_width = (width > core_len) ? (width - core_len) : 0;

    int left_align = (flags & 1);
    int zero_pad = (flags & 2) && !(flags & 1) && (prec < 0);

    /* helper macro to emit char */
#define EMIT_C(c) do { \
    if (*left > 1) { **out = (c); (*out)++; (*left)--; } \
    (*total)++; \
} while(0)

    if (!left_align && !zero_pad) {
        while (pad_width-- > 0) EMIT_C(' ');
    }
    if (neg) EMIT_C('-');
    if (!left_align && zero_pad) {
        while (pad_width-- > 0) EMIT_C('0');
    }
    while (pad_prec-- > 0) EMIT_C('0');
    while (len > 0) EMIT_C(buf[--len]);
    if (left_align) {
        while (pad_width-- > 0) EMIT_C(' ');
    }
#undef EMIT_C
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    char *out = str;
    size_t left = size;
    size_t total = 0;

#define EMIT_CHAR(c) do { \
    if (left > 1) { *out++ = (c); left--; } \
    total++; \
} while(0)

    while (*format) {
        if (*format != '%') {
            EMIT_CHAR(*format++);
            continue;
        }
        format++; /* skip '%' */

        int flags = 0; /* bit 0: '-', bit 1: '0' */
        while (*format == '-' || *format == '0') {
            if (*format == '-') flags |= 1;
            if (*format == '0') flags |= 2;
            format++;
        }

        int width = -1;
        if (*format == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= 1; width = -width; }
            format++;
        } else if (isdigit((unsigned char)*format)) {
            width = 0;
            while (isdigit((unsigned char)*format)) {
                width = width * 10 + (*format++ - '0');
            }
        }

        int prec = -1;
        if (*format == '.') {
            format++;
            if (*format == '*') {
                prec = va_arg(ap, int);
                format++;
            } else if (isdigit((unsigned char)*format)) {
                prec = 0;
                while (isdigit((unsigned char)*format)) {
                    prec = prec * 10 + (*format++ - '0');
                }
            } else {
                prec = 0;
            }
        }

        int length = 0; /* 0: default, 1: l, 2: ll, 3: h, 4: z */
        if (*format == 'l') {
            format++;
            if (*format == 'l') { length = 2; format++; }
            else length = 1;
        } else if (*format == 'h') {
            length = 3; format++;
        } else if (*format == 'z') {
            length = 4; format++;
        }

        char spec = *format++;
        if (spec == '%') {
            EMIT_CHAR('%');
        } else if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            EMIT_CHAR(c);
        } else if (spec == 's') {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int len = strlen(s);
            if (prec >= 0 && prec < len) len = prec;
            int pad = (width > len) ? (width - len) : 0;
            if (!(flags & 1)) {
                while (pad-- > 0) EMIT_CHAR(' ');
            }
            for (int i = 0; i < len; i++) EMIT_CHAR(s[i]);
            if (flags & 1) {
                while (pad-- > 0) EMIT_CHAR(' ');
            }
        } else if (spec == 'd' || spec == 'i') {
            int64_t val;
            if (length == 2) val = va_arg(ap, int64_t);
            else if (length == 1) val = va_arg(ap, long);
            else val = va_arg(ap, int);
            format_num(&out, &left, &total, (uint64_t)val, 10, 1, width, prec, flags, 0);
        } else if (spec == 'u') {
            uint64_t val;
            if (length == 2) val = va_arg(ap, uint64_t);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            format_num(&out, &left, &total, val, 10, 0, width, prec, flags, 0);
        } else if (spec == 'x' || spec == 'X') {
            uint64_t val;
            if (length == 2) val = va_arg(ap, uint64_t);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            format_num(&out, &left, &total, val, 16, 0, width, prec, flags, (spec == 'X'));
        } else if (spec == 'p') {
            void *ptr = va_arg(ap, void*);
            format_num(&out, &left, &total, (uintptr_t)ptr, 16, 0, width, prec, flags, 0);
        } else {
            EMIT_CHAR(spec);
        }
    }

    if (size > 0) {
        if (left > 0) *out = '\0';
        else str[size - 1] = '\0';
    }
#undef EMIT_CHAR
    return (int)total;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, size, format, ap);
    va_end(ap);
    return res;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, 0x7FFFFFFF, format, ap);
    va_end(ap);
    return res;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[2048];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) {
        if (stream == stdout || stream == stderr) {
            print_n(buf, (len < (int)sizeof(buf)) ? len : (sizeof(buf) - 1));
        } else {
            fwrite(buf, 1, (len < (int)sizeof(buf)) ? len : (sizeof(buf) - 1), stream);
        }
    }
    return len;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vfprintf(stream, format, ap);
    va_end(ap);
    return res;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vprintf(format, ap);
    va_end(ap);
    return res;
}

/* Algorithms */

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    char *b = (char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0; j--) {
            char *p1 = b + (j - 1) * size;
            char *p2 = b + j * size;
            if (compar(p1, p2) > 0) {
                for (size_t k = 0; k < size; k++) {
                    char tmp = p1[k];
                    p1[k] = p2[k];
                    p2[k] = tmp;
                }
            } else {
                break;
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    size_t l = 0, r = nmemb;
    while (l < r) {
        size_t m = l + (r - l) / 2;
        const void *p = (const char *)base + m * size;
        int cmp = compar(key, p);
        if (cmp == 0) return (void *)p;
        if (cmp < 0) r = m;
        else l = m + 1;
    }
    return NULL;
}

/* Number Parsing */

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    while (isspace((unsigned char)*p)) p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }

    if (base == 0) {
        if (*p == '0') {
            p++;
            if (*p == 'x' || *p == 'X') { base = 16; p++; }
            else base = 8;
        } else base = 10;
    } else if (base == 16) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    }

    unsigned long long val = 0;
    int digits = 0;
    while (*p) {
        int d = -1;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        if (d < 0 || d >= base) break;
        val = val * base + d;
        p++;
        digits++;
    }
    if (endptr) *endptr = (char *)(digits ? p : nptr);
    return (sign < 0) ? (unsigned long long)(-(long long)val) : val;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtoull(nptr, endptr, base);
}

long strtol(const char *nptr, char **endptr, int base) {
    return (long)strtoll(nptr, endptr, base);
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtoull(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr) {
    const char *p = nptr;
    while (isspace((unsigned char)*p)) p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }

    double val = 0.0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
        val = val * 10.0 + (*p - '0');
        p++;
        digits++;
    }
    if (*p == '.') {
        p++;
        double frac = 0.1;
        while (isdigit((unsigned char)*p)) {
            val += (*p - '0') * frac;
            frac *= 0.1;
            p++;
            digits++;
        }
    }
    if (digits > 0 && (*p == 'e' || *p == 'E')) {
        const char *ep = p;
        p++;
        int esign = 1;
        if (*p == '-') { esign = -1; p++; }
        else if (*p == '+') { p++; }
        if (isdigit((unsigned char)*p)) {
            int exp = 0;
            while (isdigit((unsigned char)*p)) {
                exp = exp * 10 + (*p - '0');
                p++;
            }
            double factor = 1.0;
            for (int i = 0; i < exp; i++) factor *= 10.0;
            if (esign < 0) val /= factor;
            else val *= factor;
        } else {
            p = ep;
        }
    }
    if (endptr) *endptr = (char *)(digits ? p : nptr);
    return sign * val;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

/* System / Env */

char *getenv(const char *name) {
    return (char*)get_env(name);
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && get_env(name)) return 0;
    set_env(name, value);
    return 0;
}

time_t time(time_t *t) {
    time_t sec = get_uptime_ms() / 1000;
    if (t) *t = sec;
    return sec;
}

int system(const char *command) {
    if (!command) return 1;
    execute_command(command);
    return 0;
}

sighandler_t signal(int signum, sighandler_t handler) {
    (void)signum; (void)handler;
    return SIG_DFL;
}

int fstat(int fd, struct stat *st) {
    if (fd < 0 || fd >= TANJA_MAX_FD || !g_fds[fd].used || !st) return -1;
    memset(st, 0, sizeof(struct stat));
    st->st_size = g_fds[fd].size;
    st->st_mode = S_IFREG | 0644;
    return 0;
}

int stat(const char *path, struct stat *st) {
    if (!path || !st) return -1;
    if (path[0] == '.' && path[1] == '/') path += 2;
    memset(st, 0, sizeof(struct stat));
    if (fs_directory_exists(path)) {
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    if (!fs_file_exists(path)) return -1;
    st->st_size = fs_get_file_size(path);
    st->st_mode = S_IFREG | 0644;
    return 0;
}

int setjmp(jmp_buf env) { (void)env; return 0; }
void longjmp(jmp_buf env, int val) { (void)env; (void)val; abort(); }

/* Libgcc 64-bit shift helpers */

long long __ashldi3(long long a, int b) {
    b &= 63;
    if (b == 0) return a;
    unsigned int low = (unsigned int)a;
    unsigned int high = (unsigned int)(a >> 32);
    if (b >= 32) {
        high = low << (b - 32);
        low = 0;
    } else {
        high = (high << b) | (low >> (32 - b));
        low = low << b;
    }
    return ((unsigned long long)high << 32) | low;
}

unsigned long long __lshrdi3(unsigned long long a, int b) {
    b &= 63;
    if (b == 0) return a;
    unsigned int low = (unsigned int)a;
    unsigned int high = (unsigned int)(a >> 32);
    if (b >= 32) {
        low = high >> (b - 32);
        high = 0;
    } else {
        low = (low >> b) | (high << (32 - b));
        high = high >> b;
    }
    return ((unsigned long long)high << 32) | low;
}

long long __ashrdi3(long long a, int b) {
    b &= 63;
    if (b == 0) return a;
    unsigned int low = (unsigned int)a;
    int high = (int)(a >> 32);
    if (b >= 32) {
        low = (unsigned int)(high >> (b - 32));
        high = high >> 31;
    } else {
        low = (low >> b) | ((unsigned int)high << (32 - b));
        high = high >> b;
    }
    return ((unsigned long long)(unsigned int)high << 32) | low;
}

/* Double/Float conversions */

double __floatdidf(long long a) { return (double)a; }
double __floatundidf(unsigned long long a) { return (double)a; }
float __floatdisf(long long a) { return (float)a; }
float __floatundisf(unsigned long long a) { return (float)a; }
long long __fixdfdi(double a) { return (long long)a; }
unsigned long long __fixunsdfdi(double a) { return (unsigned long long)a; }
long long __fixsfdi(float a) { return (long long)a; }
unsigned long long __fixunssfdi(float a) { return (unsigned long long)a; }

/* --- time support (__DATE__/__TIME__) --- */
static struct tm g_tm;
static time_t g_time_base = 0; /* seconds at boot (arbitrary epoch) */

struct tm *localtime(const time_t *timep) {
    time_t v = timep ? *timep : time(NULL);
    uint32_t days = (uint32_t)(v / 86400);
    uint32_t rem = (uint32_t)(v % 86400);
    g_tm.tm_hour = rem / 3600;
    g_tm.tm_min = (rem / 60) % 60;
    g_tm.tm_sec = rem % 60;
    /* Fixed base date: Jan 1, 2026 (days since epoch handled simply). */
    g_tm.tm_year = 126;                 /* years since 1900 */
    static const uint8_t mdays[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    g_tm.tm_mon = 0; g_tm.tm_mday = 1 + days;
    while (g_tm.tm_mday > mdays[g_tm.tm_mon]) {
        g_tm.tm_mday -= mdays[g_tm.tm_mon];
        if (++g_tm.tm_mon == 12) { g_tm.tm_mon = 0; g_tm.tm_year++; }
    }
    g_tm.tm_wday = 0; g_tm.tm_yday = 0; g_tm.tm_isdst = 0;
    return &g_tm;
}

struct tm *gmtime(const time_t *timep) { return localtime(timep); }

int fflush(FILE *fp) { (void)fp; return 0; }   /* unbuffered: nothing to do */

long double ldexpl(long double x, int exp) {
    /* only used by the preprocessor for hex float exponents */
    while (exp > 0) { x = x * 2.0L; exp--; }
    while (exp < 0) { x = x / 2.0L; exp++; }
    return x;
}
double ldexp(double x, int exp) { return (double)ldexpl((long double)x, exp); }
double ldexpf(float x, int exp) { return (float)ldexpl((long double)x, exp); }

/* --- signal / process / mman stubs (never called in compile mode) --- */
static struct sigaction g_oldact;
int sigaction(int signum, const struct sigaction *act, struct sigaction *old) {
    (void)signum; if (old) *old = g_oldact; if (act) g_oldact = *act;
    return 0;
}
int sigemptyset(sigset_t *set) { if (set) set->val = 0; return 0; }
int sigfillset(sigset_t *set) { if (set) set->val = 0xffffffffu; return 0; }
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how; (void)set; (void)oldset; return 0;
}
int gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tv) {
        uint32_t ms = get_uptime_ms();
        tv->tv_sec = ms / 1000;
        tv->tv_usec = (ms % 1000) * 1000;
    }
    (void)tz;
    return 0;
}
char *strerror(int errnum) {
    switch (errnum) {
    case ENOENT: return (char*)"No such file or directory";
    case EINTR:  return (char*)"Interrupted system call";
    case EACCES: return (char*)"Permission denied";
    case EEXIST: return (char*)"File exists";
    case ENOMEM: return (char*)"Out of memory";
    case EINVAL: return (char*)"Invalid argument";
    default:     return (char*)"Unknown error";
    }
}
char **environ = NULL;
int execv(const char *path, char *const argv[]) {
    (void)path; (void)argv; errno = EINVAL; return -1;
}
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)off;
    errno = EINVAL; return MAP_FAILED;
}
int mprotect(void *addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot; errno = EINVAL; return -1;
}
int munmap(void *addr, size_t length) {
    (void)addr; (void)length; errno = EINVAL; return -1;
}
void *dlopen(const char *filename, int flag) { (void)filename; (void)flag; return 0; }
void *dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; return 0; }
int dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { return (char*)"no dynamic linking on TanjaOS"; }

/* --- remaining libc bits tcc needs --- */
#define TANJA_PATH_MAX 1024

void abort(void) { exit(134); }

int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv; errno = EINVAL; return -1;
}

static FILE g_fdopen_pool[8];
static int g_fdopen_used[8];

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    for (int i = 0; i < 8; i++) {
        if (!g_fdopen_used[i]) {
            g_fdopen_used[i] = 1;
            g_fdopen_pool[i].fd = fd;
            g_fdopen_pool[i].eof = 0;
            g_fdopen_pool[i].error = 0;
            g_fdopen_pool[i].unget_buf = -1;
            return &g_fdopen_pool[i];
        }
    }
    errno = EMFILE; return NULL;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (stream && stream->fd > 2) {
        for (int i = 0; i < 8; i++)
            if (g_fdopen_used[i] && &g_fdopen_pool[i] == stream)
                { g_fdopen_used[i] = 0; break; }
    }
    int fd = open(path, mode && strchr(mode, 'r') ? O_RDONLY : O_WRONLY|O_CREAT|O_TRUNC);
    if (fd < 0) { if (stream) stream->error = 1; return NULL; }
    if (stream) {
        stream->fd = fd;
        stream->eof = stream->error = 0;
        stream->unget_buf = -1;
    }
    return stream;
}

char *getcwd(char *buf, size_t size) {
    static char tmp[TANJA_PATH_MAX];
    fs_get_current_path(tmp);
    if (!buf) {
        buf = malloc(TANJA_PATH_MAX);
        if (!buf) { errno = ENOMEM; return NULL; }
        size = TANJA_PATH_MAX;
    }
    if (strlen(tmp) >= size) { errno = ERANGE; return NULL; }
    strcpy(buf, tmp);
    return buf;
}

char *realpath(const char *path, char *resolved_path) {
    /* TanjaOS has no symlinks: the resolved path IS the input path.
     * Like glibc, fail if the file does not exist. */
    if (!path || !fs_file_exists(path)) { errno = ENOENT; return NULL; }
    if (!resolved_path) {
        resolved_path = malloc(TANJA_PATH_MAX);
        if (!resolved_path) { errno = ENOMEM; return NULL; }
    }
    if (strlen(path) >= TANJA_PATH_MAX) { errno = ERANGE; return NULL; }
    strcpy(resolved_path, path);
    return resolved_path;
}

int sigaddset(sigset_t *set, int signum) {
    if (set && signum > 0 && signum <= 32) set->val |= (1u << (signum - 1));
    return 0;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char *)s;
    return NULL;
}

/* ---- 64-bit divide helpers (gcc emits calls to these on i386) ----
 * Implemented with a plain shift-subtract loop so they never recurse
 * into themselves the way a `/` in C would (gcc would emit a call to
 * the very helper we are defining). */
static unsigned long long tanja_udiv64(unsigned long long a, unsigned long long b,
                                       unsigned long long *rem_out)
{
    unsigned long long q = 0, r = 0;
    int i;
    if (b == 0) { if (rem_out) *rem_out = 0; return 0; }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1ull);
        if (r >= b) { r -= b; q |= (1ull << i); }
    }
    if (rem_out) *rem_out = r;
    return q;
}

unsigned long long __udivdi3(unsigned long long a, unsigned long long b)
{
    return tanja_udiv64(a, b, (unsigned long long *)0);
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b)
{
    unsigned long long r = 0;
    tanja_udiv64(a, b, &r);
    return r;
}

long long __divdi3(long long a, long long b)
{
    int neg = 0;
    unsigned long long ua, ub, r = 0;
    if (a < 0) { neg = !neg; ua = (unsigned long long)(-(a + 1)) + 1ull; }
    else ua = (unsigned long long)a;
    if (b < 0) { neg = !neg; ub = (unsigned long long)(-(b + 1)) + 1ull; }
    else ub = (unsigned long long)b;
    tanja_udiv64(ua, ub, &r);
    return neg ? -(long long)r : (long long)r;
}

long long __moddi3(long long a, long long b)
{
    unsigned long long ua, ub, r = 0;
    if (a < 0) ua = (unsigned long long)(-(a + 1)) + 1ull;
    else ua = (unsigned long long)a;
    if (b < 0) ub = (unsigned long long)(-(b + 1)) + 1ull;
    else ub = (unsigned long long)b;
    tanja_udiv64(ua, ub, &r);
    return (a < 0) ? -(long long)r : (long long)r;
}

unsigned long long __udivmoddi4(unsigned long long a, unsigned long long b,
                               unsigned long long *rem)
{
    unsigned long long r = 0;
    unsigned long long q = tanja_udiv64(a, b, &r);
    if (rem) *rem = r;
    return q;
}

/* ================= TanjaOS program heap =================
 * tcc (and any libc user inside the tcc object) allocates from this
 * fixed arena carved out of the object's BSS.  First-fit with
 * address-ordered coalescing; blocks are split and merged exactly the
 * way a textbook allocator does.  The kernel zero-fills and
 * save_brk-restores this region between execs, so a leak never
 * survives past one tcc invocation. */
#define TANJA_HEAP_SIZE (6u * 1024u * 1024u)

typedef struct tanja_block {
    size_t size;                  /* total block size, header included */
    struct tanja_block *next;     /* address-ordered free/all list    */
    int is_free;
    int pad;
} tanja_block_t;                  /* 16 bytes on i386 */

static uint8_t tanja_heap[TANJA_HEAP_SIZE] __attribute__((aligned(16)));
static tanja_block_t *tanja_blocks;
static int tanja_heap_ready;

static void tanja_heap_init(void)
{
    tanja_blocks = (tanja_block_t *)tanja_heap;
    tanja_blocks->size = TANJA_HEAP_SIZE;
    tanja_blocks->next = (tanja_block_t *)0;
    tanja_blocks->is_free = 1;
    tanja_heap_ready = 1;
}

void *malloc(uint32_t n)
{
    size_t need, take;
    tanja_block_t *b, *nb;

    if (!tanja_heap_ready) tanja_heap_init();
    if (n == 0) n = 1;

    /* round the payload up so every block stays 16-byte aligned */
    need = ((size_t)n + sizeof(tanja_block_t) + 15u) & ~(size_t)15u;

    for (b = tanja_blocks; b; b = b->next) {
        if (!b->is_free || b->size < need) continue;

        /* split if the remainder is worth keeping */
        if (b->size >= need + sizeof(tanja_block_t) + 32u) {
            take = need;
            nb = (tanja_block_t *)((char *)b + take);
            nb->size = b->size - take;
            nb->is_free = 1;
            nb->next = b->next;
            b->next = nb;
            b->size = take;
        }
        b->is_free = 0;
        return (char *)b + sizeof(tanja_block_t);
    }

    errno = ENOMEM;
    return (void *)0;
}

void free(void *p)
{
    tanja_block_t *b, *prev;

    if (!p) return;
    b = (tanja_block_t *)((char *)p - sizeof(tanja_block_t));
    if (b < (tanja_block_t *)tanja_heap ||
        b >= (tanja_block_t *)(tanja_heap + TANJA_HEAP_SIZE)) {
        return; /* not ours (defensive) */
    }
    b->is_free = 1;

    /* coalesce: the list is address-ordered, so one pass merges
     * every run of free neighbours */
    prev = (tanja_block_t *)0;
    for (b = tanja_blocks; b; ) {
        if (b->is_free && b->next && b->next->is_free &&
            (char *)b + b->size == (char *)b->next) {
            b->size += b->next->size;
            b->next = b->next->next;
            continue; /* re-examine b; it may absorb more */
        }
        prev = b;
        b = b->next;
    }
}

void *calloc(uint32_t nmemb, uint32_t size)
{
    void *p;
    uint32_t total;

    if (size && nmemb > (0xFFFFFFFFu / size)) {
        errno = ENOMEM;
        return (void *)0;
    }
    total = nmemb * size;
    p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, uint32_t n)
{
    tanja_block_t *b;
    void *q;
    uint32_t have;

    if (!p) return malloc(n);
    if (n == 0) { free(p); return (void *)0; }

    b = (tanja_block_t *)((char *)p - sizeof(tanja_block_t));
    have = (uint32_t)(b->size - sizeof(tanja_block_t));
    if (have >= n) return p;

    /* grow in place by absorbing a free successor */
    if (b->next && b->next->is_free) {
        size_t merged;
        size_t need = ((size_t)n + sizeof(tanja_block_t) + 15u) & ~(size_t)15u;
        merged = b->size + b->next->size;
        if (merged >= need) {
            b->size = merged;
            b->next = b->next->next;
            if (b->size >= need + sizeof(tanja_block_t) + 32u) {
                tanja_block_t *nb = (tanja_block_t *)((char *)b + need);
                nb->size = b->size - need;
                nb->is_free = 1;
                nb->next = b->next;
                b->next = nb;
                b->size = need;
            }
            return p;
        }
    }

    q = malloc(n);
    if (!q) return (void *)0;
    memcpy(q, p, have < n ? have : n);
    free(p);
    return q;
}
