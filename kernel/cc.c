#include <stdint.h>
#include "../include/cc.h"
#include "../include/fs.h"

extern void putc(char c);
extern int putchar(int c);
extern int getchar(void);
extern int puts(const char* s);
extern int strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, unsigned int n);
extern char* strcpy(char* dst, const char* src);
extern char* strncpy(char* dst, const char* src, unsigned int n);
extern char* strcat(char* dst, const char* src);
extern char* strchr(const char* s, int c);
extern char* strrchr(const char* s, int c);
extern char* strstr(const char* haystack, const char* needle);
extern void* memset(void* dst, int value, unsigned int n);
extern void* memcpy(void* dst, const void* src, unsigned int n);
extern void* memmove(void* dst, const void* src, unsigned int n);
extern int memcmp(const void* a, const void* b, unsigned int n);
extern int atoi(const char* s);
extern int abs(int n);
extern int rand(void);
extern void srand(unsigned int seed);
extern int isdigit(int c);
extern int isalpha(int c);
extern int isalnum(int c);
extern int islower(int c);
extern int isupper(int c);
extern int isspace(int c);
extern int isxdigit(int c);
extern int tolower(int c);
extern int toupper(int c);
extern void clear_screen(void);
extern void print(const char* s);
extern void print_dec(uint32_t n);
extern int cc_strcmp_bridge(const char*, const char*);
extern int cc_strncmp_bridge(unsigned int, const char*, const char*);
extern char* cc_strcpy_bridge(const char*, char*);
extern char* cc_strncpy_bridge(unsigned int, const char*, char*);
extern char* cc_strcat_bridge(const char*, char*);
extern char* cc_strchr_bridge(int, const char*);
extern char* cc_strrchr_bridge(int, const char*);
extern char* cc_strstr_bridge(const char*, const char*);
extern void* cc_memset_bridge(unsigned int, int, void*);
extern void* cc_memcpy_bridge(unsigned int, const void*, void*);
extern void* cc_memmove_bridge(unsigned int, const void*, void*);
extern int cc_memcmp_bridge(unsigned int, const void*, const void*);
extern int cc_printf_1(const char*); extern int cc_printf_2(unsigned int,const char*);
extern int cc_printf_3(unsigned int,unsigned int,const char*); extern int cc_printf_4(unsigned int,unsigned int,unsigned int,const char*);
extern int cc_printf_5(unsigned int,unsigned int,unsigned int,unsigned int,const char*);
extern int cc_printf_6(unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,const char*);
extern int cc_printf_7(unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,const char*);
extern int cc_printf_8(unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,const char*);
extern int cc_printf_9(unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,const char*);
extern int cc_scanf_2(unsigned int,const char*);
// Float-argument printf bridges (see printf's "use_float_printf" path
// below): value arguments are passed as real 8-byte doubles rather
// than 4-byte ints, so these need their own bridge family. Only 0-2
// float value arguments are supported per call.
extern int cc_printf_f2(double,const char*);
extern int cc_printf_f3(double,double,const char*);
extern int cc_printf_f4(double,double,double,const char*);
extern int cc_printf_mixed(const char*, int, uint32_t, const uint64_t*);

#define CC_HEAP_SIZE 65536
static uint8_t cc_heap[CC_HEAP_SIZE];
static uint32_t cc_heap_pos;
static char cc_tok_state[256];
static const char* cc_tok_next;

static void* cc_malloc(unsigned int n) {
    /* Header stores the requested size for realloc. */
    uint32_t total = n + 8;
    uint32_t pos = (cc_heap_pos + 3u) & ~3u;
    if (n == 0) n = 1;
    total = n + 8;
    if (pos + total > CC_HEAP_SIZE) return (void*)0;
    *(uint32_t*)(void*)(cc_heap + pos) = n;
    *(uint32_t*)(void*)(cc_heap + pos + 4) = 0x434D414Cull;
    cc_heap_pos = pos + total;
    return (void*)(cc_heap + pos + 8);
}

static void cc_free(void* p) {
    (void)p;
}

static void* cc_calloc(unsigned int count, unsigned int size) {
    if (count != 0 && size > 0xFFFFFFFFu / count) return (void*)0;
    unsigned int total = count * size;
    void* p = cc_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void* cc_realloc(void* p, unsigned int n) {
    if (!p) return cc_malloc(n);
    if (n == 0) { cc_free(p); return (void*)0; }
    uint8_t* b = (uint8_t*)p;
    if (b < cc_heap + 8 || b >= cc_heap + CC_HEAP_SIZE) return (void*)0;
    uint32_t old = *(uint32_t*)(void*)(b - 8);
    void* q = cc_malloc(n);
    if (!q) return (void*)0;
    unsigned int copy = old < n ? old : n;
    memcpy(q, p, copy);
    return q;
}

static long cc_atol(const char* s) { return (long)atoi(s); }
static long cc_labs(long n) { return n < 0 ? -n : n; }

static long cc_strtol(const char* s, char** endp, int base) {
    const char* p = s;
    int neg = 0;
    unsigned long v = 0;
    int digit;
    if (!p) { if (endp) *endp = (char*)s; return 0; }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') base = 8;
        else base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    const char* start = p;
    while (*p) {
        char c = *p;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        v = v * (unsigned)base + (unsigned)digit;
        p++;
    }
    if (endp) *endp = (char*)(p == start ? s : p);
    return neg ? -(long)v : (long)v;
}

static unsigned long cc_strtoul(const char* s, char** endp, int base) {
    return (unsigned long)cc_strtol(s, endp, base);
}

static char* cc_strncat(char* dst, const char* src, unsigned int n) {
    unsigned int i = 0, d = (unsigned int)strlen(dst);
    while (i < n && src[i]) { dst[d++] = src[i++]; }
    dst[d] = 0;
    return dst;
}

static void* cc_memchr(const void* s, int c, unsigned int n) {
    const unsigned char* p = (const unsigned char*)s;
    for (unsigned int i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void*)(p + i);
    return (void*)0;
}

static unsigned int cc_strspn(const char* s, const char* accept) {
    unsigned int n = 0;
    while (s[n]) { int found = 0; for (unsigned int j = 0; accept[j]; j++) if (s[n] == accept[j]) { found = 1; break; } if (!found) break; n++; }
    return n;
}

static unsigned int cc_strcspn(const char* s, const char* reject) {
    unsigned int n = 0;
    while (s[n]) { int found = 0; for (unsigned int j = 0; reject[j]; j++) if (s[n] == reject[j]) { found = 1; break; } if (found) break; n++; }
    return n;
}

static char* cc_strpbrk(const char* s, const char* accept) {
    while (*s) { for (unsigned int j = 0; accept[j]; j++) if (*s == accept[j]) return (char*)s; s++; }
    return (char*)0;
}

static char* cc_strtok(char* s, const char* delim) {
    if (s) cc_tok_next = s;
    if (!cc_tok_next) return (char*)0;
    while (*cc_tok_next) { int d = 0; for (unsigned int j = 0; delim[j]; j++) if (*cc_tok_next == delim[j]) { d = 1; break; } if (!d) break; cc_tok_next++; }
    if (!*cc_tok_next) { cc_tok_next = (const char*)0; return (char*)0; }
    char* out = (char*)cc_tok_next;
    while (*cc_tok_next) { int d = 0; for (unsigned int j = 0; delim[j]; j++) if (*cc_tok_next == delim[j]) { d = 1; break; } if (d) { *((char*)cc_tok_next) = 0; cc_tok_next++; return out; } cc_tok_next++; }
    cc_tok_next = (const char*)0;
    return out;
}

static char* cc_strdup(const char* s) {
    unsigned int n = strlen(s) + 1;
    char* p = (char*)cc_malloc(n);
    if (!p) return (char*)0;
    memcpy(p, s, n);
    return p;
}

static char* cc_strndup(const char* s, unsigned int n) {
    unsigned int len = strlen(s); if (len > n) len = n;
    char* p = (char*)cc_malloc(len + 1);
    if (!p) return (char*)0;
    memcpy(p, s, len); p[len] = 0; return p;
}

static int cc_isgraph(int c) { return c > 32 && c < 127; }
static int cc_isprint(int c) { return c >= 32 && c < 127; }
static int cc_ispunct(int c) { return cc_isgraph(c) && !isalnum(c); }
static int cc_iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }

static int cc_strcoll(const char* a, const char* b) { return strcmp(a, b); }

static unsigned int cc_strxfrm(char* dst, const char* src, unsigned int n) {
    unsigned int len = strlen(src);
    if (n) { unsigned int copy = len < (n - 1) ? len : (n - 1); memcpy(dst, src, copy); dst[copy] = 0; }
    return len;
}

static int cc_remove(const char* path) {
    if (fs_file_exists(path)) return fs_delete_file(path) == 0 ? 0 : -1;
    if (fs_directory_exists(path)) return fs_delete_directory(path) == 0 ? 0 : -1;
    return -1;
}

static void cc_perror(const char* s) {
    if (s && *s) { puts(s); }
    puts("error");
}

static char* cc_strerror(int err) {
    static char msg[32];
    if (err == 0) { strcpy(msg, "no error"); return msg; }
    if (err == 1) { strcpy(msg, "operation failed"); return msg; }
    strcpy(msg, "error");
    return msg;
}

static void cc_heap_reset(void) { cc_heap_pos = 0; cc_tok_next = (const char*)0; }

/*
 * Generic TanjaOS shell-command bridge.
 *
 * Every command in bin/ is registered with the shell at boot, so the C
 * compiler does not need to keep a second hard-coded list of commands.
 * `cmd_foo(...)` is translated into a normal shell invocation of `foo`.
 *
 * Arguments are collected as either numbers or C strings.  String literals
 * are marked by the compiler; numeric expressions are rendered in decimal.
 * The shell command itself performs the normal command-specific parsing.
 */
#define CC_CMD_MAX_ARGS 8
#define CC_CMD_LINE_SIZE 512
static uint32_t cc_cmd_values[CC_CMD_MAX_ARGS];
static uint8_t cc_cmd_string[CC_CMD_MAX_ARGS];
static int cc_cmd_arg_count;
static char cc_cmd_name[64];
static char cc_cmd_line[CC_CMD_LINE_SIZE];

extern void execute_command(const char* cmd_line);
static int str_eq(const char* a, const char* b);

static void cc_cmd_begin(const char* name) {
    int i = 0;
    while (name[i] && i < (int)sizeof(cc_cmd_name) - 1) {
        cc_cmd_name[i] = name[i];
        i++;
    }
    cc_cmd_name[i] = 0;
    cc_cmd_arg_count = 0;
}

static void cc_cmd_set_arg(unsigned int index, unsigned int value,
                           unsigned int is_string) {
    if (index >= CC_CMD_MAX_ARGS) return;
    cc_cmd_values[index] = value;
    cc_cmd_string[index] = is_string ? 1 : 0;
    if ((int)index + 1 > cc_cmd_arg_count)
        cc_cmd_arg_count = (int)index + 1;
}

static void cc_cmd_append_char(int* pos, char c) {
    if (*pos < CC_CMD_LINE_SIZE - 1)
        cc_cmd_line[(*pos)++] = c;
}

static void cc_cmd_append_text(int* pos, const char* s) {
    while (*s && *pos < CC_CMD_LINE_SIZE - 1)
        cc_cmd_line[(*pos)++] = *s++;
}

static void cc_cmd_append_uint(int* pos, uint32_t v) {
    char tmp[11];
    int n = 0;
    if (v == 0) {
        cc_cmd_append_char(pos, '0');
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n--)
        cc_cmd_append_char(pos, tmp[n]);
}

static void cc_cmd_append_string(int* pos, const char* s, int quote) {
    if (!s) s = "";

    if (!quote) {
        cc_cmd_append_text(pos, s);
        return;
    }

    cc_cmd_append_char(pos, '"');
    while (*s && *pos < CC_CMD_LINE_SIZE - 2) {
        if (*s == '"' || *s == '\\')
            cc_cmd_append_char(pos, '\\');
        cc_cmd_append_char(pos, *s++);
    }
    cc_cmd_append_char(pos, '"');
}

static void cc_cmd_end(void) {
    int pos = 0;
    int i;

    cc_cmd_line[0] = 0;
    cc_cmd_append_text(&pos, cc_cmd_name);

    for (i = 0; i < cc_cmd_arg_count; i++) {
        int quote = 0;
        const char* s;

        cc_cmd_append_char(&pos, ' ');

        if (!cc_cmd_string[i]) {
            cc_cmd_append_uint(&pos, cc_cmd_values[i]);
            continue;
        }

        s = (const char*)(uintptr_t)cc_cmd_values[i];

        /*
         * `read` expects a literal variable name, not shell-style quotes.
         * For other commands, quote strings containing whitespace or
         * characters that the command parsers commonly treat specially.
         */
        if (!str_eq(cc_cmd_name, "read") && s) {
            const char* p = s;
            while (*p) {
                if (*p == ' ' || *p == '\t' || *p == '\n' ||
                    *p == '"' || *p == '\\') {
                    quote = 1;
                    break;
                }
                p++;
            }
        }

        cc_cmd_append_string(&pos, s, quote);
    }

    cc_cmd_line[pos] = 0;
    execute_command(cc_cmd_line);
}

// ------------------------------------------------------------
// Code buffer
// ------------------------------------------------------------

#define CODE_BUF_SIZE 32768
#define TJBIN_HEADER_SIZE 12
static uint8_t code_buf[CODE_BUF_SIZE];
static uint32_t code_len;

static void emit_u8(uint8_t b) {
    if (code_len < CODE_BUF_SIZE) code_buf[code_len++] = b;
}
static void emit_u32(uint32_t v) {
    emit_u8((uint8_t)(v & 0xFF));
    emit_u8((uint8_t)((v >> 8) & 0xFF));
    emit_u8((uint8_t)((v >> 16) & 0xFF));
    emit_u8((uint8_t)((v >> 24) & 0xFF));
}
static void patch_u32(uint32_t at, uint32_t v) {
    if (at + 4 > CODE_BUF_SIZE) return;
    code_buf[at]     = (uint8_t)(v & 0xFF);
    code_buf[at + 1] = (uint8_t)((v >> 8) & 0xFF);
    code_buf[at + 2] = (uint8_t)((v >> 16) & 0xFF);
    code_buf[at + 3] = (uint8_t)((v >> 24) & 0xFF);
}

// ------------------------------------------------------------
// Error handling: first error wins, checked after every sub-parse.
// ------------------------------------------------------------

static int cc_error_flag = 0;
static int cc_error_line = 0;
static char cc_error_msg[128];

static void cc_seterr(int line, const char* msg) {
    if (cc_error_flag) return;
    cc_error_flag = 1;
    cc_error_line = line;
    int i = 0;
    while (msg[i] && i < 127) { cc_error_msg[i] = msg[i]; i++; }
    cc_error_msg[i] = 0;
}

// ------------------------------------------------------------
// Lexer
// ------------------------------------------------------------

typedef enum {
    T_EOF, T_NUM, T_IDENT, T_STR,
    T_INT, T_VOID, T_CHAR, T_CONST, T_UNSIGNED, T_SIGNED, T_LONG, T_SHORT,
    T_FLOAT, T_DOUBLE,
    T_IF, T_ELSE, T_WHILE, T_DO, T_FOR, T_RETURN, T_BREAK, T_CONTINUE,
    T_SIZEOF, T_GOTO, T_SWITCH, T_CASE, T_DEFAULT,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET, T_SEMI, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_AMP,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_ANDAND, T_OROR, T_NOT,
    T_BITAND, T_BITOR, T_XOR, T_SHL, T_SHR, T_TILDE, T_QUESTION, T_COLON,
    T_INC, T_DEC, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_ANDEQ, T_OREQ, T_XOREQ, T_SHLEQ, T_SHREQ
} token_type_t;

typedef struct {
    token_type_t type;
    int32_t ival;
    double fval;
    int is_float_lit;   // this T_NUM is actually a floating-point literal
    int is_float_single; // literal had an 'f'/'F' suffix (float, not double)
    char sval[256];
    int line;
    uint32_t start_pos;
} token_t;

static const char* src;
static uint32_t src_len;
static uint32_t src_pos;
static int cur_line;
static token_t cur_tok;
static uint32_t token_count;

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}
static int str_starts_with(const char* s, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static int peekc(void) { return src_pos < src_len ? (unsigned char)src[src_pos] : -1; }
static int getc_src(void) {
    if (src_pos >= src_len) return -1;
    int c = (unsigned char)src[src_pos++];
    if (c == '\n') cur_line++;
    return c;
}

static void skip_ws_comments(void) {
    for (;;) {
        int c = peekc();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { getc_src(); continue; }
        if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '/') {
            while (peekc() != -1 && peekc() != '\n') getc_src();
            continue;
        }
        if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '*') {
            getc_src(); getc_src();
            while (peekc() != -1 && !(peekc() == '*' && src_pos + 1 < src_len && src[src_pos + 1] == '/')) getc_src();
            if (peekc() != -1) { getc_src(); getc_src(); }
            continue;
        }
        break;
    }
}

static void next_token(void) {
    token_count++;
    if (token_count > 200000) { cc_seterr(cur_line, "input too complex / possibly unterminated construct"); cur_tok.type = T_EOF; return; }

    skip_ws_comments();
    cur_tok.start_pos = src_pos;
    cur_tok.line = cur_line;
    cur_tok.sval[0] = 0;
    cur_tok.is_float_lit = 0;
    cur_tok.is_float_single = 0;

    int c = peekc();
    if (c == -1) { cur_tok.type = T_EOF; return; }

    if (c == '.' && src_pos + 1 < src_len && src[src_pos + 1] >= '0' && src[src_pos + 1] <= '9') {
        // A float literal with no leading digit, e.g. ".5" (== 0.5) -
        // valid in real C, so handled here before the digit-only path.
        getc_src(); // consume '.'
        double frac = 0.0;
        double scale = 0.1;
        while (peekc() >= '0' && peekc() <= '9') {
            frac += (double)(getc_src() - '0') * scale;
            scale *= 0.1;
        }
        cur_tok.fval = frac;
        cur_tok.is_float_lit = 1;
        if (peekc() == 'f' || peekc() == 'F') { getc_src(); cur_tok.is_float_single = 1; }
        cur_tok.type = T_NUM;
        cur_tok.ival = (int32_t)cur_tok.fval;
        return;
    }

    if (c >= '0' && c <= '9') {
        int32_t v = 0;

        if (c == '0' && src_pos + 1 < src_len &&
            (src[src_pos + 1] == 'x' || src[src_pos + 1] == 'X')) {
            getc_src(); getc_src();
            while (1) {
                int h = peekc();
                int digit = -1;
                if (h >= '0' && h <= '9') digit = h - '0';
                else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                if (digit < 0) break;
                v = (v << 4) | digit;
                getc_src();
            }
        } else if (c == '0' && src_pos + 1 < src_len &&
                   (src[src_pos + 1] == 'b' || src[src_pos + 1] == 'B')) {
            getc_src(); getc_src();
            while (peekc() == '0' || peekc() == '1') {
                v = (v << 1) | (getc_src() - '0');
            }
        } else {
            while (peekc() >= '0' && peekc() <= '9')
                v = v * 10 + (getc_src() - '0');

            if (peekc() == '.') {
                // Floating-point literal: v '.' fractional-digits ['f'|'F']
                getc_src(); // consume '.'
                double frac = 0.0;
                double scale = 0.1;
                while (peekc() >= '0' && peekc() <= '9') {
                    frac += (double)(getc_src() - '0') * scale;
                    scale *= 0.1;
                }
                cur_tok.fval = (double)v + frac;
                cur_tok.is_float_lit = 1;
                if (peekc() == 'f' || peekc() == 'F') { getc_src(); cur_tok.is_float_single = 1; }
                cur_tok.type = T_NUM;
                cur_tok.ival = (int32_t)cur_tok.fval;
                return;
            }
        }

        cur_tok.type = T_NUM; cur_tok.ival = v; return;
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int i = 0;
        while ((peekc() >= 'a' && peekc() <= 'z') || (peekc() >= 'A' && peekc() <= 'Z') ||
               (peekc() >= '0' && peekc() <= '9') || peekc() == '_') {
            if (i < 255) cur_tok.sval[i++] = (char)getc_src(); else getc_src();
        }
        cur_tok.sval[i] = 0;
        if (str_eq(cur_tok.sval, "int")) cur_tok.type = T_INT;
        else if (str_eq(cur_tok.sval, "void")) cur_tok.type = T_VOID;
        else if (str_eq(cur_tok.sval, "char")) cur_tok.type = T_CHAR;
        else if (str_eq(cur_tok.sval, "const")) cur_tok.type = T_CONST;
        else if (str_eq(cur_tok.sval, "unsigned")) cur_tok.type = T_UNSIGNED;
        else if (str_eq(cur_tok.sval, "signed")) cur_tok.type = T_SIGNED;
        else if (str_eq(cur_tok.sval, "long")) cur_tok.type = T_LONG;
        else if (str_eq(cur_tok.sval, "short")) cur_tok.type = T_SHORT;
        else if (str_eq(cur_tok.sval, "float")) cur_tok.type = T_FLOAT;
        else if (str_eq(cur_tok.sval, "double")) cur_tok.type = T_DOUBLE;
        else if (str_eq(cur_tok.sval, "if")) cur_tok.type = T_IF;
        else if (str_eq(cur_tok.sval, "else")) cur_tok.type = T_ELSE;
        else if (str_eq(cur_tok.sval, "while")) cur_tok.type = T_WHILE;
        else if (str_eq(cur_tok.sval, "do")) cur_tok.type = T_DO;
        else if (str_eq(cur_tok.sval, "for")) cur_tok.type = T_FOR;
        else if (str_eq(cur_tok.sval, "return")) cur_tok.type = T_RETURN;
        else if (str_eq(cur_tok.sval, "break")) cur_tok.type = T_BREAK;
        else if (str_eq(cur_tok.sval, "continue")) cur_tok.type = T_CONTINUE;
        else if (str_eq(cur_tok.sval, "sizeof")) cur_tok.type = T_SIZEOF;
        else if (str_eq(cur_tok.sval, "goto")) cur_tok.type = T_GOTO;
        else if (str_eq(cur_tok.sval, "switch")) cur_tok.type = T_SWITCH;
        else if (str_eq(cur_tok.sval, "case")) cur_tok.type = T_CASE;
        else if (str_eq(cur_tok.sval, "default")) cur_tok.type = T_DEFAULT;
        else if (str_eq(cur_tok.sval, "NULL") || str_eq(cur_tok.sval, "false")) {
            cur_tok.type = T_NUM; cur_tok.ival = 0;
        } else if (str_eq(cur_tok.sval, "true")) {
            cur_tok.type = T_NUM; cur_tok.ival = 1;
        } else cur_tok.type = T_IDENT;
        return;
    }

    if (c == '"') {
        getc_src();
        int i = 0;
        while (peekc() != -1 && peekc() != '"') {
            int ch = getc_src();
            if (ch == '\\') {
                int e = getc_src();
                if (e == 'n') ch = '\n';
                else if (e == 'r') ch = '\r';
                else if (e == 't') ch = '\t';
                else if (e == '0') ch = '\0';
                else if (e == '\\') ch = '\\';
                else if (e == '"') ch = '"';
                else ch = e;
            }
            if (i < 255) cur_tok.sval[i++] = (char)ch;
        }
        if (peekc() == '"') getc_src();
        cur_tok.sval[i] = 0;
        cur_tok.type = T_STR;
        return;
    }

    if (c == '\'') {
        getc_src();
        int ch = getc_src();
        if (ch == '\\') {
            int e = getc_src();
            if (e == 'n') ch = '\n';
            else if (e == 'r') ch = '\r';
            else if (e == 't') ch = '\t';
            else if (e == '0') ch = '\0';
            else if (e == '\\') ch = '\\';
            else if (e == '\'') ch = '\'';
            else ch = e;
        }
        if (peekc() == '\'') getc_src();
        cur_tok.type = T_NUM; cur_tok.ival = ch; return;
    }

    getc_src();
    switch (c) {
        case '(': cur_tok.type = T_LPAREN; return;
        case ')': cur_tok.type = T_RPAREN; return;
        case '{': cur_tok.type = T_LBRACE; return;
        case '}': cur_tok.type = T_RBRACE; return;
        case '[': cur_tok.type = T_LBRACKET; return;
        case ']': cur_tok.type = T_RBRACKET; return;
        case ';': cur_tok.type = T_SEMI; return;
        case ',': cur_tok.type = T_COMMA; return;
        case '+':
            if (peekc() == '+') { getc_src(); cur_tok.type = T_INC; return; }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_PLUSEQ; return; }
            cur_tok.type = T_PLUS; return;
        case '-':
            if (peekc() == '-') { getc_src(); cur_tok.type = T_DEC; return; }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_MINUSEQ; return; }
            cur_tok.type = T_MINUS; return;
        case '*':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_STAREQ; return; }
            cur_tok.type = T_STAR; return;
        case '/':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_SLASHEQ; return; }
            cur_tok.type = T_SLASH; return;
        case '%':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_PERCENTEQ; return; }
            cur_tok.type = T_PERCENT; return;
        case '=':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_EQ; } else cur_tok.type = T_ASSIGN;
            return;
        case '!':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_NE; } else cur_tok.type = T_NOT;
            return;
        case '<':
            if (peekc() == '<') {
                getc_src();
                if (peekc() == '=') { getc_src(); cur_tok.type = T_SHLEQ; return; }
                cur_tok.type = T_SHL; return;
            }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_LE; } else cur_tok.type = T_LT;
            return;
        case '>':
            if (peekc() == '>') {
                getc_src();
                if (peekc() == '=') { getc_src(); cur_tok.type = T_SHREQ; return; }
                cur_tok.type = T_SHR; return;
            }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_GE; } else cur_tok.type = T_GT;
            return;
        case '&':
            if (peekc() == '&') { getc_src(); cur_tok.type = T_ANDAND; return; }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_ANDEQ; return; }
            cur_tok.type = T_BITAND; return;
        case '|':
            if (peekc() == '|') { getc_src(); cur_tok.type = T_OROR; return; }
            if (peekc() == '=') { getc_src(); cur_tok.type = T_OREQ; return; }
            cur_tok.type = T_BITOR; return;
        case '^':
            if (peekc() == '=') { getc_src(); cur_tok.type = T_XOREQ; return; }
            cur_tok.type = T_XOR; return;
        case '~': cur_tok.type = T_TILDE; return;
        case '?': cur_tok.type = T_QUESTION; return;
        case ':': cur_tok.type = T_COLON; return;
        default:
            cc_seterr(cur_line, "unexpected character in source");
            cur_tok.type = T_EOF; return;
    }
}

typedef struct { uint32_t pos; int line; token_t tok; } lexer_state_t;
static lexer_state_t lexer_save(void) {
    lexer_state_t s; s.pos = src_pos; s.line = cur_line; s.tok = cur_tok; return s;
}
static void lexer_restore(lexer_state_t s) {
    src_pos = s.pos; cur_line = s.line; cur_tok = s.tok;
}

static void expect(token_type_t t, const char* what) {
    if (cc_error_flag) return;
    if (cur_tok.type != t) { cc_seterr(cur_tok.line, what); return; }
    next_token();
}

// ------------------------------------------------------------
// Symbol tables
// ------------------------------------------------------------

#define MAX_LOCALS 48
typedef struct { char name[32]; int32_t offset; int is_array; int array_len; int elem_size; int ptr_depth; int is_fp; int fp_double; } local_t;
static local_t locals[MAX_LOCALS];
static int local_count;
static int next_local_offset;
static int next_param_offset;

#define MAX_FUNCS 24
typedef struct { char name[32]; uint32_t addr; int nparams; } func_t;
static func_t funcs[MAX_FUNCS];
static int func_count;
static uint32_t cur_func_start; // for self-recursion

// Calls to a function that has only been prototyped so far (declared,
// not yet defined) can't be resolved to a real address at the call
// site. A placeholder call is emitted instead, and the patch is
// resolved once every top-level declaration has been parsed (see
// cc_resolve_pending_calls). Real C requires a prior prototype or
// definition to call a function at all - an entirely unknown name is
// still a hard "call to undefined function" error, matching this.
#define MAX_PENDING_CALLS 32
typedef struct { uint32_t patch_at; int func_index; int line; } pending_call_t;
static pending_call_t pending_calls[MAX_PENDING_CALLS];
static int pending_call_count;

#define MAX_GLOBALS 24
typedef struct { char name[32]; uint32_t addr; int is_array; int array_len; int elem_size; int ptr_depth; int is_fp; int fp_double; } global_t;
static int32_t globals_data[MAX_GLOBALS];
static double globals_fp_init[MAX_GLOBALS]; // initializer value for float/double globals, reapplied by the entry wrapper
static global_t globals[MAX_GLOBALS];
static int global_count;

// Backing storage for global ARRAYS (globals_data above is one slot
// per scalar global; arrays need contiguous space). Tracked in raw
// BYTES (not elements) so char arrays can be genuinely byte-packed,
// matching real C memory layout, while int arrays still use 4 bytes
// per element.
#define GLOBAL_ARRAY_POOL_BYTES 4096
static uint8_t global_array_pool[GLOBAL_ARRAY_POOL_BYTES];
static int global_array_pool_used;

// break/continue support: tracks the innermost enclosing loop so
// break/continue statements know where to jump. Nested loops save and
// restore the previous context around their body (matches the natural
// LIFO nesting of the recursive-descent parser itself).
#define MAX_BREAK_PATCHES 16
typedef struct {
    uint32_t break_patches[MAX_BREAK_PATCHES];
    int break_count;
    uint32_t continue_patches[MAX_BREAK_PATCHES];
    int continue_count;
    int continue_target_known; // true for while (target = loop_start,
                                // known immediately); false for for
                                // (target = increment code, which is
                                // only known after the body is parsed
                                // - see the for-loop handler)
    uint32_t continue_target;
} loop_ctx_t;
static loop_ctx_t* cur_loop = 0;

// Nesting-order counters so a `break` inside a switch nested in a loop
// (or vice versa) resolves to whichever construct is actually
// innermost, while `continue` always stays keyed to the loop only.
static int nest_seq_counter;
static int cur_loop_seq = -1;
static int cur_switch_seq = -1;

#define MAX_SWITCH_CASES 32
typedef struct {
    int32_t case_values[MAX_SWITCH_CASES];
    uint32_t case_addrs[MAX_SWITCH_CASES];
    int case_count;
    int has_default;
    uint32_t default_addr;
    uint32_t break_patches[MAX_BREAK_PATCHES];
    int break_count;
    int32_t value_slot; // ebp-relative slot holding the switch expression
} switch_ctx_t;
static switch_ctx_t* cur_switch = 0;

// goto/label support: labels are function-scoped, reset per function.
#define MAX_LABELS 16
typedef struct { char name[32]; uint32_t addr; int defined; } label_t;
static label_t func_labels[MAX_LABELS];
static int func_label_count;

#define MAX_LABEL_PATCHES 32
typedef struct { char name[32]; uint32_t patch_at; int line; } label_patch_t;
static label_patch_t label_patches[MAX_LABEL_PATCHES];
static int label_patch_count;

static label_t* find_label(const char* name) {
    int i;
    for (i = 0; i < func_label_count; i++) if (str_eq(func_labels[i].name, name)) return &func_labels[i];
    return 0;
}

static local_t* find_local(const char* name) {
    int i;
    for (i = 0; i < local_count; i++) if (str_eq(locals[i].name, name)) return &locals[i];
    return 0;
}
static global_t* find_global(const char* name) {
    int i;
    for (i = 0; i < global_count; i++) if (str_eq(globals[i].name, name)) return &globals[i];
    return 0;
}
static func_t* find_func(const char* name) {
    int i;
    for (i = 0; i < func_count; i++) if (str_eq(funcs[i].name, name)) return &funcs[i];
    return 0;
}

// ------------------------------------------------------------
// x86 code generation helpers
// ------------------------------------------------------------

static void gen_push_eax(void) { emit_u8(0x50); }
static void gen_pop_ecx(void)  { emit_u8(0x59); }
static void gen_mov_eax_imm32(uint32_t v) { emit_u8(0xB8); emit_u32(v); }
static void gen_mov_eax_ebp_disp32(int32_t d) { emit_u8(0x8B); emit_u8(0x85); emit_u32((uint32_t)d); }
static void gen_mov_ebp_disp32_eax(int32_t d) { emit_u8(0x89); emit_u8(0x85); emit_u32((uint32_t)d); }
static void gen_lea_eax_ebp_disp32(int32_t d) { emit_u8(0x8D); emit_u8(0x85); emit_u32((uint32_t)d); }
static void gen_mov_eax_indirect_eax(void) { emit_u8(0x8B); emit_u8(0x00); } // mov eax,[eax]
static void gen_mov_indirect_ecx_eax(void) { emit_u8(0x89); emit_u8(0x01); } // mov [ecx],eax
static void gen_shl_eax_2(void) { emit_u8(0xC1); emit_u8(0xE0); emit_u8(0x02); } // shl eax,2 (i.e. *4)
static void gen_sub_eax_ecx(void) { emit_u8(0x29); emit_u8(0xC8); } // eax -= ecx (direct order, for compound assign)
static void gen_pop_eax(void) { emit_u8(0x58); }
static void gen_mov_eax_esp0(void) { emit_u8(0x8B); emit_u8(0x04); emit_u8(0x24); } // mov eax,[esp] (peek, no pop)
static void gen_mov_eax_abs(uint32_t addr) { emit_u8(0xA1); emit_u32(addr); }
static void gen_mov_abs_eax(uint32_t addr) { emit_u8(0x89); emit_u8(0x05); emit_u32(addr); } // mov [addr], eax
static void gen_mov_abs_imm32(uint32_t addr, uint32_t value) {
    emit_u8(0xC7); emit_u8(0x05); emit_u32(addr); emit_u32(value);
}
static void gen_add_eax_ecx(void) { emit_u8(0x01); emit_u8(0xC8); }
static void gen_and_eax_ecx(void) { emit_u8(0x21); emit_u8(0xC8); }
static void gen_or_eax_ecx(void)  { emit_u8(0x09); emit_u8(0xC8); }
static void gen_xor_eax_ecx(void) { emit_u8(0x31); emit_u8(0xC8); }
static void gen_shl_eax_cl(void)  { emit_u8(0xD3); emit_u8(0xE0); }
static void gen_sar_eax_cl(void)  { emit_u8(0xD3); emit_u8(0xF8); }
static void gen_xchg_eax_ecx(void){ emit_u8(0x91); }
static void gen_not_eax(void)     { emit_u8(0xF7); emit_u8(0xD0); }
static void gen_sub_ecx_eax_then_mov(void) { emit_u8(0x29); emit_u8(0xC1); emit_u8(0x89); emit_u8(0xC8); } // ecx-=eax; eax=ecx
static void gen_imul_eax_ecx(void) { emit_u8(0x0F); emit_u8(0xAF); emit_u8(0xC1); }
static void gen_div_setup_and_idiv(void) {
    // pre: ecx=left, eax=right. want eax=left/right, edx=left%right
    emit_u8(0x89); emit_u8(0xC3); // mov ebx, eax   (ebx = right)
    emit_u8(0x89); emit_u8(0xC8); // mov eax, ecx   (eax = left)
    emit_u8(0x99);                // cdq
    emit_u8(0xF7); emit_u8(0xFB); // idiv ebx
}
static void gen_cmp_ecx_eax(void) { emit_u8(0x39); emit_u8(0xC1); }
static void gen_setcc_movzx(uint8_t setcc_op2) {
    emit_u8(0x0F); emit_u8(setcc_op2); emit_u8(0xC0); // setX al
    emit_u8(0x0F); emit_u8(0xB6); emit_u8(0xC0);      // movzx eax, al
}
static void gen_test_eax_eax(void) { emit_u8(0x85); emit_u8(0xC0); }
static void gen_neg_eax(void) { emit_u8(0xF7); emit_u8(0xD8); }
static void gen_cmp_eax_0(void) { emit_u8(0x3D); emit_u32(0); }
static void gen_cmp_eax_imm32(uint32_t v) { emit_u8(0x3D); emit_u32(v); } // cmp eax, imm32

// ------------------------------------------------------------
// x87 FPU codegen for float/double support. Values flow through the
// FPU stack top (ST(0)) rather than EAX while a float expression is
// being evaluated - a fundamentally different discipline than the
// rest of this compiler, which is why float/double get their own
// declaration/expression/statement code paths instead of being woven
// into the existing int machinery.
// ------------------------------------------------------------
static void gen_fld_ebp_disp32(int32_t d, int is_double) {
    if (is_double) { emit_u8(0xDD); emit_u8(0x85); emit_u32((uint32_t)d); } // fld qword [ebp+d]
    else { emit_u8(0xD9); emit_u8(0x85); emit_u32((uint32_t)d); }          // fld dword [ebp+d]
}
static void gen_fstp_ebp_disp32(int32_t d, int is_double) {
    if (is_double) { emit_u8(0xDD); emit_u8(0x9D); emit_u32((uint32_t)d); } // fstp qword [ebp+d]
    else { emit_u8(0xD9); emit_u8(0x9D); emit_u32((uint32_t)d); }          // fstp dword [ebp+d]
}
static void gen_fld_abs(uint32_t addr, int is_double) {
    if (is_double) { emit_u8(0xDD); emit_u8(0x05); emit_u32(addr); } // fld qword [addr]
    else { emit_u8(0xD9); emit_u8(0x05); emit_u32(addr); }          // fld dword [addr]
}
static void gen_fstp_abs(uint32_t addr, int is_double) {
    if (is_double) { emit_u8(0xDD); emit_u8(0x1D); emit_u32(addr); } // fstp qword [addr]
    else { emit_u8(0xD9); emit_u8(0x1D); emit_u32(addr); }          // fstp dword [addr]
}
static void gen_faddp(void) { emit_u8(0xDE); emit_u8(0xC1); } // ST(1) += ST(0), pop
static void gen_fsubp(void) { emit_u8(0xDE); emit_u8(0xE9); } // ST(1) -= ST(0), pop  -> gives (pushed-first - pushed-second)
static void gen_fmulp(void) { emit_u8(0xDE); emit_u8(0xC9); } // ST(1) *= ST(0), pop
static void gen_fdivp(void) { emit_u8(0xDE); emit_u8(0xF9); } // ST(1) /= ST(0), pop  -> gives (pushed-first / pushed-second)
static void gen_fchs(void)  { emit_u8(0xD9); emit_u8(0xE0); } // negate ST(0)
static void gen_fcompp(void) { emit_u8(0xDE); emit_u8(0xD9); } // compare ST(0),ST(1), pop both
static void gen_fnstsw_ax(void) { emit_u8(0xDF); emit_u8(0xE0); }
static void gen_sahf(void) { emit_u8(0x9E); }

static void gen_int_to_float(void) {
    // EAX (int) -> pushed onto the FPU stack as ST(0), for (float)/
    // (double) casts of an int expression.
    emit_u8(0x50);                               // push eax
    emit_u8(0xDB); emit_u8(0x04); emit_u8(0x24); // fild dword [esp]
    emit_u8(0x83); emit_u8(0xC4); emit_u8(0x04); // add esp, 4
}
static void gen_float_to_int_trunc(void) {
    // ST(0) -> EAX, truncating toward zero (matching C's (int)floatExpr
    // semantics) rather than the FPU's default round-to-nearest, by
    // temporarily swapping in a truncating rounding mode around the
    // conversion and restoring the original control word afterward.
    emit_u8(0x83); emit_u8(0xEC); emit_u8(0x08);                             // sub esp, 8
    emit_u8(0xD9); emit_u8(0x3C); emit_u8(0x24);                             // fnstcw [esp]
    emit_u8(0x66); emit_u8(0x8B); emit_u8(0x04); emit_u8(0x24);              // mov ax, [esp]
    emit_u8(0x66); emit_u8(0x0D); emit_u8(0x00); emit_u8(0x0C);              // or ax, 0x0C00
    emit_u8(0x66); emit_u8(0x89); emit_u8(0x44); emit_u8(0x24); emit_u8(0x02); // mov [esp+2], ax
    emit_u8(0xD9); emit_u8(0x6C); emit_u8(0x24); emit_u8(0x02);              // fldcw [esp+2]
    emit_u8(0xDB); emit_u8(0x5C); emit_u8(0x24); emit_u8(0x04);              // fistp dword [esp+4]
    emit_u8(0xD9); emit_u8(0x2C); emit_u8(0x24);                             // fldcw [esp]
    emit_u8(0x8B); emit_u8(0x44); emit_u8(0x24); emit_u8(0x04);              // mov eax, [esp+4]
    emit_u8(0x83); emit_u8(0xC4); emit_u8(0x08);                             // add esp, 8
}
static void gen_float_relop_to_eax(token_type_t op) {
    // Called immediately after gen_fcompp+gen_fnstsw_ax+gen_sahf, which
    // maps the FPU's C0/C2/C3 compare flags onto CF/PF/ZF the same way
    // an UNSIGNED integer compare would - so the unsigned-flavored
    // SETcc opcodes must be used here (SF/OF aren't reliably set by
    // SAHF, so the signed ones would be wrong).
    if (op == T_GT) gen_setcc_movzx(0x97);      // seta
    else if (op == T_LT) gen_setcc_movzx(0x92); // setb
    else if (op == T_GE) gen_setcc_movzx(0x93); // setae
    else if (op == T_LE) gen_setcc_movzx(0x96); // setbe
    else if (op == T_EQ) gen_setcc_movzx(0x94); // sete
    else gen_setcc_movzx(0x95);                 // setne (T_NE)
}

static uint32_t gen_jz_placeholder(void) {
    emit_u8(0x0F); emit_u8(0x84); uint32_t at = code_len; emit_u32(0); return at;
}
static uint32_t gen_jnz_placeholder(void) {
    emit_u8(0x0F); emit_u8(0x85); uint32_t at = code_len; emit_u32(0); return at;
}
static uint32_t gen_jmp_placeholder(void) {
    emit_u8(0xE9); uint32_t at = code_len; emit_u32(0); return at;
}
static void patch_jump_here(uint32_t at) { patch_u32(at, code_len - (at + 4)); }
static void gen_jmp_to(uint32_t target) {
    emit_u8(0xE9); emit_u32(target - (code_len + 4));
}
static void gen_je_to(uint32_t target) {
    emit_u8(0x0F); emit_u8(0x84); emit_u32(target - (code_len + 4)); // je near
}
static void gen_call_abs(uint32_t target) {
    // `target` is an absolute runtime address (either a native kernel
    // function pointer, or code_buf+offset for a JIT'd function/
    // self-recursive call) - code_len on its own is only a
    // buffer-relative offset, so it must be added to code_buf's own
    // base address before subtracting to get a correct rel32.
    emit_u8(0xE8);
    emit_u32(target - ((uint32_t)code_buf + code_len + 4));
}

// A call to a function that isn't defined yet (only prototyped so
// far). Emits a placeholder displacement and returns the position of
// that 4-byte field, to be resolved later with patch_call_target once
// the real function address is known.
static uint32_t gen_call_placeholder(void) {
    emit_u8(0xE8);
    uint32_t at = code_len;
    emit_u32(0);
    return at;
}
static void patch_call_target(uint32_t at, uint32_t target_absolute) {
    patch_u32(at, target_absolute - ((uint32_t)code_buf + at + 4));
}

// Like patch_jump_here, but patches a jump to a previously-recorded
// target position instead of "wherever code_len happens to be right
// now" - needed for forward gotos, which may be resolved well after
// more code has already been emitted.
static void patch_jump_to_target(uint32_t at, uint32_t target_pos) {
    patch_u32(at, target_pos - (at + 4));
}

// Byte-sized indirect load/store, used when dereferencing a pointer or
// indexing an array whose element type is `char` (1 byte), so that
// memory layout genuinely matches real C - a `char[]` is byte-packed
// and interoperates correctly with real byte-oriented string/memory
// builtins, rather than every element silently occupying 4 bytes.
static void gen_mov_eax_indirect_eax_byte(void) { emit_u8(0x0F); emit_u8(0xB6); emit_u8(0x00); } // movzx eax, byte [eax]
static void gen_mov_indirect_ecx_al(void) { emit_u8(0x88); emit_u8(0x01); } // mov [ecx], al

// ------------------------------------------------------------
// Forward decls
// ------------------------------------------------------------
static void parse_expr(void);
static void parse_stmt(void);
static void parse_float_expr(void);

// ------------------------------------------------------------
// float/double support. Values live on the x87 FPU stack (ST(0))
// rather than in EAX while a float expression is being evaluated, so
// these get their own self-contained grammar instead of being woven
// into the int expression machinery above. Scope, deliberately:
// arithmetic, assignment, comparison in if/while, and int<->float
// casts all work; float function parameters/return values, mixing
// int and float in the same expression without an explicit cast, and
// arrays of float/double are not supported.
// ------------------------------------------------------------

// Constant pool for float/double LITERALS (3.14, 2.0, etc) - x87 has
// no "load immediate" instruction for arbitrary values, so each
// literal's bit pattern is stashed here once at compile time and
// loaded from memory (fld) wherever it's used.
#define FLOAT_CONST_POOL_BYTES 1024
static uint8_t float_const_pool[FLOAT_CONST_POOL_BYTES];
static int float_const_pool_used;

// Fixed scratch memory (not from the pool above, always available,
// never grows) used to shuttle values through memory when the FPU's
// single-stack-of-registers model needs an assist: narrowing a
// double-precision cast result down to float precision, and holding
// one side of a comparison steady while the other side is evaluated.
static uint8_t fp_scratch4[4];
static uint8_t fp_scratch8[8];
static uint8_t printf_fmt_scratch[4]; // stashes printf's format-string pointer across the mixed-args scratch-array build

// Backing storage for float/double GLOBAL VARIABLES (as opposed to
// float_const_pool above, which is for float/double LITERALS).
#define FLOAT_GLOBAL_POOL_BYTES 512
static uint8_t float_global_pool[FLOAT_GLOBAL_POOL_BYTES];
static int float_global_pool_used;

static uint32_t emit_float_const(double v, int is_double) {
    int sz = is_double ? 8 : 4;
    if (float_const_pool_used + sz > FLOAT_CONST_POOL_BYTES) {
        cc_seterr(cur_tok.line, "too many floating-point literals");
        return 0;
    }
    uint32_t addr = (uint32_t)&float_const_pool[float_const_pool_used];
    if (is_double) {
        double dv = v;
        uint8_t* b = (uint8_t*)&dv;
        int i; for (i = 0; i < 8; i++) float_const_pool[float_const_pool_used + i] = b[i];
    } else {
        float fv = (float)v;
        uint8_t* b = (uint8_t*)&fv;
        int i; for (i = 0; i < 4; i++) float_const_pool[float_const_pool_used + i] = b[i];
    }
    float_const_pool_used += sz;
    return addr;
}

// Heuristic, checked only at the very start of an expression: does
// this look like a float-valued expression? Not full type inference -
// just enough to route a declaration initializer, an assignment RHS,
// an if/while condition, or a printf argument to the float grammar
// instead of the int one.
static int looks_like_float_expr(void) {
    if (cur_tok.type == T_NUM && cur_tok.is_float_lit) return 1;
    if (cur_tok.type == T_IDENT) {
        local_t* l = find_local(cur_tok.sval);
        if (l) return l->is_fp;
        global_t* g = find_global(cur_tok.sval);
        if (g) return g->is_fp;
        return 0;
    }
    if (cur_tok.type == T_LPAREN) {
        lexer_state_t save = lexer_save();
        next_token();
        int r = (cur_tok.type == T_FLOAT || cur_tok.type == T_DOUBLE);
        lexer_restore(save);
        return r;
    }
    if (cur_tok.type == T_MINUS) {
        lexer_state_t save = lexer_save();
        next_token();
        int r = looks_like_float_expr();
        lexer_restore(save);
        return r;
    }
    return 0;
}

static void parse_float_primary(void) {
    if (cur_tok.type == T_NUM && cur_tok.is_float_lit) {
        int is_double = !cur_tok.is_float_single;
        uint32_t addr = emit_float_const(cur_tok.fval, is_double);
        if (cc_error_flag) return;
        gen_fld_abs(addr, is_double);
        next_token();
        return;
    }
    if (cur_tok.type == T_NUM) {
        // A plain integer literal used where a float is expected, e.g.
        // `f = 2;` - treated as a double constant.
        uint32_t addr = emit_float_const((double)cur_tok.ival, 1);
        if (cc_error_flag) return;
        gen_fld_abs(addr, 1);
        next_token();
        return;
    }
    if (cur_tok.type == T_MINUS) {
        next_token();
        parse_float_primary();
        if (cc_error_flag) return;
        gen_fchs();
        return;
    }
    if (cur_tok.type == T_LPAREN) {
        next_token();
        if (cur_tok.type == T_FLOAT || cur_tok.type == T_DOUBLE) {
            // (float)/(double) cast of an int expression.
            int want_double = (cur_tok.type == T_DOUBLE);
            next_token();
            expect(T_RPAREN, "expected ')' after cast"); if (cc_error_flag) return;
            parse_expr(); // inner int expr -> eax
            if (cc_error_flag) return;
            gen_int_to_float();
            if (!want_double) {
                // Round-trip through a 4-byte memory location to
                // actually narrow to float precision.
                gen_fstp_abs((uint32_t)fp_scratch4, 0);
                gen_fld_abs((uint32_t)fp_scratch4, 0);
            }
            return;
        }
        parse_float_expr(); // parenthesized float sub-expression
        if (cc_error_flag) return;
        expect(T_RPAREN, "expected ')'");
        return;
    }
    if (cur_tok.type == T_IDENT) {
        local_t* l = find_local(cur_tok.sval);
        global_t* g = l ? 0 : find_global(cur_tok.sval);
        if (l && l->is_fp) { gen_fld_ebp_disp32(l->offset, l->fp_double); next_token(); return; }
        if (g && g->is_fp) { gen_fld_abs(g->addr, g->fp_double); next_token(); return; }
        cc_seterr(cur_tok.line, "expected a float/double variable");
        return;
    }
    cc_seterr(cur_tok.line, "expected a floating-point expression");
}

static void parse_float_term(void) {
    parse_float_primary();
    if (cc_error_flag) return;
    while (cur_tok.type == T_STAR || cur_tok.type == T_SLASH) {
        token_type_t op = cur_tok.type;
        next_token();
        parse_float_primary();
        if (cc_error_flag) return;
        if (op == T_STAR) gen_fmulp(); else gen_fdivp();
    }
}

static void parse_float_expr(void) {
    parse_float_term();
    if (cc_error_flag) return;
    while (cur_tok.type == T_PLUS || cur_tok.type == T_MINUS) {
        token_type_t op = cur_tok.type;
        next_token();
        parse_float_term();
        if (cc_error_flag) return;
        if (op == T_PLUS) gen_faddp(); else gen_fsubp();
    }
}

// Parses `float_expr RELOP float_expr` and leaves a 0/1 result in EAX,
// so the existing if/while codegen (which tests EAX) needs no changes
// at all to accept a float condition.
static void parse_float_condition_to_eax(void) {
    parse_float_expr(); // LHS -> ST(0)
    if (cc_error_flag) return;
    gen_fstp_abs((uint32_t)fp_scratch8, 1); // stash LHS as a double

    token_type_t op = cur_tok.type;
    if (op != T_LT && op != T_GT && op != T_LE && op != T_GE && op != T_EQ && op != T_NE) {
        cc_seterr(cur_tok.line, "expected a comparison operator in a floating-point condition");
        return;
    }
    next_token();

    parse_float_expr(); // RHS -> ST(0)
    if (cc_error_flag) return;
    gen_fld_abs((uint32_t)fp_scratch8, 1); // ST(0)=LHS, ST(1)=RHS
    gen_fcompp();
    gen_fnstsw_ax();
    gen_sahf();
    gen_float_relop_to_eax(op);
}

// Dispatches a condition (used by if/while/do-while/for) to the float
// or int grammar as appropriate. Either path leaves a plain 0/1 (or
// any nonzero-truthy value, for the int path) in EAX, so callers can
// keep using the same test-eax-and-jz pattern either way.
static void parse_condition_to_eax(void) {
    if (looks_like_float_expr()) parse_float_condition_to_eax();
    else parse_expr();
}

// ------------------------------------------------------------
// Expressions (result always ends up in eax)
// ------------------------------------------------------------

// Loads the VALUE of a named variable into eax. Arrays decay to their
// base address (matching normal C array-to-pointer decay when an
// array name is used as a value, e.g. bare "arr" or as a call arg).
static void gen_var_load_to_eax(const char* name, int line) {
    local_t* l = find_local(name);
    if (l) {
        if (l->is_fp) { cc_seterr(line, "float/double value used in an integer expression (not supported - use it in a float expression, comparison, or printf argument)"); return; }
        if (l->is_array) { gen_lea_eax_ebp_disp32(l->offset); return; }
        gen_mov_eax_ebp_disp32(l->offset);
        return;
    }
    global_t* g = find_global(name);
    if (g) {
        if (g->is_fp) { cc_seterr(line, "float/double value used in an integer expression (not supported - use it in a float expression, comparison, or printf argument)"); return; }
        if (g->is_array) { gen_mov_eax_imm32(g->addr); return; }
        gen_mov_eax_abs(g->addr);
        return;
    }
    cc_seterr(line, "undefined variable");
}

// Stores eax into a named variable. Errors if it's an array (whole
// arrays aren't assignable, matching real C).
static void gen_var_store_from_eax(const char* name, int line) {
    local_t* l = find_local(name);
    if (l) {
        if (l->is_array) { cc_seterr(line, "cannot assign to an array"); return; }
        gen_mov_ebp_disp32_eax(l->offset);
        return;
    }
    global_t* g = find_global(name);
    if (g) {
        if (g->is_array) { cc_seterr(line, "cannot assign to an array"); return; }
        gen_mov_abs_eax(g->addr);
        return;
    }
    cc_seterr(line, "undefined variable");
}

// Computes the address of arr[index] into eax. Caller has already
// consumed IDENT and the '[' - this parses the index expression and
// the closing ']'.
static int lookup_elem_size(const char* name) {
    local_t* l = find_local(name);
    if (l) return l->elem_size ? l->elem_size : 4;
    global_t* g = find_global(name);
    if (g) return g->elem_size ? g->elem_size : 4;
    return 4;
}

static void parse_array_index_addr(const char* name, int line) {
    local_t* l = find_local(name);
    global_t* g = l ? 0 : find_global(name);
    if (!l && !g) { cc_seterr(line, "undefined array"); return; }
    int is_array = (l && l->is_array) || (g && g->is_array);
    if (!is_array) { cc_seterr(line, "not an array"); return; }
    int elem_size = l ? l->elem_size : g->elem_size;
    if (elem_size == 0) elem_size = 4;

    if (l) gen_lea_eax_ebp_disp32(l->offset);
    else gen_mov_eax_imm32(g->addr);
    gen_push_eax();
    parse_expr(); // index -> eax
    if (cc_error_flag) return;
    if (elem_size == 4) gen_shl_eax_2(); // else elem_size == 1: index is already a byte offset
    gen_pop_ecx();
    gen_add_eax_ecx(); // eax = base + index*elem_size
    expect(T_RBRACKET, "expected ']'");
}

// Pure token-scan lookahead (emits no code) to check whether the
// upcoming "[ ... ]" is followed by an assignment operator, without
// risking double-emitting code for an index expression that might
// have side effects (e.g. a function call). Caller must have already
// consumed IDENT and be looking at T_LBRACKET. Always restores the
// lexer to wherever it started.
static int peek_is_array_assign(void) {
    lexer_state_t save = lexer_save();
    next_token(); // consume '['
    int depth = 0;
    for (;;) {
        if (cur_tok.type == T_LBRACKET) depth++;
        else if (cur_tok.type == T_RBRACKET) { if (depth == 0) break; depth--; }
        else if (cur_tok.type == T_EOF) { lexer_restore(save); return 0; }
        next_token();
    }
    next_token(); // consume the matching ']'
    int result = (cur_tok.type == T_ASSIGN || cur_tok.type == T_PLUSEQ || cur_tok.type == T_MINUSEQ ||
                  cur_tok.type == T_STAREQ || cur_tok.type == T_SLASHEQ || cur_tok.type == T_PERCENTEQ || cur_tok.type == T_ANDEQ || cur_tok.type == T_OREQ || cur_tok.type == T_XOREQ || cur_tok.type == T_SHLEQ || cur_tok.type == T_SHREQ);
    lexer_restore(save);
    return result;
}

static void skip_one_arg_tokens(void) {
    // Pure token-level skip (no codegen) past one comma-separated call
    // argument, stopping at the top-level comma or closing ')'.
    int depth = 0;
    while (cur_tok.type != T_EOF) {
        if (cur_tok.type == T_LPAREN || cur_tok.type == T_LBRACKET) depth++;
        else if (cur_tok.type == T_RPAREN || cur_tok.type == T_RBRACKET) {
            if (depth == 0) return;
            depth--;
        } else if (cur_tok.type == T_COMMA && depth == 0) {
            return;
        }
        next_token();
    }
}

// Scans a printf call's value arguments (after the format string)
// purely at the token level - no codegen, fully restored afterward -
// classifying each as float-looking (bit set in *out_typemask) or
// not, and counting them into *out_argc.
#define MAX_PRINTF_ARGS 16
static void scan_printf_arg_types(int* out_argc, uint32_t* out_typemask) {
    *out_argc = 0;
    *out_typemask = 0;
    lexer_state_t save = lexer_save();
    skip_one_arg_tokens(); // skip the format string itself
    int idx = 0;
    while (cur_tok.type == T_COMMA && idx < MAX_PRINTF_ARGS) {
        next_token();
        if (looks_like_float_expr()) *out_typemask |= (1u << idx);
        skip_one_arg_tokens();
        idx++;
    }
    *out_argc = idx;
    lexer_restore(save);
}

static void parse_call_args_and_call(func_t* fn, const char* builtin_name) {
    // printf gets dedicated argument-passing paths beyond the default
    // all-int one below:
    //  - all value arguments float-looking: passed as real 8-byte
    //    doubles via the cc_printf_fN bridge family (up to 3 args).
    //  - a MIX of float and int arguments (e.g. printf("%f %d", f, i)):
    //    every argument, whatever its type, is written into an 8-byte
    //    "slot" in a small on-stack scratch array (ints zero-extended,
    //    doubles stored natively), and a bitmask says which slots are
    //    which - cc_printf_mixed reads the array generically using
    //    that mask. This is what actually makes %d and %f coexist in
    //    one call; the homogeneous cases above are simpler/cheaper
    //    special cases of it.
    // Any call with zero float-looking arguments falls through
    // unchanged to the plain int path used by every other function.
    if (builtin_name && str_eq(builtin_name, "printf") && cur_tok.type != T_RPAREN) {
        int pf_argc; uint32_t pf_typemask;
        scan_printf_arg_types(&pf_argc, &pf_typemask);
        uint32_t all_bits = (pf_argc >= 32) ? 0xFFFFFFFFu : ((1u << pf_argc) - 1u);
        int all_float = (pf_argc > 0) && (pf_typemask == all_bits);
        int any_float = (pf_typemask != 0);

        if (all_float) {
            parse_expr(); if (cc_error_flag) return; // format string -> eax
            gen_push_eax();
            int nvalues = 0;
            while (cur_tok.type == T_COMMA) {
                next_token();
                parse_float_expr(); if (cc_error_flag) return;
                emit_u8(0x83); emit_u8(0xEC); emit_u8(0x08); // sub esp, 8
                emit_u8(0xDD); emit_u8(0x1C); emit_u8(0x24); // fstp qword [esp]
                nvalues++;
            }
            expect(T_RPAREN, "expected ')' after arguments"); if (cc_error_flag) return;

            uint32_t target;
            if (nvalues == 0) target = (uint32_t)(void*)cc_printf_1;
            else if (nvalues == 1) target = (uint32_t)(void*)cc_printf_f2;
            else if (nvalues == 2) target = (uint32_t)(void*)cc_printf_f3;
            else if (nvalues == 3) target = (uint32_t)(void*)cc_printf_f4;
            else { cc_seterr(cur_tok.line, "printf supports at most 3 floating-point-only arguments"); return; }
            gen_call_abs(target);

            uint32_t cleanup = 4 + (uint32_t)nvalues * 8; // fmt (4B) + each double (8B)
            emit_u8(0x81); emit_u8(0xC4); emit_u32(cleanup); // add esp, cleanup
            return;
        }

        if (any_float) {
            // Mixed %d/%f call.
            parse_expr(); if (cc_error_flag) return; // format string -> eax
            gen_mov_abs_eax((uint32_t)printf_fmt_scratch);

            if (pf_argc > 0) {
                emit_u8(0x81); emit_u8(0xEC); emit_u32((uint32_t)pf_argc * 8); // sub esp, argc*8
            }
            int k;
            for (k = 0; k < pf_argc; k++) {
                expect(T_COMMA, "expected ',' between printf arguments"); if (cc_error_flag) return;
                if ((pf_typemask >> k) & 1u) {
                    parse_float_expr(); if (cc_error_flag) return;
                    emit_u8(0xDD); emit_u8(0x9C); emit_u8(0x24); emit_u32((uint32_t)k * 8); // fstp qword [esp+k*8]
                } else {
                    parse_expr(); if (cc_error_flag) return;
                    emit_u8(0x89); emit_u8(0x84); emit_u8(0x24); emit_u32((uint32_t)k * 8);       // mov [esp+k*8], eax
                    emit_u8(0xC7); emit_u8(0x84); emit_u8(0x24); emit_u32((uint32_t)k * 8 + 4); emit_u32(0); // mov dword [esp+k*8+4], 0
                }
            }
            expect(T_RPAREN, "expected ')' after arguments"); if (cc_error_flag) return;

            emit_u8(0x8D); emit_u8(0x04); emit_u8(0x24); // lea eax, [esp]   (slots pointer)
            gen_push_eax();
            emit_u8(0x68); emit_u32(pf_typemask);        // push imm32 typemask
            emit_u8(0x68); emit_u32((uint32_t)pf_argc);  // push imm32 argc
            gen_mov_eax_abs((uint32_t)printf_fmt_scratch);
            gen_push_eax();                              // push fmt

            gen_call_abs((uint32_t)(void*)cc_printf_mixed);

            uint32_t cleanup = 4 * 4 + (uint32_t)pf_argc * 8; // 4 call-arg pushes + the scratch array
            emit_u8(0x81); emit_u8(0xC4); emit_u32(cleanup); // add esp, cleanup
            return;
        }
    }

    // consumes '(' already done by caller; parses args up to ')'
    int argc = 0;
    int32_t saved_ival[8];
    (void)saved_ival;
    if (cur_tok.type != T_RPAREN) {
        // We must push args in reverse order for our calling convention,
        // but we only get to parse them left-to-right (single pass), and
        // String arguments are handled by the normal expression machinery.
        // and correct without buffering, we evaluate left-to-right but
        // push immediately, then before the call we've pushed in
        // left-to-right order onto a LIFO stack, which is backwards from
        // what we want. Simplest fix: cap args at 8 and evaluate them
        // into temporary stack slots, then push in reverse.
        // Practically: evaluate each arg (eax), push eax (left to right).
        // Then to call with param1 at [ebp+8], we need the LAST pushed
        // value to be param1 - i.e. push order must be reverse. Since we
        // can't re-order after the fact easily without extra stack
        // shuffling, we instead just define OUR calling convention as
        // "first pushed = last param" and assign local param offsets to
        // match (see function definition parsing) - param offsets are
        // assigned in DECLARATION order matching PUSH order at call
        // sites (left-to-right), so this works out consistently as long
        // as both sides agree, which they do since both use this file.
        for (;;) {
            parse_expr();
            if (cc_error_flag) return;
            gen_push_eax();
            argc++;
            if (cur_tok.type == T_COMMA) { next_token(); continue; }
            break;
        }
    }
    expect(T_RPAREN, "expected ')' after arguments");
    if (cc_error_flag) return;

    if (builtin_name) {
        int expected = -1;
        uint32_t target = 0;
        int returns_value = 1;

        if (str_eq(builtin_name, "printf")) expected = argc;
        else if (str_eq(builtin_name, "scanf")) expected = 2;
        else if (str_eq(builtin_name, "putchar") || str_eq(builtin_name, "strlen") ||
            str_eq(builtin_name, "atoi") || str_eq(builtin_name, "abs") ||
            str_eq(builtin_name, "rand") || str_eq(builtin_name, "getchar") ||
            str_eq(builtin_name, "puts") || str_eq(builtin_name, "isdigit") ||
            str_eq(builtin_name, "isalpha") || str_eq(builtin_name, "isalnum") ||
            str_eq(builtin_name, "islower") || str_eq(builtin_name, "isupper") ||
            str_eq(builtin_name, "isspace") || str_eq(builtin_name, "isxdigit") ||
            str_eq(builtin_name, "tolower") || str_eq(builtin_name, "toupper")) expected = 1;
        else if (str_eq(builtin_name, "srand")) expected = 1;
        else if (str_eq(builtin_name, "clear_screen")) expected = 0;
        else if (str_eq(builtin_name, "strcmp") || str_eq(builtin_name, "strcpy") ||
                 str_eq(builtin_name, "strcat") || str_eq(builtin_name, "strchr") ||
                 str_eq(builtin_name, "strrchr") || str_eq(builtin_name, "strstr")) expected = 2;
        else if (str_eq(builtin_name, "strncmp") || str_eq(builtin_name, "strncpy") ||
                 str_eq(builtin_name, "memset") || str_eq(builtin_name, "memcpy") ||
                 str_eq(builtin_name, "memmove") || str_eq(builtin_name, "memcmp")) expected = 3;
        else if (str_eq(builtin_name, "strcoll")) expected = 2;
        else if (str_eq(builtin_name, "strxfrm")) expected = 3;
        else if (str_eq(builtin_name, "remove") || str_eq(builtin_name, "perror")) expected = 1;
        else if (str_eq(builtin_name, "strncat") || str_eq(builtin_name, "strpbrk") ||
                 str_eq(builtin_name, "strtok") || str_eq(builtin_name, "strdup") ||
                 str_eq(builtin_name, "strndup")) expected = 2;
        else if (str_eq(builtin_name, "memchr") || str_eq(builtin_name, "strspn") ||
                 str_eq(builtin_name, "strcspn") || str_eq(builtin_name, "strtol") ||
                 str_eq(builtin_name, "strtoul")) expected = 3;
        else if (str_eq(builtin_name, "malloc") || str_eq(builtin_name, "free") ||
                 str_eq(builtin_name, "atol") || str_eq(builtin_name, "labs") ||
                 str_eq(builtin_name, "strerror") || str_eq(builtin_name, "isgraph") ||
                 str_eq(builtin_name, "isprint") || str_eq(builtin_name, "ispunct") ||
                 str_eq(builtin_name, "iscntrl")) expected = 1;
        else if (str_eq(builtin_name, "calloc") || str_eq(builtin_name, "realloc")) expected = 2;
        else if (str_eq(builtin_name, "strerror")) expected = 1;
        if (str_eq(builtin_name, "printf")) {
            if (argc < 1 || argc > 9) { cc_seterr(cur_tok.line, "printf supports 1 to 9 arguments"); return; }
        } else if (argc != expected) {
            cc_seterr(cur_tok.line, "wrong number of arguments to builtin");
            return;
        }

        if (str_eq(builtin_name, "printf")) {
            if (argc == 1) target=(uint32_t)(void*)cc_printf_1;
            else if (argc == 2) target=(uint32_t)(void*)cc_printf_2;
            else if (argc == 3) target=(uint32_t)(void*)cc_printf_3;
            else if (argc == 4) target=(uint32_t)(void*)cc_printf_4;
            else if (argc == 5) target=(uint32_t)(void*)cc_printf_5;
            else if (argc == 6) target=(uint32_t)(void*)cc_printf_6;
            else if (argc == 7) target=(uint32_t)(void*)cc_printf_7;
            else if (argc == 8) target=(uint32_t)(void*)cc_printf_8;
            else target=(uint32_t)(void*)cc_printf_9;
        } else if (str_eq(builtin_name, "scanf")) target=(uint32_t)(void*)cc_scanf_2;
        else if (str_eq(builtin_name, "putchar")) target = (uint32_t)(void*)putchar;
        else if (str_eq(builtin_name, "getchar")) target = (uint32_t)(void*)getchar;
        else if (str_eq(builtin_name, "puts")) target = (uint32_t)(void*)puts;
        else if (str_eq(builtin_name, "clear_screen")) { target = (uint32_t)(void*)clear_screen; returns_value = 0; }
        else if (str_eq(builtin_name, "strlen")) target = (uint32_t)(void*)strlen;
        else if (str_eq(builtin_name, "strcmp")) target = (uint32_t)(void*)cc_strcmp_bridge;
        else if (str_eq(builtin_name, "strncmp")) target = (uint32_t)(void*)cc_strncmp_bridge;
        else if (str_eq(builtin_name, "strcpy")) target = (uint32_t)(void*)cc_strcpy_bridge;
        else if (str_eq(builtin_name, "strncpy")) target = (uint32_t)(void*)cc_strncpy_bridge;
        else if (str_eq(builtin_name, "strcat")) target = (uint32_t)(void*)cc_strcat_bridge;
        else if (str_eq(builtin_name, "strchr")) target = (uint32_t)(void*)cc_strchr_bridge;
        else if (str_eq(builtin_name, "strrchr")) target = (uint32_t)(void*)cc_strrchr_bridge;
        else if (str_eq(builtin_name, "strstr")) target = (uint32_t)(void*)cc_strstr_bridge;
        else if (str_eq(builtin_name, "memset")) target = (uint32_t)(void*)cc_memset_bridge;
        else if (str_eq(builtin_name, "memcpy")) target = (uint32_t)(void*)cc_memcpy_bridge;
        else if (str_eq(builtin_name, "memmove")) target = (uint32_t)(void*)cc_memmove_bridge;
        else if (str_eq(builtin_name, "memcmp")) target = (uint32_t)(void*)cc_memcmp_bridge;
        else if (str_eq(builtin_name, "atoi")) target = (uint32_t)(void*)atoi;
        else if (str_eq(builtin_name, "abs")) target = (uint32_t)(void*)abs;
        else if (str_eq(builtin_name, "rand")) target = (uint32_t)(void*)rand;
        else if (str_eq(builtin_name, "srand")) { target = (uint32_t)(void*)srand; returns_value = 0; }
        else if (str_eq(builtin_name, "isdigit")) target = (uint32_t)(void*)isdigit;
        else if (str_eq(builtin_name, "isalpha")) target = (uint32_t)(void*)isalpha;
        else if (str_eq(builtin_name, "isalnum")) target = (uint32_t)(void*)isalnum;
        else if (str_eq(builtin_name, "islower")) target = (uint32_t)(void*)islower;
        else if (str_eq(builtin_name, "isupper")) target = (uint32_t)(void*)isupper;
        else if (str_eq(builtin_name, "isspace")) target = (uint32_t)(void*)isspace;
        else if (str_eq(builtin_name, "isxdigit")) target = (uint32_t)(void*)isxdigit;
        else if (str_eq(builtin_name, "tolower")) target = (uint32_t)(void*)tolower;
        else if (str_eq(builtin_name, "toupper")) target = (uint32_t)(void*)toupper;
        else if (str_eq(builtin_name, "malloc")) target = (uint32_t)(void*)cc_malloc;
        else if (str_eq(builtin_name, "calloc")) target = (uint32_t)(void*)cc_calloc;
        else if (str_eq(builtin_name, "realloc")) target = (uint32_t)(void*)cc_realloc;
        else if (str_eq(builtin_name, "free")) { target = (uint32_t)(void*)cc_free; returns_value = 0; }
        else if (str_eq(builtin_name, "atol")) target = (uint32_t)(void*)cc_atol;
        else if (str_eq(builtin_name, "labs")) target = (uint32_t)(void*)cc_labs;
        else if (str_eq(builtin_name, "strtol")) target = (uint32_t)(void*)cc_strtol;
        else if (str_eq(builtin_name, "strtoul")) target = (uint32_t)(void*)cc_strtoul;
        else if (str_eq(builtin_name, "strcoll")) target = (uint32_t)(void*)cc_strcoll;
        else if (str_eq(builtin_name, "strxfrm")) target = (uint32_t)(void*)cc_strxfrm;
        else if (str_eq(builtin_name, "remove")) target = (uint32_t)(void*)cc_remove;
        else if (str_eq(builtin_name, "perror")) { target = (uint32_t)(void*)cc_perror; returns_value = 0; }
        else if (str_eq(builtin_name, "strncat")) target = (uint32_t)(void*)cc_strncat;
        else if (str_eq(builtin_name, "memchr")) target = (uint32_t)(void*)cc_memchr;
        else if (str_eq(builtin_name, "strspn")) target = (uint32_t)(void*)cc_strspn;
        else if (str_eq(builtin_name, "strcspn")) target = (uint32_t)(void*)cc_strcspn;
        else if (str_eq(builtin_name, "strpbrk")) target = (uint32_t)(void*)cc_strpbrk;
        else if (str_eq(builtin_name, "strtok")) target = (uint32_t)(void*)cc_strtok;
        else if (str_eq(builtin_name, "strdup")) target = (uint32_t)(void*)cc_strdup;
        else if (str_eq(builtin_name, "strndup")) target = (uint32_t)(void*)cc_strndup;
        else if (str_eq(builtin_name, "isgraph")) target = (uint32_t)(void*)cc_isgraph;
        else if (str_eq(builtin_name, "isprint")) target = (uint32_t)(void*)cc_isprint;
        else if (str_eq(builtin_name, "ispunct")) target = (uint32_t)(void*)cc_ispunct;
        else if (str_eq(builtin_name, "iscntrl")) target = (uint32_t)(void*)cc_iscntrl;
        else if (str_eq(builtin_name, "strerror")) target = (uint32_t)(void*)cc_strerror;
        else {
            cc_seterr(cur_tok.line, "unknown builtin");
            return;
        }

        gen_call_abs(target);
        if (!returns_value)
            gen_mov_eax_imm32(0);
    } else {
        if (fn->nparams != argc) { cc_seterr(cur_tok.line, "wrong number of arguments"); return; }
        if (fn->addr != 0) {
            gen_call_abs(fn->addr);
        } else {
            // Declared (prototyped) but not defined yet - emit a
            // placeholder and resolve it once the whole file has been
            // parsed, by which point the real definition (required to
            // exist, just like real C) will have set fn->addr.
            if (pending_call_count >= MAX_PENDING_CALLS) { cc_seterr(cur_tok.line, "too many forward-referenced calls"); return; }
            int idx = (int)(fn - funcs);
            pending_calls[pending_call_count].patch_at = gen_call_placeholder();
            pending_calls[pending_call_count].func_index = idx;
            pending_calls[pending_call_count].line = cur_tok.line;
            pending_call_count++;
        }
    }
    if (argc > 0) { emit_u8(0x81); emit_u8(0xC4); emit_u32((uint32_t)(argc * 4)); } // add esp, argc*4
}

static int is_type_start(token_type_t t);
static int consume_scalar_type(void);

static void parse_primary(void) {
    if (cc_error_flag) return;
    if (cur_tok.type == T_STR) {
        /* String literals are first-class pointer values.  Store the bytes
         * inline in the generated code and return their runtime address. */
        uint32_t jmp_at = gen_jmp_placeholder();
        uint32_t str_addr = (uint32_t)code_buf + code_len;
        int i = 0;
        while (cur_tok.sval[i]) { emit_u8((uint8_t)cur_tok.sval[i]); i++; }
        emit_u8(0);
        patch_jump_here(jmp_at);
        gen_mov_eax_imm32(str_addr);
        next_token();
        return;
    }
    if (cur_tok.type == T_NUM) {
        gen_mov_eax_imm32((uint32_t)cur_tok.ival);
        next_token();
        return;
    }
    if (cur_tok.type == T_SIZEOF) {
        next_token();
        uint32_t size = 4;
        int has_paren = (cur_tok.type == T_LPAREN);
        if (has_paren) next_token();

        if (cur_tok.type == T_VOID || is_type_start(cur_tok.type)) {
            int is_char = 0;
            if (cur_tok.type == T_VOID) next_token();
            else { is_char = consume_scalar_type(); if (cc_error_flag) return; }
            int stars = 0;
            while (cur_tok.type == T_STAR) { stars++; next_token(); }
            size = (stars > 0) ? 4 : (is_char ? 1 : 4);
            if (has_paren) { expect(T_RPAREN, "expected ')' after sizeof type"); if (cc_error_flag) return; }
        } else {
            int stars = 0;
            while (cur_tok.type == T_STAR) { stars++; next_token(); } // sizeof *p, sizeof **p, ...
            int handled = 0;
            if (cur_tok.type == T_IDENT) {
                lexer_state_t save = lexer_save();
                char name[64];
                int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
                int line = cur_tok.line;
                next_token();
                if (cur_tok.type != T_LBRACKET) {
                    // Plain identifier (optionally star-prefixed) - its
                    // static type is known without evaluating anything,
                    // exactly what sizeof requires.
                    local_t* l = find_local(name);
                    global_t* g = l ? 0 : find_global(name);
                    if (!l && !g) { cc_seterr(line, "undefined variable"); return; }
                    int elem_size = l ? l->elem_size : g->elem_size;
                    int ptr_depth = l ? l->ptr_depth : g->ptr_depth;
                    int is_array = l ? l->is_array : g->is_array;
                    int array_len = l ? l->array_len : g->array_len;
                    if (stars > 0) size = (ptr_depth > stars) ? 4 : (elem_size ? (uint32_t)elem_size : 4);
                    else if (is_array) size = (uint32_t)(array_len * (elem_size ? elem_size : 4));
                    else if (ptr_depth > 0) size = 4;
                    else size = (uint32_t)(elem_size ? elem_size : 4);
                    handled = 1;
                } else {
                    lexer_restore(save);
                }
            }
            if (!handled) {
                // General expression - sizeof never evaluates its
                // operand, so parse it purely to consume tokens
                // correctly and then discard any code that got emitted.
                uint32_t saved_len = code_len;
                parse_expr();
                if (cc_error_flag) return;
                code_len = saved_len;
                size = 4; // best-effort default, matching typical int-sized results
            }
            if (has_paren) { expect(T_RPAREN, "expected ')' after sizeof expression"); if (cc_error_flag) return; }
        }

        gen_mov_eax_imm32(size);
        return;
    }
    if (cur_tok.type == T_LPAREN) {
        next_token();
        if (cur_tok.type == T_VOID || is_type_start(cur_tok.type)) {
            // Explicit cast: (type)expr. The internal representation is
            // a uniform 32-bit cell for everything, so a cast to char
            // truncates to a single byte (matching real C's narrowing
            // behavior on assignment/cast); casts to int-family types
            // and pointer types are identity operations, since nothing
            // in this compiler distinguishes signed/unsigned or pointee
            // types at the register level.
            int is_char = 0;
            if (cur_tok.type == T_VOID) next_token();
            else { is_char = consume_scalar_type(); if (cc_error_flag) return; }
            while (cur_tok.type == T_STAR) next_token(); // pointer cast levels - accepted, no runtime effect
            expect(T_RPAREN, "expected ')' after cast"); if (cc_error_flag) return;
            if (looks_like_float_expr()) {
                // (int)/(char) cast of a float/double expression:
                // truncate toward zero, matching real C.
                parse_float_expr();
                if (cc_error_flag) return;
                gen_float_to_int_trunc();
            } else {
                parse_primary(); // operand -> eax
                if (cc_error_flag) return;
            }
            if (is_char) { emit_u8(0x25); emit_u32(0x000000FF); } // and eax, 0xFF
            return;
        }
        parse_expr();
        expect(T_RPAREN, "expected ')'");
        return;
    }
    if (cur_tok.type == T_MINUS) {
        next_token();
        parse_primary();
        if (cc_error_flag) return;
        gen_neg_eax();
        return;
    }
    if (cur_tok.type == T_NOT) {
        next_token();
        parse_primary();
        if (cc_error_flag) return;
        gen_cmp_eax_0();
        gen_setcc_movzx(0x94); // sete
        return;
    }
    if (cur_tok.type == T_TILDE) {
        next_token();
        parse_primary();
        if (cc_error_flag) return;
        gen_not_eax();
        return;
    }
    if (cur_tok.type == T_STAR) {
        // pointer dereference (rvalue): *expr, **expr, ...
        // Multiple leading stars are counted up front. When the
        // innermost operand is a plain identifier, its declared
        // pointer depth/element size are known statically, so the
        // final indirection can use a byte-sized load for a char
        // pointer; every level above that is always a plain 4-byte
        // pointer value. For any other kind of operand (e.g. *(p+1),
        // *f()) the element size defaults to 4, matching this
        // compiler's existing behavior for non-identifier addresses.
        int stars = 0;
        while (cur_tok.type == T_STAR) { stars++; next_token(); }
        int elem_size = 4;
        if (cur_tok.type == T_IDENT) {
            local_t* l = find_local(cur_tok.sval);
            global_t* g = l ? 0 : find_global(cur_tok.sval);
            if (l) elem_size = l->elem_size ? l->elem_size : 4;
            else if (g) elem_size = g->elem_size ? g->elem_size : 4;
        }
        parse_primary(); // address -> eax
        if (cc_error_flag) return;
        int k;
        for (k = 0; k < stars - 1; k++) gen_mov_eax_indirect_eax(); // intermediate pointer levels
        if (elem_size == 1) gen_mov_eax_indirect_eax_byte();
        else gen_mov_eax_indirect_eax();
        return;
    }
    if (cur_tok.type == T_BITAND) {
        // address-of: &ident or &ident[expr]
        next_token();
        int line = cur_tok.line;
        if (cur_tok.type != T_IDENT) { cc_seterr(line, "'&' must be followed by a variable"); return; }
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        next_token();
        if (cur_tok.type == T_LBRACKET) {
            next_token();
            parse_array_index_addr(name, line); // element address, already in eax
            return;
        }
        local_t* l = find_local(name);
        if (l) { gen_lea_eax_ebp_disp32(l->offset); return; }
        global_t* g = find_global(name);
        if (g) { gen_mov_eax_imm32(g->addr); return; }
        cc_seterr(line, "undefined variable");
        return;
    }
    if (cur_tok.type == T_INC || cur_tok.type == T_DEC) {
        // prefix ++x / --x (simple variables only, not array elements)
        int is_inc = (cur_tok.type == T_INC);
        next_token();
        if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "'++'/'--' must be followed by a variable"); return; }
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int line = cur_tok.line;
        next_token();
        gen_var_load_to_eax(name, line);
        if (cc_error_flag) return;
        if (is_inc) { emit_u8(0x83); emit_u8(0xC0); emit_u8(0x01); } // add eax,1
        else { emit_u8(0x83); emit_u8(0xE8); emit_u8(0x01); }        // sub eax,1
        gen_var_store_from_eax(name, line);
        return;
    }
    if (cur_tok.type == T_IDENT) {
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int line = cur_tok.line;
        next_token();
        if (cur_tok.type == T_LPAREN) {
            next_token();
            func_t* fn = find_func(name);

            /*
             * Any `cmd_<name>(...)` which is not a user-defined C function
             * is a TanjaOS shell command.  This deliberately happens before
             * the normal builtin-C list, so new commands added to bin/ are
             * automatically available without changing kernel/cc.c.
             */
            if (!fn && str_starts_with(name, "cmd_") && name[4]) {
                char command_name[64];
                int name_len = 0;
                int argc = 0;

                while (name[4 + name_len] && name_len < (int)sizeof(command_name) - 1) {
                    command_name[name_len] = name[4 + name_len];
                    name_len++;
                }
                command_name[name_len] = 0;

                /*
                 * Begin the runtime command line.  Push arguments through a
                 * small setter so evaluation still happens normally and all
                 * ordinary C expressions remain fully supported.
                 */
                {
                    uint32_t begin_target = (uint32_t)(void*)cc_cmd_begin;
                    uint32_t set_target = (uint32_t)(void*)cc_cmd_set_arg;
                    uint32_t end_target = (uint32_t)(void*)cc_cmd_end;
                    uint32_t jmp_name = gen_jmp_placeholder();
                    uint32_t name_addr = (uint32_t)code_buf + code_len;
                    int j;

                    for (j = 0; command_name[j]; j++) emit_u8((uint8_t)command_name[j]);
                    emit_u8(0);
                    patch_jump_here(jmp_name);

                    gen_mov_eax_imm32(name_addr);
                    gen_push_eax();
                    gen_call_abs(begin_target);
                    emit_u8(0x81); emit_u8(0xC4); emit_u32(4);

                    if (cur_tok.type != T_RPAREN) {
                        for (;;) {
                            int is_string = (cur_tok.type == T_STR);
                            if (cur_tok.type == T_IDENT) {
                                local_t* al = find_local(cur_tok.sval);
                                global_t* ag = find_global(cur_tok.sval);
                                if ((al && al->is_array) || (ag && ag->is_array))
                                    is_string = 1;
                            }

                            parse_expr();
                            if (cc_error_flag) return;

                            /*
                             * cc_cmd_set_arg(index, value, is_string) is
                             * called with the compiler's left-to-right
                             * argument convention: push value, then type,
                             * then index.
                             */
                            gen_push_eax();
                            gen_mov_eax_imm32((uint32_t)is_string);
                            gen_push_eax();
                            gen_mov_eax_imm32((uint32_t)argc);
                            gen_push_eax();
                            gen_call_abs(set_target);
                            emit_u8(0x81); emit_u8(0xC4); emit_u32(12);

                            argc++;
                            if (argc > CC_CMD_MAX_ARGS) {
                                cc_seterr(cur_tok.line, "too many command arguments (maximum 8)");
                                return;
                            }

                            if (cur_tok.type == T_COMMA) {
                                next_token();
                                continue;
                            }
                            break;
                        }
                    }

                    expect(T_RPAREN, "expected ')' after command arguments");
                    if (cc_error_flag) return;

                    gen_call_abs(end_target);
                    gen_mov_eax_imm32(0);
                    return;
                }
            }

            const char* builtin = 0;
            if (!fn) {
                if (str_eq(name, "printf") || str_eq(name, "scanf") ||
                    str_eq(name, "putchar") || str_eq(name, "getchar") ||
                    str_eq(name, "puts") || str_eq(name, "strlen") ||
                    str_eq(name, "strcmp") || str_eq(name, "strncmp") ||
                    str_eq(name, "strcpy") || str_eq(name, "strncpy") ||
                    str_eq(name, "strcat") || str_eq(name, "strchr") ||
                    str_eq(name, "strrchr") || str_eq(name, "strstr") ||
                    str_eq(name, "memset") || str_eq(name, "memcpy") ||
                    str_eq(name, "memmove") || str_eq(name, "memcmp") ||
                    str_eq(name, "atoi") || str_eq(name, "abs") ||
                    str_eq(name, "rand") || str_eq(name, "srand") ||
                    str_eq(name, "isdigit") || str_eq(name, "isalpha") ||
                    str_eq(name, "isalnum") || str_eq(name, "islower") ||
                    str_eq(name, "isupper") || str_eq(name, "isspace") ||
                    str_eq(name, "isxdigit") || str_eq(name, "tolower") ||
                    str_eq(name, "toupper") || str_eq(name, "malloc") ||
                    str_eq(name, "calloc") || str_eq(name, "realloc") ||
                    str_eq(name, "free") || str_eq(name, "atol") ||
                    str_eq(name, "labs") || str_eq(name, "strtol") ||
                    str_eq(name, "strtoul") || str_eq(name, "strncat") ||
                    str_eq(name, "memchr") || str_eq(name, "strspn") ||
                    str_eq(name, "strcspn") || str_eq(name, "strpbrk") ||
                    str_eq(name, "strtok") || str_eq(name, "strdup") ||
                    str_eq(name, "strndup") || str_eq(name, "isgraph") ||
                    str_eq(name, "isprint") || str_eq(name, "ispunct") ||
                    str_eq(name, "iscntrl") || str_eq(name, "strerror") ||
                    str_eq(name, "clear_screen") ||
                    str_eq(name, "clear_screen")) builtin = name;
                else { cc_seterr(line, "call to undefined function"); return; }
            }
            parse_call_args_and_call(fn, builtin);
            return;
        }
        if (cur_tok.type == T_LBRACKET) {
            next_token();
            parse_array_index_addr(name, line); // element address -> eax
            if (cc_error_flag) return;
            if (lookup_elem_size(name) == 1) gen_mov_eax_indirect_eax_byte();
            else gen_mov_eax_indirect_eax(); // load value
            return;
        }
        if (cur_tok.type == T_INC || cur_tok.type == T_DEC) {
            // postfix x++ / x-- (simple variables only)
            int is_inc = (cur_tok.type == T_INC);
            next_token();
            gen_var_load_to_eax(name, line); // old value -> eax (this IS the expression result)
            if (cc_error_flag) return;
            gen_push_eax(); // save old value
            if (is_inc) { emit_u8(0x83); emit_u8(0xC0); emit_u8(0x01); } // add eax,1
            else { emit_u8(0x83); emit_u8(0xE8); emit_u8(0x01); }        // sub eax,1
            gen_var_store_from_eax(name, line);
            if (cc_error_flag) return;
            gen_pop_eax(); // restore old value as the final result
            return;
        }
        gen_var_load_to_eax(name, line);
        return;
    }
    cc_seterr(cur_tok.line, "expected expression");
}

static void parse_term(void) {
    parse_primary();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_STAR) {
            next_token(); gen_push_eax(); parse_primary(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_imul_eax_ecx();
        } else if (cur_tok.type == T_SLASH) {
            next_token(); gen_push_eax(); parse_primary(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_div_setup_and_idiv();
        } else if (cur_tok.type == T_PERCENT) {
            next_token(); gen_push_eax(); parse_primary(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_div_setup_and_idiv();
            emit_u8(0x89); emit_u8(0xD0); // mov eax, edx (remainder)
        } else break;
    }
}

static void parse_additive(void) {
    parse_term();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_PLUS) {
            next_token(); gen_push_eax(); parse_term(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_add_eax_ecx();
        } else if (cur_tok.type == T_MINUS) {
            next_token(); gen_push_eax(); parse_term(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_sub_ecx_eax_then_mov();
        } else break;
    }
}

static void parse_shift(void) {
    parse_additive();
    while (cur_tok.type == T_SHL || cur_tok.type == T_SHR) {
        token_type_t t = cur_tok.type;
        next_token();
        gen_push_eax();
        parse_additive();
        if (cc_error_flag) return;
        gen_pop_ecx();
        gen_xchg_eax_ecx();
        if (t == T_SHL) gen_shl_eax_cl();
        else gen_sar_eax_cl();
    }
}

static void parse_bitand(void) {
    parse_shift();
    while (cur_tok.type == T_BITAND) {
        next_token(); gen_push_eax(); parse_shift(); if (cc_error_flag) return;
        gen_pop_ecx(); gen_and_eax_ecx();
    }
}

static void parse_bitxor(void) {
    parse_bitand();
    while (cur_tok.type == T_XOR) {
        next_token(); gen_push_eax(); parse_bitand(); if (cc_error_flag) return;
        gen_pop_ecx(); gen_xor_eax_ecx();
    }
}

static void parse_bitor(void) {
    parse_bitxor();
    while (cur_tok.type == T_BITOR) {
        next_token(); gen_push_eax(); parse_bitxor(); if (cc_error_flag) return;
        gen_pop_ecx(); gen_or_eax_ecx();
    }
}

static void parse_relational(void) {
    parse_bitor();
    for (;;) {
        if (cc_error_flag) return;
        token_type_t t = cur_tok.type;
        if (t == T_LT || t == T_GT || t == T_LE || t == T_GE) {
            next_token(); gen_push_eax(); parse_additive(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_cmp_ecx_eax();
            if (t == T_LT) gen_setcc_movzx(0x9C);
            else if (t == T_GT) gen_setcc_movzx(0x9F);
            else if (t == T_LE) gen_setcc_movzx(0x9E);
            else gen_setcc_movzx(0x9D);
        } else break;
    }
}

static void parse_equality(void) {
    parse_relational();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_EQ || cur_tok.type == T_NE) {
            token_type_t t = cur_tok.type;
            next_token(); gen_push_eax(); parse_relational(); if (cc_error_flag) return;
            gen_pop_ecx(); gen_cmp_ecx_eax();
            gen_setcc_movzx(t == T_EQ ? 0x94 : 0x95);
        } else break;
    }
}

static void parse_logic_and(void) {
    parse_equality();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_ANDAND) {
            next_token();
            gen_test_eax_eax();
            uint32_t jz_at = gen_jz_placeholder(); // false -> skip rhs, result 0
            parse_equality(); if (cc_error_flag) return;
            gen_test_eax_eax();
            gen_setcc_movzx(0x95); // eax = (rhs != 0)
            uint32_t jmp_at = gen_jmp_placeholder();
            patch_jump_here(jz_at);
            gen_mov_eax_imm32(0);
            patch_jump_here(jmp_at);
        } else break;
    }
}

static void parse_logic_or(void) {
    parse_logic_and();
    for (;;) {
        if (cc_error_flag) return;
        if (cur_tok.type == T_OROR) {
            next_token();
            gen_test_eax_eax();
            uint32_t jnz_at = gen_jnz_placeholder(); // true -> skip rhs, result 1
            parse_logic_and(); if (cc_error_flag) return;
            gen_test_eax_eax();
            gen_setcc_movzx(0x95);
            uint32_t jmp_at = gen_jmp_placeholder();
            patch_jump_here(jnz_at);
            gen_mov_eax_imm32(1);
            patch_jump_here(jmp_at);
        } else break;
    }
}

static void parse_conditional(void) {
    parse_logic_or();
    if (cc_error_flag) return;

    if (cur_tok.type == T_QUESTION) {
        next_token();
        gen_test_eax_eax();
        uint32_t false_at = gen_jz_placeholder();

        parse_expr();
        if (cc_error_flag) return;

        uint32_t end_at = gen_jmp_placeholder();
        expect(T_COLON, "expected ':' in conditional expression");
        if (cc_error_flag) return;

        patch_jump_here(false_at);
        parse_conditional();
        if (cc_error_flag) return;
        patch_jump_here(end_at);
    }
}

static void parse_assign(void) {
    if (cur_tok.type == T_IDENT) {
        lexer_state_t save = lexer_save();
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int line = cur_tok.line;
        next_token();

        if (cur_tok.type == T_LBRACKET && peek_is_array_assign()) {
            next_token(); // consume '['
            parse_array_index_addr(name, line); // element address -> eax
            if (cc_error_flag) return;
            int elem_size = lookup_elem_size(name);
            token_type_t op = cur_tok.type;
            next_token(); // consume the assign/compound-assign operator
            if (op == T_ASSIGN) {
                gen_push_eax(); // save element address
                parse_assign();
                if (cc_error_flag) return;
                gen_pop_ecx();
                if (elem_size == 1) gen_mov_indirect_ecx_al();
                else gen_mov_indirect_ecx_eax();
            } else {
                gen_push_eax();             // save element address
                gen_mov_eax_esp0();         // peek address without popping
                if (elem_size == 1) gen_mov_eax_indirect_eax_byte();
                else gen_mov_eax_indirect_eax(); // eax = old value
                gen_push_eax();             // stack: [addr, oldval]
                parse_assign();             // RHS -> eax
                if (cc_error_flag) return;
                gen_pop_ecx();              // ecx = oldval (left operand)
                if (op == T_PLUSEQ) gen_add_eax_ecx();
                else if (op == T_MINUSEQ) gen_sub_ecx_eax_then_mov();
                else if (op == T_STAREQ) gen_imul_eax_ecx();
                else if (op == T_SLASHEQ) gen_div_setup_and_idiv();
                else if (op == T_ANDEQ) gen_and_eax_ecx();
                else if (op == T_OREQ) gen_or_eax_ecx();
                else if (op == T_XOREQ) gen_xor_eax_ecx();
                else if (op == T_SHLEQ || op == T_SHREQ) {
                    gen_xchg_eax_ecx();
                    if (op == T_SHLEQ) gen_shl_eax_cl();
                    else gen_sar_eax_cl();
                } else { gen_div_setup_and_idiv(); emit_u8(0x89); emit_u8(0xD0); } // %=
                gen_pop_ecx();              // ecx = element address
                if (elem_size == 1) gen_mov_indirect_ecx_al();
                else gen_mov_indirect_ecx_eax();
            }
            return;
        }

        if (cur_tok.type == T_ASSIGN) {
            next_token();
            local_t* fl = find_local(name);
            global_t* fg = fl ? 0 : find_global(name);
            if ((fl && fl->is_fp) || (fg && fg->is_fp)) {
                int is_double = fl ? fl->fp_double : fg->fp_double;
                parse_float_expr();
                if (cc_error_flag) return;
                if (fl) gen_fstp_ebp_disp32(fl->offset, is_double);
                else gen_fstp_abs(fg->addr, is_double);
                return;
            }
            parse_assign();
            if (cc_error_flag) return;
            gen_var_store_from_eax(name, line);
            return;
        }
        if (cur_tok.type == T_PLUSEQ || cur_tok.type == T_MINUSEQ || cur_tok.type == T_STAREQ ||
            cur_tok.type == T_SLASHEQ || cur_tok.type == T_PERCENTEQ || cur_tok.type == T_ANDEQ || cur_tok.type == T_OREQ || cur_tok.type == T_XOREQ || cur_tok.type == T_SHLEQ || cur_tok.type == T_SHREQ) {
            token_type_t op = cur_tok.type;
            next_token();
            gen_var_load_to_eax(name, line); // old value -> eax
            if (cc_error_flag) return;
            gen_push_eax();
            parse_assign(); // RHS -> eax
            if (cc_error_flag) return;
            gen_pop_ecx(); // ecx = old value (left operand)
            if (op == T_PLUSEQ) gen_add_eax_ecx();
            else if (op == T_MINUSEQ) gen_sub_ecx_eax_then_mov();
            else if (op == T_STAREQ) gen_imul_eax_ecx();
            else if (op == T_SLASHEQ) gen_div_setup_and_idiv();
            else { gen_div_setup_and_idiv(); emit_u8(0x89); emit_u8(0xD0); } // %=
            gen_var_store_from_eax(name, line);
            return;
        }

        lexer_restore(save);
    }

    // Pointer dereference assignment: *IDENT = expr, **IDENT = expr, ...
    // (plain '=' only - compound-assign through a pointer isn't
    // supported in this subset). Restricted to a plain identifier
    // after the star-chain, same scoping as the rvalue deref above.
    if (cur_tok.type == T_STAR) {
        lexer_state_t save = lexer_save();
        int stars = 0;
        while (cur_tok.type == T_STAR) { stars++; next_token(); }
        if (cur_tok.type == T_IDENT) {
            char name[64];
            int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
            int line = cur_tok.line;
            next_token();
            if (cur_tok.type == T_ASSIGN) {
                next_token();
                int elem_size = lookup_elem_size(name);
                gen_var_load_to_eax(name, line); // level-1 pointer value -> eax
                if (cc_error_flag) return;
                int k;
                for (k = 0; k < stars - 1; k++) gen_mov_eax_indirect_eax(); // walk down remaining pointer levels
                gen_push_eax(); // final target address
                parse_assign(); // RHS -> eax
                if (cc_error_flag) return;
                gen_pop_ecx();
                if (elem_size == 1) gen_mov_indirect_ecx_al();
                else gen_mov_indirect_ecx_eax();
                return;
            }
        }
        lexer_restore(save);
    }

    parse_conditional();
}

static void parse_expr(void) { parse_assign(); }

// ------------------------------------------------------------
// Statements
// ------------------------------------------------------------

static void add_local(const char* name, int is_array, int array_len, int elem_size, int ptr_depth) {
    if (local_count >= MAX_LOCALS) { cc_seterr(cur_tok.line, "too many local variables"); return; }
    if (find_local(name)) { cc_seterr(cur_tok.line, "duplicate variable name"); return; }
    int i = 0; while (name[i] && i < 31) { locals[local_count].name[i] = name[i]; i++; }
    locals[local_count].name[i] = 0;
    // Arrays are byte-packed at their declared element size (1 for
    // char, 4 otherwise) so pointer arithmetic and indexing agree with
    // real C memory layout; a plain scalar still gets a full 4-byte
    // slot regardless of type, which is a legal implementation choice
    // (the standard doesn't mandate tight packing for scalars, only
    // that sizeof report the right value and array elements sit
    // sizeof(T) bytes apart).
    int bytes = is_array ? array_len * elem_size : 4;
    next_local_offset -= bytes;
    if (next_local_offset < -2048) { cc_seterr(cur_tok.line, "too many/too large local variables (frame too large)"); return; }
    locals[local_count].offset = next_local_offset; // base offset = element 0 (lowest address)
    locals[local_count].is_array = is_array;
    locals[local_count].array_len = array_len;
    locals[local_count].elem_size = elem_size;
    locals[local_count].ptr_depth = ptr_depth;
    locals[local_count].is_fp = 0;
    locals[local_count].fp_double = 0;
    local_count++;
}

static void add_local_fp(const char* name, int is_double) {
    if (local_count >= MAX_LOCALS) { cc_seterr(cur_tok.line, "too many local variables"); return; }
    if (find_local(name)) { cc_seterr(cur_tok.line, "duplicate variable name"); return; }
    int i = 0; while (name[i] && i < 31) { locals[local_count].name[i] = name[i]; i++; }
    locals[local_count].name[i] = 0;
    int bytes = is_double ? 8 : 4;
    next_local_offset -= bytes;
    if (next_local_offset < -2048) { cc_seterr(cur_tok.line, "too many/too large local variables (frame too large)"); return; }
    locals[local_count].offset = next_local_offset;
    locals[local_count].is_array = 0;
    locals[local_count].array_len = 0;
    locals[local_count].elem_size = bytes;
    locals[local_count].ptr_depth = 0;
    locals[local_count].is_fp = 1;
    locals[local_count].fp_double = is_double;
    local_count++;
}

static int is_type_start(token_type_t t) {
    return t == T_INT || t == T_CHAR || t == T_CONST ||
           t == T_UNSIGNED || t == T_SIGNED || t == T_LONG || t == T_SHORT ||
           t == T_FLOAT || t == T_DOUBLE;
}

static int consume_scalar_type(void) {
    int saw_base = 0;
    int is_char = 0;
    while (cur_tok.type == T_CONST || cur_tok.type == T_UNSIGNED ||
           cur_tok.type == T_SIGNED || cur_tok.type == T_LONG ||
           cur_tok.type == T_SHORT || cur_tok.type == T_CHAR ||
           cur_tok.type == T_INT) {
        if (cur_tok.type == T_CHAR || cur_tok.type == T_INT) saw_base = 1;
        if (cur_tok.type == T_CHAR) is_char = 1;
        if (cur_tok.type == T_INT) is_char = 0; // `int` anywhere means it's not a plain char
        next_token();
    }
    if (!saw_base && !cc_error_flag)
        cc_seterr(cur_tok.line, "expected scalar type");
    return is_char;
}

static void parse_local_decl_fp(int is_double) {
    next_token(); // consume 'float'/'double'
    for (;;) {
        if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected identifier after type"); return; }
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        next_token();

        add_local_fp(name, is_double);
        if (cc_error_flag) return;

        if (cur_tok.type == T_ASSIGN) {
            next_token();
            parse_float_expr();
            if (cc_error_flag) return;
            local_t* l = find_local(name);
            gen_fstp_ebp_disp32(l->offset, is_double);
        }

        if (cur_tok.type != T_COMMA) break;
        next_token();
    }
}

static void parse_local_decl(void) {
    if (cur_tok.type == T_FLOAT || cur_tok.type == T_DOUBLE) {
        parse_local_decl_fp(cur_tok.type == T_DOUBLE);
        return;
    }
    int is_char = consume_scalar_type();
    if (cc_error_flag) return;

    for (;;) {
        int ptr_depth = 0;
        while (cur_tok.type == T_STAR) { ptr_depth++; next_token(); }
        if (cur_tok.type != T_IDENT) {
            cc_seterr(cur_tok.line, "expected identifier after type");
            return;
        }

        char name[64];
        int i = 0;
        while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; }
        name[i] = 0;
        next_token();

        // A pointer's own storage is always a 4-byte address, but
        // elem_size here tracks what it (ultimately) POINTS TO, which
        // is what dereferencing/array-indexing need to pick the right
        // load/store width - independent of how many `*` levels deep.
        int elem_size = is_char ? 1 : 4;

        if (cur_tok.type == T_LBRACKET) {
            next_token();
            if (cur_tok.type != T_NUM || cur_tok.ival <= 0) {
                cc_seterr(cur_tok.line, "array size must be a positive constant");
                return;
            }
            int len = cur_tok.ival;
            next_token();
            expect(T_RBRACKET, "expected ']'");
            if (cc_error_flag) return;
            add_local(name, 1, len, is_char ? 1 : 4, 0);
        } else {
            add_local(name, 0, 0, elem_size, ptr_depth);
            if (cc_error_flag) return;

            if (cur_tok.type == T_ASSIGN) {
                next_token();
                parse_expr();
                if (cc_error_flag) return;
                local_t* l = find_local(name);
                gen_mov_ebp_disp32_eax(l->offset);
            }
        }

        if (cur_tok.type != T_COMMA)
            break;
        next_token();
    }
}

static void parse_block(void);

static void parse_stmt(void) {
    if (cc_error_flag) return;
    if (cur_tok.type == T_LBRACE) { parse_block(); return; }

    if (cur_tok.type == T_IF) {
        next_token();
        expect(T_LPAREN, "expected '(' after if"); if (cc_error_flag) return;
        parse_condition_to_eax(); if (cc_error_flag) return;
        expect(T_RPAREN, "expected ')'"); if (cc_error_flag) return;
        gen_test_eax_eax();
        uint32_t jz_at = gen_jz_placeholder();
        parse_stmt(); if (cc_error_flag) return;
        if (cur_tok.type == T_ELSE) {
            uint32_t jmp_at = gen_jmp_placeholder();
            patch_jump_here(jz_at);
            next_token();
            parse_stmt(); if (cc_error_flag) return;
            patch_jump_here(jmp_at);
        } else {
            patch_jump_here(jz_at);
        }
        return;
    }

    if (cur_tok.type == T_DO) {
        next_token();

        uint32_t body_start = code_len;
        loop_ctx_t ctx;
        ctx.break_count = 0;
        ctx.continue_count = 0;
        ctx.continue_target_known = 0;

        loop_ctx_t* prev_loop = cur_loop;
        int prev_loop_seq = cur_loop_seq;
        cur_loop = &ctx;
        cur_loop_seq = ++nest_seq_counter;
        parse_stmt();
        cur_loop = prev_loop;
        cur_loop_seq = prev_loop_seq;
        if (cc_error_flag) return;

        uint32_t cond_start = code_len;
        int ci;
        for (ci = 0; ci < ctx.continue_count; ci++)
            patch_jump_here(ctx.continue_patches[ci]);

        if (cur_tok.type != T_WHILE) {
            cc_seterr(cur_tok.line, "expected 'while' after do statement");
            return;
        }
        next_token();
        expect(T_LPAREN, "expected '(' after do/while");
        if (cc_error_flag) return;
        parse_condition_to_eax();
        if (cc_error_flag) return;
        expect(T_RPAREN, "expected ')' after do/while condition");
        if (cc_error_flag) return;
        expect(T_SEMI, "expected ';' after do/while");
        if (cc_error_flag) return;

        gen_test_eax_eax();
        uint32_t exit_at = gen_jz_placeholder();
        gen_jmp_to(body_start);
        patch_jump_here(exit_at);

        for (ci = 0; ci < ctx.break_count; ci++)
            patch_jump_here(ctx.break_patches[ci]);
        (void)cond_start;
        return;
    }

    if (cur_tok.type == T_WHILE) {
        next_token();
        uint32_t loop_start = code_len;
        expect(T_LPAREN, "expected '(' after while"); if (cc_error_flag) return;
        parse_condition_to_eax(); if (cc_error_flag) return;
        expect(T_RPAREN, "expected ')'"); if (cc_error_flag) return;
        gen_test_eax_eax();
        uint32_t jz_at = gen_jz_placeholder();

        loop_ctx_t ctx; ctx.break_count = 0; ctx.continue_count = 0;
        ctx.continue_target_known = 1; ctx.continue_target = loop_start;
        loop_ctx_t* prev_loop = cur_loop; cur_loop = &ctx;
        int prev_loop_seq = cur_loop_seq; cur_loop_seq = ++nest_seq_counter;
        parse_stmt();
        cur_loop = prev_loop;
        cur_loop_seq = prev_loop_seq;
        if (cc_error_flag) return;

        gen_jmp_to(loop_start);
        patch_jump_here(jz_at);
        { int bi; for (bi = 0; bi < ctx.break_count; bi++) patch_jump_here(ctx.break_patches[bi]); }
        return;
    }

    if (cur_tok.type == T_FOR) {
        next_token();
        expect(T_LPAREN, "expected '(' after for"); if (cc_error_flag) return;
        if (cur_tok.type != T_SEMI) {
            if (is_type_start(cur_tok.type)) parse_local_decl();
            else parse_expr();
            if (cc_error_flag) return;
        }
        expect(T_SEMI, "expected ';' after for-init"); if (cc_error_flag) return;

        uint32_t cond_start = code_len;
        int has_cond = (cur_tok.type != T_SEMI);
        uint32_t jz_at = 0;
        if (has_cond) {
            parse_condition_to_eax(); if (cc_error_flag) return;
            gen_test_eax_eax();
            jz_at = gen_jz_placeholder();
        }
        expect(T_SEMI, "expected ';' after for-condition"); if (cc_error_flag) return;

        // Skip the increment expression's tokens for now (no codegen);
        // remember its source range and re-parse it after the body.
        uint32_t incr_start_byte = cur_tok.start_pos;
        int incr_start_line = cur_tok.line;
        int has_incr = (cur_tok.type != T_RPAREN);
        if (has_incr) {
            int depth = 0;
            for (;;) {
                if (cur_tok.type == T_LPAREN) depth++;
                else if (cur_tok.type == T_RPAREN) { if (depth == 0) break; depth--; }
                else if (cur_tok.type == T_EOF) { cc_seterr(cur_tok.line, "unterminated for(...)"); return; }
                next_token();
            }
        }
        expect(T_RPAREN, "expected ')' after for-clauses"); if (cc_error_flag) return;

        // continue jumps here: run the increment, then fall through to
        // re-checking the condition - correct C for-loop continue
        // semantics. This address isn't known until after the body is
        // parsed, so continue statements inside the body get a
        // backpatch placeholder instead of a direct jump (see the
        // continue-statement handler below).
        loop_ctx_t ctx; ctx.break_count = 0; ctx.continue_count = 0;
        ctx.continue_target_known = 0;
        loop_ctx_t* prev_loop = cur_loop; cur_loop = &ctx;
        int prev_loop_seq = cur_loop_seq; cur_loop_seq = ++nest_seq_counter;
        parse_stmt();
        cur_loop = prev_loop;
        cur_loop_seq = prev_loop_seq;
        if (cc_error_flag) return;

        uint32_t incr_label = code_len;
        { int ci; for (ci = 0; ci < ctx.continue_count; ci++) patch_jump_here(ctx.continue_patches[ci]); }
        if (has_incr) {
            lexer_state_t save = lexer_save();
            src_pos = incr_start_byte;
            cur_line = incr_start_line;
            next_token();
            parse_expr();
            if (cc_error_flag) return;
            lexer_restore(save);
        }
        (void)incr_label;

        gen_jmp_to(cond_start);
        if (has_cond) patch_jump_here(jz_at);
        { int bi; for (bi = 0; bi < ctx.break_count; bi++) patch_jump_here(ctx.break_patches[bi]); }
        return;
    }

    if (cur_tok.type == T_BREAK) {
        next_token();
        expect(T_SEMI, "expected ';' after break"); if (cc_error_flag) return;
        if (!cur_loop && !cur_switch) { cc_seterr(cur_tok.line, "'break' outside a loop or switch"); return; }
        if (cur_switch && cur_switch_seq > cur_loop_seq) {
            if (cur_switch->break_count >= MAX_BREAK_PATCHES) { cc_seterr(cur_tok.line, "too many break statements in one switch"); return; }
            cur_switch->break_patches[cur_switch->break_count++] = gen_jmp_placeholder();
        } else {
            if (cur_loop->break_count >= MAX_BREAK_PATCHES) { cc_seterr(cur_tok.line, "too many break statements in one loop"); return; }
            cur_loop->break_patches[cur_loop->break_count++] = gen_jmp_placeholder();
        }
        return;
    }

    if (cur_tok.type == T_CONTINUE) {
        next_token();
        expect(T_SEMI, "expected ';' after continue"); if (cc_error_flag) return;
        if (!cur_loop) { cc_seterr(cur_tok.line, "'continue' outside a loop"); return; }
        if (cur_loop->continue_target_known) {
            gen_jmp_to(cur_loop->continue_target);
        } else {
            if (cur_loop->continue_count >= MAX_BREAK_PATCHES) { cc_seterr(cur_tok.line, "too many continue statements in one loop"); return; }
            cur_loop->continue_patches[cur_loop->continue_count++] = gen_jmp_placeholder();
        }
        return;
    }

    if (cur_tok.type == T_RETURN) {
        next_token();
        if (cur_tok.type != T_SEMI) { parse_expr(); if (cc_error_flag) return; }
        else gen_mov_eax_imm32(0);
        expect(T_SEMI, "expected ';' after return"); if (cc_error_flag) return;
        emit_u8(0xC9); // leave
        emit_u8(0xC3); // ret
        return;
    }

    if (cur_tok.type == T_GOTO) {
        next_token();
        if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected label name after goto"); return; }
        char name[64];
        int ni = 0; while (cur_tok.sval[ni] && ni < 31) { name[ni] = cur_tok.sval[ni]; ni++; } name[ni] = 0;
        int line = cur_tok.line;
        next_token();
        expect(T_SEMI, "expected ';' after goto"); if (cc_error_flag) return;

        label_t* lbl = find_label(name);
        if (lbl && lbl->defined) {
            gen_jmp_to(lbl->addr);
        } else {
            if (label_patch_count >= MAX_LABEL_PATCHES) { cc_seterr(line, "too many pending goto statements"); return; }
            uint32_t at = gen_jmp_placeholder();
            int k = 0; while (name[k] && k < 31) { label_patches[label_patch_count].name[k] = name[k]; k++; }
            label_patches[label_patch_count].name[k] = 0;
            label_patches[label_patch_count].patch_at = at;
            label_patches[label_patch_count].line = line;
            label_patch_count++;
        }
        return;
    }

    if (cur_tok.type == T_CASE) {
        if (!cur_switch) { cc_seterr(cur_tok.line, "'case' outside a switch"); return; }
        next_token();
        int neg = 0;
        if (cur_tok.type == T_MINUS) { neg = 1; next_token(); }
        if (cur_tok.type != T_NUM) { cc_seterr(cur_tok.line, "case label must be a constant"); return; }
        int32_t val = neg ? -cur_tok.ival : cur_tok.ival;
        next_token();
        expect(T_COLON, "expected ':' after case label"); if (cc_error_flag) return;
        if (cur_switch->case_count >= MAX_SWITCH_CASES) { cc_seterr(cur_tok.line, "too many case labels in one switch"); return; }
        cur_switch->case_values[cur_switch->case_count] = val;
        cur_switch->case_addrs[cur_switch->case_count] = code_len;
        cur_switch->case_count++;
        return;
    }

    if (cur_tok.type == T_DEFAULT) {
        if (!cur_switch) { cc_seterr(cur_tok.line, "'default' outside a switch"); return; }
        next_token();
        expect(T_COLON, "expected ':' after default"); if (cc_error_flag) return;
        if (cur_switch->has_default) { cc_seterr(cur_tok.line, "multiple 'default' labels in one switch"); return; }
        cur_switch->has_default = 1;
        cur_switch->default_addr = code_len;
        return;
    }

    if (cur_tok.type == T_SWITCH) {
        next_token();
        expect(T_LPAREN, "expected '(' after switch"); if (cc_error_flag) return;
        parse_expr(); if (cc_error_flag) return; // switch value -> eax
        expect(T_RPAREN, "expected ')' after switch expression"); if (cc_error_flag) return;

        // Store the switch value in a hidden stack slot (not a named
        // variable, so it can never collide with anything the user
        // declared) so the dispatch code emitted after the body can
        // reload it.
        next_local_offset -= 4;
        if (next_local_offset < -2048) { cc_seterr(cur_tok.line, "too many/too large local variables (frame too large)"); return; }
        int32_t value_slot = next_local_offset;
        gen_mov_ebp_disp32_eax(value_slot);

        // Body first, dispatch table after: jump over the body (which
        // is really just a sequence of statements with case/default
        // labels recorded inline), then emit a linear chain of
        // compares against the now-known case addresses, so every
        // address referenced by the dispatch table is already known
        // by the time it's generated - no separate backpatch pass
        // needed for the case jumps themselves.
        uint32_t to_dispatch = gen_jmp_placeholder();

        switch_ctx_t ctx;
        ctx.case_count = 0;
        ctx.has_default = 0;
        ctx.break_count = 0;
        ctx.value_slot = value_slot;
        switch_ctx_t* prev_switch = cur_switch;
        int prev_switch_seq = cur_switch_seq;
        cur_switch = &ctx;
        cur_switch_seq = ++nest_seq_counter;

        expect(T_LBRACE, "expected '{' after switch(...)");
        if (!cc_error_flag) {
            while (!cc_error_flag && cur_tok.type != T_RBRACE && cur_tok.type != T_EOF) {
                parse_stmt();
            }
            expect(T_RBRACE, "expected '}'");
        }

        cur_switch = prev_switch;
        cur_switch_seq = prev_switch_seq;
        if (cc_error_flag) return;

        // If the body falls off its natural end without an explicit
        // break (very common for the last case, e.g. a `default:`
        // with no trailing break), execution must NOT fall through
        // into the dispatch table below - those are raw compare/jump
        // bytes, not a continuation of the switch body, and running
        // into them as code can jump right back into a case and loop
        // forever. Treat "falls off the end" exactly like an implicit
        // break here.
        if (ctx.break_count >= MAX_BREAK_PATCHES) { cc_seterr(cur_tok.line, "too many break statements in one switch"); return; }
        ctx.break_patches[ctx.break_count++] = gen_jmp_placeholder();

        patch_jump_here(to_dispatch);
        gen_mov_eax_ebp_disp32(value_slot);
        { int ci; for (ci = 0; ci < ctx.case_count; ci++) {
            gen_cmp_eax_imm32((uint32_t)ctx.case_values[ci]);
            gen_je_to(ctx.case_addrs[ci]);
        } }
        if (ctx.has_default) gen_jmp_to(ctx.default_addr);

        { int bi; for (bi = 0; bi < ctx.break_count; bi++) patch_jump_here(ctx.break_patches[bi]); }
        return;
    }

    if (cur_tok.type == T_IDENT) {
        // A label definition: IDENT ':' - only possible at
        // statement-start position, and unambiguous since a bare
        // "identifier:" is never a valid C expression on its own.
        lexer_state_t save = lexer_save();
        char name[64];
        int ni = 0; while (cur_tok.sval[ni] && ni < 31) { name[ni] = cur_tok.sval[ni]; ni++; } name[ni] = 0;
        next_token();
        if (cur_tok.type == T_COLON) {
            next_token();
            label_t* existing = find_label(name);
            if (existing) {
                if (existing->defined) { cc_seterr(cur_tok.line, "duplicate label"); return; }
            } else {
                if (func_label_count >= MAX_LABELS) { cc_seterr(cur_tok.line, "too many labels in one function"); return; }
                existing = &func_labels[func_label_count++];
                int k = 0; while (name[k] && k < 31) { existing->name[k] = name[k]; k++; } existing->name[k] = 0;
            }
            existing->addr = code_len;
            existing->defined = 1;

            // Resolve any gotos that were already waiting on this label.
            int w, r = 0;
            for (w = 0; w < label_patch_count; w++) {
                if (str_eq(label_patches[w].name, name)) {
                    patch_jump_to_target(label_patches[w].patch_at, code_len);
                } else {
                    label_patches[r++] = label_patches[w];
                }
            }
            label_patch_count = r;
            return;
        }
        lexer_restore(save);
    }

    if (is_type_start(cur_tok.type)) {
        parse_local_decl();
        expect(T_SEMI, "expected ';' after declaration");
        return;
    }

    if (cur_tok.type == T_SEMI) { next_token(); return; }

    parse_expr();
    expect(T_SEMI, "expected ';'");
}

static void parse_block(void) {
    expect(T_LBRACE, "expected '{'");
    while (!cc_error_flag && cur_tok.type != T_RBRACE && cur_tok.type != T_EOF) {
        parse_stmt();
    }
    expect(T_RBRACE, "expected '}'");
}

// ------------------------------------------------------------
// Top level: functions and globals
// ------------------------------------------------------------

// Parameter names/types are scanned before any codegen decisions are
// made, since a `(...)` parameter list can turn out to belong to
// either a full function definition (`{`) or a standalone forward
// declaration (`;`) - only known after this list and the closing `)`
// have already been consumed.
typedef struct { char name[32]; int is_char; int ptr_depth; } param_scan_t;

static int parse_param_list_prescan(param_scan_t* out, int max_out) {
    int n = 0;
    if (cur_tok.type == T_VOID) { next_token(); return 0; }
    if (cur_tok.type == T_RPAREN) return 0;
    for (;;) {
        int is_char = consume_scalar_type();
        if (cc_error_flag) return n;
        int ptr_depth = 0;
        while (cur_tok.type == T_STAR) { ptr_depth++; next_token(); }
        if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected parameter name"); return n; }
        if (n >= max_out) { cc_seterr(cur_tok.line, "too many parameters"); return n; }
        int k = 0; while (cur_tok.sval[k] && k < 31) { out[n].name[k] = cur_tok.sval[k]; k++; } out[n].name[k] = 0;
        out[n].is_char = is_char;
        out[n].ptr_depth = ptr_depth;
        n++;
        next_token();
        if (cur_tok.type == T_COMMA) { next_token(); continue; }
        break;
    }
    return n;
}

static void parse_function(const char* name) {
    param_scan_t params[MAX_LOCALS];
    int nparams = parse_param_list_prescan(params, MAX_LOCALS);
    if (cc_error_flag) return;
    expect(T_RPAREN, "expected ')'"); if (cc_error_flag) return;

    if (cur_tok.type == T_SEMI) {
        // Forward declaration only, e.g. `int bar(int x);` - real C
        // requires exactly this (or a prior definition) before a
        // function can be called ahead of its definition, which is
        // what makes mutual recursion possible.
        next_token();
        func_t* existing = find_func(name);
        if (existing) {
            if (existing->nparams != nparams) {
                cc_seterr(cur_tok.line, "conflicting declaration for function");
                return;
            }
        } else {
            if (func_count >= MAX_FUNCS) { cc_seterr(cur_tok.line, "too many functions"); return; }
            func_t* fn = &funcs[func_count++];
            int i = 0; while (name[i] && i < 31) { fn->name[i] = name[i]; i++; } fn->name[i] = 0;
            fn->addr = 0; // declared, not yet defined
            fn->nparams = nparams;
        }
        return;
    }

    func_t* fn = find_func(name);
    if (fn) {
        if (fn->addr != 0) { cc_seterr(cur_tok.line, "redefinition of function"); return; }
        if (fn->nparams != nparams) { cc_seterr(cur_tok.line, "definition disagrees with earlier declaration"); return; }
    } else {
        if (func_count >= MAX_FUNCS) { cc_seterr(cur_tok.line, "too many functions"); return; }
        fn = &funcs[func_count++];
        int i = 0; while (name[i] && i < 31) { fn->name[i] = name[i]; i++; } fn->name[i] = 0;
        fn->nparams = nparams;
    }
    fn->addr = (uint32_t)code_buf + code_len;
    cur_func_start = fn->addr;

    local_count = 0;
    next_local_offset = 0;
    next_param_offset = 8;
    func_label_count = 0;
    label_patch_count = 0;

    // prologue
    emit_u8(0x55);             // push ebp
    emit_u8(0x89); emit_u8(0xE5); // mov ebp, esp
    uint32_t frame_patch = code_len;
    emit_u8(0x81); emit_u8(0xEC); emit_u32(0); // sub esp, <patched later>

    {
        int k;
        for (k = 0; k < nparams; k++) {
            int m = 0; while (params[k].name[m] && m < 31) { locals[local_count].name[m] = params[k].name[m]; m++; }
            locals[local_count].name[m] = 0;
            locals[local_count].offset = next_param_offset;
            locals[local_count].is_array = 0;
            locals[local_count].array_len = 0;
            locals[local_count].elem_size = params[k].is_char ? 1 : 4;
            locals[local_count].ptr_depth = params[k].ptr_depth;
            next_param_offset += 4;
            local_count++;
        }
    }

    // Call sites push arguments left-to-right (evaluation order), which
    // is the reverse of what's needed for the first-declared parameter
    // to land at [ebp+8]. Fix up the offsets we assigned above (which
    // assumed sequential 8,12,16,...) to match the actual layout:
    // the LAST-pushed (last-declared) parameter is the one closest to
    // the return address.
    {
        int k;
        for (k = 0; k < nparams; k++) {
            locals[k].offset = 4 * nparams + 12 - locals[k].offset;
        }
    }

    parse_block();
    if (cc_error_flag) return;

    // Any goto that never found its label is a compile error, exactly
    // as in real C.
    if (label_patch_count > 0) {
        cc_seterr(label_patches[0].line, "goto to undefined label");
        return;
    }

    // implicit "return 0;" if the function falls through without one
    gen_mov_eax_imm32(0);
    emit_u8(0xC9); // leave
    emit_u8(0xC3); // ret

    // Patch the stack frame size using the actual accumulated offset
    // (not local_count*4 - arrays can use many slots per declaration,
    // so a per-declaration count would undercount the real space needed).
    patch_u32(frame_patch + 2, (uint32_t)(-next_local_offset + 16)); // +16 slack, cheap and safe
}

static void parse_program(void) {
    while (!cc_error_flag && cur_tok.type != T_EOF) {
        if (cur_tok.type == T_FLOAT || cur_tok.type == T_DOUBLE) {
            int is_double = (cur_tok.type == T_DOUBLE);
            next_token();
            for (;;) {
                if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected identifier"); return; }
                char name[64];
                int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
                next_token();

                if (cur_tok.type == T_LPAREN) {
                    cc_seterr(cur_tok.line, "functions with a float/double return type or parameters are not supported yet");
                    return;
                }

                if (global_count >= MAX_GLOBALS) { cc_seterr(cur_tok.line, "too many globals"); return; }
                int sz = is_double ? 8 : 4;
                if (float_global_pool_used + sz > FLOAT_GLOBAL_POOL_BYTES) {
                    cc_seterr(cur_tok.line, "too many float/double globals");
                    return;
                }
                int off = float_global_pool_used;
                uint32_t addr = (uint32_t)&float_global_pool[off];
                float_global_pool_used += sz;

                double init = 0.0;
                if (cur_tok.type == T_ASSIGN) {
                    next_token();
                    int neg = 0;
                    if (cur_tok.type == T_MINUS) { neg = 1; next_token(); }
                    if (cur_tok.type != T_NUM) { cc_seterr(cur_tok.line, "global initializer must be a constant"); return; }
                    init = cur_tok.is_float_lit ? cur_tok.fval : (double)cur_tok.ival;
                    if (neg) init = -init;
                    next_token();
                }

                if (is_double) {
                    double dv = init; uint8_t* b = (uint8_t*)&dv;
                    int k; for (k = 0; k < 8; k++) float_global_pool[off + k] = b[k];
                } else {
                    float fv = (float)init; uint8_t* b = (uint8_t*)&fv;
                    int k; for (k = 0; k < 4; k++) float_global_pool[off + k] = b[k];
                }

                int j = 0; while (name[j] && j < 31) { globals[global_count].name[j] = name[j]; j++; } globals[global_count].name[j] = 0;
                globals[global_count].addr = addr;
                globals[global_count].is_array = 0;
                globals[global_count].array_len = 0;
                globals[global_count].elem_size = sz;
                globals[global_count].ptr_depth = 0;
                globals[global_count].is_fp = 1;
                globals[global_count].fp_double = is_double;
                globals_fp_init[global_count] = init;
                global_count++;

                if (cur_tok.type != T_COMMA) break;
                next_token();
            }
            expect(T_SEMI, "expected ';' after global declaration");
            continue;
        }

        int is_void = (cur_tok.type == T_VOID);
        int is_char = 0;
        if (!is_void && !is_type_start(cur_tok.type)) {
            cc_seterr(cur_tok.line, "expected a C type");
            return;
        }
        if (is_void) next_token();
        else is_char = consume_scalar_type();
        int ptr_depth = 0;
        while (cur_tok.type == T_STAR) { ptr_depth++; next_token(); }
        if (cur_tok.type != T_IDENT) { cc_seterr(cur_tok.line, "expected identifier"); return; }
        char name[64];
        int i = 0; while (cur_tok.sval[i]) { name[i] = cur_tok.sval[i]; i++; } name[i] = 0;
        int decl_line = cur_tok.line;
        next_token();

        if (cur_tok.type == T_LPAREN) {
            next_token();
            parse_function(name);
        } else if (is_void) {
            cc_seterr(decl_line, "'void' is only valid as a function return type");
            return;
        } else {
            int elem_size = is_char ? 1 : 4;
            for (;;) {
                if (global_count >= MAX_GLOBALS) {
                    cc_seterr(cur_tok.line, "too many globals");
                    return;
                }

                if (cur_tok.type == T_LBRACKET) {
                    next_token();
                    if (cur_tok.type != T_NUM || cur_tok.ival <= 0) {
                        cc_seterr(cur_tok.line, "array size must be a positive constant");
                        return;
                    }
                    int len = cur_tok.ival;
                    next_token();
                    expect(T_RBRACKET, "expected ']'");
                    if (cc_error_flag) return;
                    if (global_array_pool_used + len * elem_size > GLOBAL_ARRAY_POOL_BYTES) {
                        cc_seterr(cur_tok.line, "global arrays too large in total");
                        return;
                    }

                    uint32_t base =
                        (uint32_t)&global_array_pool[global_array_pool_used];
                    global_array_pool_used += len * elem_size;

                    int j = 0;
                    while (name[j] && j < 31) {
                        globals[global_count].name[j] = name[j];
                        j++;
                    }
                    globals[global_count].name[j] = 0;
                    globals[global_count].addr = base;
                    globals[global_count].is_array = 1;
                    globals[global_count].array_len = len;
                    globals[global_count].elem_size = elem_size;
                    globals[global_count].ptr_depth = 0;
                    global_count++;
                } else {
                    int32_t init = 0;
                    if (cur_tok.type == T_ASSIGN) {
                        next_token();
                        int neg = 0;
                        if (cur_tok.type == T_MINUS) {
                            neg = 1;
                            next_token();
                        }
                        if (cur_tok.type != T_NUM) {
                            cc_seterr(cur_tok.line,
                                      "global initializer must be a constant");
                            return;
                        }
                        init = neg ? -cur_tok.ival : cur_tok.ival;
                        next_token();
                    }

                    globals_data[global_count] = init;
                    int j = 0;
                    while (name[j] && j < 31) {
                        globals[global_count].name[j] = name[j];
                        j++;
                    }
                    globals[global_count].name[j] = 0;
                    globals[global_count].addr =
                        (uint32_t)&globals_data[global_count];
                    globals[global_count].is_array = 0;
                    globals[global_count].array_len = 0;
                    globals[global_count].elem_size = elem_size;
                    globals[global_count].ptr_depth = ptr_depth;
                    global_count++;
                }

                if (cur_tok.type != T_COMMA)
                    break;

                next_token();
                ptr_depth = 0;
                while (cur_tok.type == T_STAR) { ptr_depth++; next_token(); }
                if (cur_tok.type != T_IDENT) {
                    cc_seterr(cur_tok.line,
                              "expected identifier after ','");
                    return;
                }
                int n = 0;
                while (cur_tok.sval[n] && n < 63) {
                    name[n] = cur_tok.sval[n];
                    n++;
                }
                name[n] = 0;
                next_token();
            }

            expect(T_SEMI, "expected ';' after global declaration");
        }
    }
}

// ------------------------------------------------------------
// Builtin runtime helpers (called directly from generated code)
// ------------------------------------------------------------

static int cc_bin_magic_ok(const uint8_t* image, unsigned int len) {
    return image && len >= TJBIN_HEADER_SIZE &&
           image[0] == 'T' && image[1] == 'J' &&
           image[2] == 'B' && image[3] == 'N' &&
           image[4] == 1;
}

static void cc_bin_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t cc_bin_get_u32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// ------------------------------------------------------------
// Entry point
// ------------------------------------------------------------
static int cc_compile(const char* source, unsigned int len, func_t** out_mainfn);

// ------------------------------------------------------------
// Style check: statements inside a block must be indented with a
// literal tab character. A line consisting only of a closing brace
// (optionally followed by more code, e.g. "} else {") is allowed to
// sit back out at the enclosing indent level. Braces/newlines inside
// string literals, char literals, and comments are not counted.
// ------------------------------------------------------------
static void cc_check_indentation(const char* s, unsigned int len) {
    unsigned int i = 0;
    int line = 1;
    int depth = 0;
    int line_start_depth = 0;
    int at_line_start = 1;

    while (i < len) {
        char c = s[i];

        if (at_line_start) {
            unsigned int j = i;
            while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
            int line_is_blank = (j >= len || s[j] == '\n' || s[j] == '\r');
            int first_is_close_brace = (j < len && s[j] == '}');

            if (line_start_depth > 0 && !line_is_blank && !first_is_close_brace && c != '\t') {
                cc_seterr(line, "statements must be indented with a tab character");
                return;
            }
            at_line_start = 0;
        }

        if (c == '"' || c == '\'') {
            char quote = c;
            i++;
            while (i < len && s[i] != quote) {
                if (s[i] == '\\' && i + 1 < len) i++;
                else if (s[i] == '\n') { line++; line_start_depth = depth; at_line_start = 1; }
                i++;
            }
            if (i < len) i++;
            continue;
        }

        if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            while (i < len && s[i] != '\n') i++;
            continue;
        }

        if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            while (i < len && !(s[i] == '*' && i + 1 < len && s[i + 1] == '/')) {
                if (s[i] == '\n') { line++; line_start_depth = depth; at_line_start = 1; }
                i++;
            }
            i = (i < len) ? i + 2 : len;
            continue;
        }

        if (c == '{') depth++;
        else if (c == '}' && depth > 0) depth--;

        if (c == '\n') {
            line++;
            line_start_depth = depth;
            at_line_start = 1;
        }

        i++;
    }
}

int cc_compile_to_binary(const char* source, unsigned int len,
                         uint8_t* out, unsigned int out_cap,
                         unsigned int* out_len) {
    func_t* entryfn;

    if (!out || !out_len || out_cap < TJBIN_HEADER_SIZE) return -1;
    if (cc_compile(source, len, &entryfn) != 0) return -1;

    /*
     * Append a tiny startup wrapper. It reinitializes file-scope globals
     * every time the binary starts, so a program's state cannot leak from
     * the previous binary that happened to use the compiler's shared
     * runtime storage. The wrapper then calls the first function definition as the program entry point.
     */
    uint32_t entry = code_len;

    for (int i = 0; i < global_count; i++) {
        if (globals[i].is_array) continue;
        if (globals[i].is_fp) {
            uint32_t const_addr = emit_float_const(globals_fp_init[i], globals[i].fp_double);
            if (cc_error_flag) return -1;
            gen_fld_abs(const_addr, globals[i].fp_double);
            gen_fstp_abs(globals[i].addr, globals[i].fp_double);
        } else {
            gen_mov_abs_imm32(globals[i].addr, (uint32_t)globals_data[i]);
        }
    }

    if (global_array_pool_used > 0) {
        /* rep stosb: clear the entire array backing pool one byte at a
         * time. global_array_pool_used is tracked in bytes, so this
         * correctly zeroes byte-packed char arrays as well as
         * dword-packed int arrays sharing the same pool. */
        gen_mov_eax_imm32(0);
        emit_u8(0xBF); emit_u32((uint32_t)&global_array_pool[0]); /* mov edi, base */
        emit_u8(0xB9); emit_u32((uint32_t)global_array_pool_used); /* byte count */
        emit_u8(0xF3); emit_u8(0xAA); /* rep stosb, ECX bytes */
    }

    gen_call_abs(entryfn->addr);
    emit_u8(0xC3); /* ret */

    if (entry >= code_len || code_len + TJBIN_HEADER_SIZE > out_cap) {
        print("Compiler: output binary is too large\n");
        return -1;
    }

    out[0] = 'T'; out[1] = 'J'; out[2] = 'B'; out[3] = 'N';
    out[4] = 1; out[5] = 0; out[6] = 0; out[7] = 0;
    cc_bin_u32(out + 8, entry);

    for (uint32_t i = 0; i < code_len; i++)
        out[TJBIN_HEADER_SIZE + i] = code_buf[i];

    *out_len = TJBIN_HEADER_SIZE + code_len;
    return 0;
}

int cc_execute_binary(const uint8_t* image, unsigned int len) {
    cc_heap_reset();
    if (!cc_bin_magic_ok(image, len)) {
        print("exec: not a TanjaOS binary\n");
        return -1;
    }

    uint32_t code_size = len - TJBIN_HEADER_SIZE;
    uint32_t entry = cc_bin_get_u32(image + 8);

    if (code_size > CODE_BUF_SIZE || entry >= code_size) {
        print("exec: invalid TanjaOS binary\n");
        return -1;
    }

    for (uint32_t i = 0; i < code_size; i++)
        code_buf[i] = image[TJBIN_HEADER_SIZE + i];

    code_len = code_size;

    int (*entry_fn)(void) =
        (int (*)(void))(void*)((uint32_t)code_buf + entry);

    int rc = entry_fn();
    return rc;
}

// Debug/test accessor - lets a host-side test harness get the code
// buffer's address and length to inspect (e.g. disassemble) what was
// generated, without needing to execute it. Not used by the kernel.
void* cc_debug_codebuf(void) { return code_buf; }
unsigned int cc_debug_codebuf_size(void) { return CODE_BUF_SIZE; }
unsigned int cc_debug_codelen(void) { return code_len; }

// Returns 0 on success (an entry function found and ready to call), -1 on
// failure (error already printed). Doesn't execute anything - split
// out from cc_compile_and_run so host-side tooling can inspect
// generated code without ever jumping into it (this kernel has no
// paging, so 32-bit machine code sitting in a plain buffer is safe to
// execute there; on a 64-bit host it would not be).
static int cc_compile(const char* source, unsigned int len, func_t** out_mainfn) {
    src = source; src_len = len; src_pos = 0; cur_line = 1;
    code_len = 0;
    cc_error_flag = 0; cc_error_line = 0; cc_error_msg[0] = 0;
    local_count = 0; func_count = 0; global_count = 0; token_count = 0; global_array_pool_used = 0;
    float_const_pool_used = 0; float_global_pool_used = 0;
    pending_call_count = 0;

    cc_check_indentation(source, len);

    if (!cc_error_flag) {
        next_token();
        parse_program();
    }

    if (!cc_error_flag) {
        int i;
        for (i = 0; i < pending_call_count; i++) {
            func_t* target = &funcs[pending_calls[i].func_index];
            if (target->addr == 0) {
                cc_seterr(pending_calls[i].line, "function declared but never defined");
                break;
            }
            patch_call_target(pending_calls[i].patch_at, target->addr);
        }
    }

    if (cc_error_flag) {
        print("Compile error (line ");
        print_dec((uint32_t)cc_error_line);
        print("): ");
        print(cc_error_msg);
        print("\n");
        return -1;
    }

    if (func_count == 0) {
        print("Compile error: no function definition\n");
        return -1;
    }

    /* TanjaOS C has no linker or separate program-start convention.
     * The first function ACTUALLY DEFINED (not merely forward-declared)
     * in the source is the program entry point, so users can name it
     * anything: int hello() { ... } works directly. A function's
     * array index reflects when it was first MENTIONED (which for a
     * forward-declared function is its prototype, not its body), so
     * entry point selection instead picks whichever defined function
     * has the lowest code address - code_buf fills up in the order
     * bodies are actually emitted, so the lowest address is always
     * the one that appeared first as a real definition. */
    int i;
    func_t* entry = 0;
    for (i = 0; i < func_count; i++) {
        if (funcs[i].addr != 0 && (!entry || funcs[i].addr < entry->addr))
            entry = &funcs[i];
    }
    if (entry) { *out_mainfn = entry; return 0; }
    print("Compile error: no function definition\n");
    return -1;
}

// Test-only entry point: compiles but never executes anything, safe
// to call from a host-side test harness on any host architecture.
int cc_compile_only(const char* source, unsigned int len) {
    func_t* entryfn;
    return cc_compile(source, len, &entryfn);
}

void cc_compile_and_run(const char* source, unsigned int len) {
    cc_heap_reset();
    func_t* entryfn;
    if (cc_compile(source, len, &entryfn) != 0) return;

    int (*entry)(void) = (int (*)(void))(void*)entryfn->addr;
    int rc = entry();
}
