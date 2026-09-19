#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../include/fs.h"
#include "../include/store.h"
#include "../include/idt.h"
#include "../include/utf8.h"

// ============================================================
// VGA CONSTANTS
// ============================================================

#define VGA_COLOR (0x0F << 8)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

uint16_t* VGA = (uint16_t*)0xB8000;
int cursor = 0;

// ============================================================
// FPU INIT
// ============================================================
// Loaded ELF32 programs can use float/double via x87 FPU
// instructions (fld/fadd/fstp/etc). The FPU is present on any CPU
// this kernel targets, but its control state isn't guaranteed sane
// coming out of reset/bootloader handoff: EM (CR0 bit 2) must be
// clear so FPU opcodes execute natively instead of faulting, MP
// (CR0 bit 1) should be set per the standard convention, and FNINIT
// resets the FPU to a clean state (empty stack, all exceptions
// masked, default control word) so a stray uninitialized FPU state
// can't corrupt the first floating-point program that runs.
static inline void fpu_init(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2); // clear EM
    cr0 |= (1u << 1);  // set MP
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("fninit");
}

// ============================================================
// GLOBAL VARIABLES
// ============================================================

uint32_t boot_ticks = 0;

typedef struct Command {
    char name[32];
    void (*func)(char* args);
    struct Command* next;
} Command;

Command* cmd_table = 0;
int cmd_count = 0;

#define CMD_POOL_SIZE 128

Command cmd_pool[CMD_POOL_SIZE];
int cmd_pool_index = 0;

int caps_lock = 0;

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_ENTER  0x84
#define KEY_BACKSPACE 0x85

// ============================================================
// USER CONFIGURATION
// ============================================================

#define MAX_USERNAME 32
#define MAX_PASSWORD 32
#define MAX_HOSTNAME 64

typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    char hostname[MAX_HOSTNAME];
    int is_setup;
} user_config_t;

user_config_t config = { .is_setup = 0 };

int shell_exit_flag = 0;

// Packs/unpacks `config` (login + hostname) into a flat blob so
// store.c can save it alongside the filesystem, letting the setup
// wizard's "create a login" step actually stick across reboots
// instead of running every single boot.
uint32_t config_store_size(void) {
    return MAX_USERNAME + MAX_PASSWORD + MAX_HOSTNAME + 1; // +1 for is_setup
}

int config_serialize(uint8_t* buf, uint32_t buf_size) {
    if (!buf || buf_size < config_store_size()) return -1;
    uint32_t p = 0;
    int i;
    for (i = 0; i < MAX_USERNAME; i++) buf[p++] = (uint8_t)config.username[i];
    for (i = 0; i < MAX_PASSWORD; i++) buf[p++] = (uint8_t)config.password[i];
    for (i = 0; i < MAX_HOSTNAME; i++) buf[p++] = (uint8_t)config.hostname[i];
    buf[p++] = (uint8_t)(config.is_setup ? 1 : 0);
    return 0;
}

int config_deserialize(const uint8_t* buf, uint32_t buf_size) {
    if (!buf || buf_size < config_store_size()) return -1;
    uint32_t p = 0;
    int i;
    for (i = 0; i < MAX_USERNAME; i++) config.username[i] = (char)buf[p++];
    for (i = 0; i < MAX_PASSWORD; i++) config.password[i] = (char)buf[p++];
    for (i = 0; i < MAX_HOSTNAME; i++) config.hostname[i] = (char)buf[p++];
    config.is_setup = buf[p++] ? 1 : 0;
    return 0;
}

// Reset the account configuration to the same state as a fresh install.
// This deliberately clears all fields and marks setup as incomplete.
void config_reset(void) {
    int i;
    for (i = 0; i < MAX_USERNAME; i++) config.username[i] = 0;
    for (i = 0; i < MAX_PASSWORD; i++) config.password[i] = 0;
    for (i = 0; i < MAX_HOSTNAME; i++) config.hostname[i] = 0;
    config.is_setup = 0;
}

// ============================================================
// PORT I/O
// ============================================================

void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

void timer_init()
{
    // PIT channel 0, rate generator mode, ~1000Hz (~1ms per tick).
    // idt_init() is what actually turns these ticks into a counted
    // value via IRQ0 - this alone just sets the rate.
    outb(0x43, 0x34);

    uint16_t divisor = 1193180 / 1000; // ~1ms ticks

    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

void timer_delay_ms(uint32_t ms)
{
    uint32_t start = get_uptime_ms();
    while (get_uptime_ms() - start < ms) {
        asm volatile("hlt"); // sleep until the next interrupt (IRQ0 wakes us every ~1ms)
    }
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================
// CURSOR
// ============================================================

void sync_cursor() {
    int max = VGA_WIDTH * VGA_HEIGHT - 1;
    if (cursor < 0) cursor = 0;
    if (cursor > max) cursor = max;

    // 1. Configure Cursor Shape to a Solid Block
    outb(0x3D4, 0x0A);                   // Select Cursor Start Register
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 0); // Start at scanline 0 (Top)
    
    outb(0x3D4, 0x0B);                   // Select Cursor End Register
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);// End at scanline 15 (Bottom)

    // 2. Update Cursor Position
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(cursor & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((cursor >> 8) & 0xFF));
}

void timer_handler()
{
    boot_ticks++;

    outb(0x20, 0x20); // send EOI
}

void underline_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0D);

    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
}

// ============================================================
// SCREEN FUNCTIONS
// ============================================================

void scroll() {
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        VGA[i] = VGA[i + VGA_WIDTH];
    for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';
    cursor = VGA_WIDTH * (VGA_HEIGHT - 1);
}

/* Draw a single already-decoded cell: control codes (newline,
 * backspace, tab) plus one printable character, then scroll if
 * needed.  This is the raw drawing primitive both the byte
 * interface and the UTF-8 decoder below funnel into. */
static void putc_draw(char c, uint16_t color) {
    if (c == '\n') {
        cursor = ((cursor / VGA_WIDTH) + 1) * VGA_WIDTH;
    } else if (c == '\b') {
        if (cursor > 0) {
            cursor--;
            VGA[cursor] = color | ' ';
        }
    } else if (c == '\t') {
        // Advance to the next multiple-of-4 column. TanjaOS uses
        // four-column tabs consistently with the editor.
        // Nothing is drawn for the tab itself -
        // previously this fell through every branch below (tab is
        // 0x09, less than ' ' at 0x20) and was silently dropped:
        // not printed, cursor not advanced, as if it never existed.
        int col = cursor % VGA_WIDTH;
        int next = (col / 4 + 1) * 4;
        if (next > VGA_WIDTH) next = VGA_WIDTH;
        cursor += (next - col);
    } else if (c >= ' ') {
        VGA[cursor] = color | (uint8_t)c;
        cursor++;
    }
    if (cursor >= VGA_WIDTH * VGA_HEIGHT) scroll();
}

/* One shared UTF-8 decoder for the whole console stream so that
 * sequences are tracked correctly even when output alternates
 * between putc(), print() and print_n() (e.g. cat piped output).
 *
 * Why: the VGA console only has CP437 glyphs, so printing UTF-8
 * one raw byte at a time showed punctuation like “ ” — as two or
 * three jumbled characters.  Now a full sequence is decoded and
 * drawn as a single ASCII cell: quotes become ", dashes become -,
 * ʼ becomes ', and anything unmappable becomes '?'.  Bytes that
 * are not valid UTF-8 (e.g. binary junk) still draw raw, exactly
 * like the old byte-oriented behavior. */
static utf8_feed_t console_utf8 = {{0, 0, 0, 0}, 0, 0};

static void console_feed(char c, uint16_t color) {
    uint32_t cp;
    int r = utf8_feed(&console_utf8, (unsigned char)c, &cp);
    char cell;

    if (r == 0)
        return;                       /* mid-sequence, nothing to draw yet */
    if (r == 2) {
        putc_draw(c, color);          /* not UTF-8: old raw behavior */
        return;
    }
    cell = utf8_to_cell(cp);
    if (cell)
        putc_draw(cell, color);
    else
        putc_draw('?', color);        /* valid UTF-8, no CP437 glyph */
}

void putc_color(char c, uint16_t color) {
    console_feed(c, color);
    sync_cursor();
}

void putc(char c) {
    putc_color(c, VGA_COLOR);
}

/* ISO C-style console functions exposed to TanjaOS C programs. */
int get_key(void);
int putchar(int c) { putc((char)c); return c; }
int getchar(void) { return get_key(); }
int puts(const char* s) { if (s) { while (*s) putc(*s++); } putc('\n'); return 0; }

void print(const char* s) {
    if (!s) return;
    while (*s) putc(*s++);
}

/* Fast bulk console output.  Unlike print()/putc(), this only updates the
 * hardware cursor once for the whole chunk.  Commands such as cat can use
 * this to avoid one sync_cursor() call per byte. */
void print_n(const char* s, uint32_t len) {
    if (!s || len == 0) return;

    /* Fast bulk console output: decode UTF-8 (via the shared
     * console decoder, same as putc/print) but only update the
     * hardware cursor once for the whole chunk. */
    for (uint32_t i = 0; i < len; i++)
        console_feed(s[i], VGA_COLOR);

    sync_cursor();
}

char* itoa(int value, char* buf, int base);
char* strupr(char* s);

/* Formatted printing for user programs - supports the common subset:
 * %s %c %d/%i %u %x %X %p %% (no width/precision/flags). */
static void printf_common(const char* fmt, va_list args) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { putc(*p); continue; }
        p++;
        char tmp[34];
        switch (*p) {
        case 0:   return;
        case '%': putc('%'); break;
        case 's': { const char* s = va_arg(args, const char*); print(s ? s : "(null)"); break; }
        case 'c': putc((char)va_arg(args, int)); break;
        case 'd': case 'i': print(itoa(va_arg(args, int), tmp, 10)); break;
        case 'u': { unsigned int uv = va_arg(args, unsigned int); int k = 0; char ut[34];
                    if (uv == 0) ut[k++] = '0';
                    while (uv) { ut[k++] = (char)('0' + (int)(uv % 10)); uv /= 10; }
                    while (k) putc(ut[--k]); break; }
                    (void)tmp;
        case 'x': itoa(va_arg(args, unsigned int), tmp, 16); print(tmp); break;
        case 'X': { char* s = itoa(va_arg(args, unsigned int), tmp, 16); strupr(s); print(s); break; }
        case 'p': print("0x"); itoa(va_arg(args, unsigned int), tmp, 16); print(tmp); break;
        default:  putc('%'); putc(*p); break;
        }
    }
}

int printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf_common(fmt, args);
    va_end(args);
    return 0;
}

void print_color(const char* s, uint16_t color) {
    if (!s) return;
    while (*s) putc_color(*s++, color);
}

void clear_screen() {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';
    cursor = 0;
    sync_cursor();
}

// ============================================================
// NUMBER PRINTING
// ============================================================

void print_hex(uint32_t n) {
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[11];
    buffer[0] = '0'; buffer[1] = 'x'; buffer[10] = 0;
    for (int i = 9; i >= 2; i--) { buffer[i] = hex_chars[n & 0xF]; n >>= 4; }
    print(buffer);
}

void print_dec(uint32_t n) {
    if (n == 0) { putc('0'); return; }
    char buffer[11]; int pos = 10; buffer[pos] = 0;
    while (n > 0 && pos > 0) { pos--; buffer[pos] = '0' + (n % 10); n /= 10; }
    print(&buffer[pos]);
}

void boot_log(const char* msg)
{
    uint32_t t = get_uptime_ms();

    print("[ ");

    print_dec(t / 1000);

    print(".");

    uint32_t ms = t % 1000;

    if (ms < 100)
        putc('0');
    if (ms < 10)
        putc('0');

    print_dec(ms);

    print(" ] ");

    print(msg);
    print("\n");
}

void print_dec_pad(uint32_t n, int width) {
    char buffer[11]; int pos = 10; buffer[pos] = 0;
    if (n == 0) buffer[--pos] = '0';
    while (n > 0 && pos > 0) { pos--; buffer[pos] = '0' + (n % 10); n /= 10; }
    int len = 10 - pos;
    for (int i = 0; i < width - len; i++) putc(' ');
    print(&buffer[pos]);
}

// ============================================================
// KEYBOARD
// ============================================================

int shift = 0;
int ctrl = 0;

char keymap[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',
    8,9,'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0,'*',0,' '
};

char keymap_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',
    8,9,'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',
    0,'*',0,' '
};

char keymap_caps[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',
    8,9,'Q','W','E','R','T','Y','U','I','O','P','[',']','\n',
    0,'A','S','D','F','G','H','J','K','L',';','\'','`',
    0,'\\','Z','X','C','V','B','N','M',',','.','/',
    0,'*',0,' '
};

int get_key() {
    while (1) {
        if (!(inb(0x64) & 1)) {
            asm volatile ("hlt");
            continue;
        }
        uint8_t sc = inb(0x60);
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0x1D) { ctrl = 1; continue; }
        if (sc == 0x9D) { ctrl = 0; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc == 0x3A) { caps_lock = !caps_lock; continue; }
        if (sc == 0xE0) {
            while (!(inb(0x64) & 1)) continue;
            uint8_t ext = inb(0x60);
            if (ext == 0x48) return KEY_UP;
            if (ext == 0x50) return KEY_DOWN;
            if (ext == 0x4B) return KEY_LEFT;
            if (ext == 0x4D) return KEY_RIGHT;
            continue;
        }
        if (sc & 0x80) continue;
        if (sc >= 128) continue;
        /* Caps Lock should only flip the case of letters, not act like
         * a second shift key for symbols/numbers. So: for letter keys,
         * shift and caps_lock cancel each other out (shift while caps
         * is on types lowercase). For everything else, only shift
         * matters, exactly like caps_lock isn't pressed at all. */
        char res;
        char base = keymap[sc];
        int is_letter = (base >= 'a' && base <= 'z');
        int use_shift_map = is_letter ? (shift ^ caps_lock) : shift;
        res = use_shift_map ? keymap_shift[sc] : keymap[sc];
        if (res == 0) continue;
        if (ctrl && res >= 'a' && res <= 'z')
            return res - 'a' + 1;      /* real control codes (^A..^Z) */
        return res;
    }
}

int key_available(void)
{
    return (inb(0x64) & 1);
}

// ============================================================
// STRING HELPERS
// ============================================================

int streq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void clean(char* s) {
    if (!s) return;
    for (int i = 0; s[i]; i++)
        if (s[i] == '\n' || s[i] == '\r') s[i] = 0;
}

// ============================================================
// INPUT
// ============================================================

#define INPUT_BUFFER_SIZE 65536

static char input_line[INPUT_BUFFER_SIZE];

void read_line(char* buffer, int max_len) {
    char* line = input_line;
    int pos = 0;
    int len = 0;
    int prompt_start = cursor;

    while (1) {

        int key = get_key();

        if (key == '\n' || key == KEY_ENTER) {
            putc('\n');
            break;
        }

        if (key == 8 || key == KEY_BACKSPACE) {

            if (pos > 0) {

                for (int i = pos - 1; i < len - 1; i++)
                    line[i] = line[i + 1];

                len--;
                pos--;

                line[len] = 0;

                cursor = prompt_start;

                for (int i = 0; i < len; i++) {
                    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
                        scroll();
                        prompt_start -= VGA_WIDTH;
                        cursor = prompt_start + i;
                    }
                    VGA[cursor] = VGA_COLOR | line[i];
                    cursor++;
                }

                if (cursor < VGA_WIDTH * VGA_HEIGHT)
                    VGA[cursor] = VGA_COLOR | ' ';

                cursor = prompt_start + pos;
                sync_cursor();
            }

            continue;
        }


        if (key == KEY_LEFT) {

            if (pos > 0) {
                pos--;
                cursor = prompt_start + pos;
                sync_cursor();
            }

            continue;
        }


        if (key == KEY_RIGHT) {

            if (pos < len) {
                pos++;
                cursor = prompt_start + pos;
                sync_cursor();
            }

            continue;
        }


        if (key == KEY_UP || key == KEY_DOWN)
            continue;


        if (key >= 32 && key <= 126) {

            if (len < INPUT_BUFFER_SIZE - 1) {

                for (int i = len; i > pos; i--)
                    line[i] = line[i - 1];

                line[pos] = key;

                len++;
                pos++;

                line[len] = 0;


                cursor = prompt_start;

                for (int i = 0; i < len; i++) {
                    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
                        scroll();
                        prompt_start -= VGA_WIDTH;
                        cursor = prompt_start + i;
                    }
                    VGA[cursor] = VGA_COLOR | line[i];
                    cursor++;
                }

                if (prompt_start + pos >= VGA_WIDTH * VGA_HEIGHT) {
                    scroll();
                    prompt_start -= VGA_WIDTH;
                }

                cursor = prompt_start + pos;
                sync_cursor();
            }
        }
    }


    int copy_len = len;

    if (copy_len >= max_len)
        copy_len = max_len - 1;


    for (int i = 0; i < copy_len; i++)
        buffer[i] = line[i];

    buffer[copy_len] = 0;
}

int read_int(void) {
    char buffer[32];
    int sign = 1;
    int value = 0;
    int i = 0;

    read_line(buffer, sizeof(buffer));

    while (buffer[i] == ' ' || buffer[i] == '\t')
        i++;

    if (buffer[i] == '-') {
        sign = -1;
        i++;
    } else if (buffer[i] == '+') {
        i++;
    }

    while (buffer[i] >= '0' && buffer[i] <= '9') {
        value = value * 10 + (buffer[i] - '0');
        i++;
    }

    return value * sign;
}


int strlen(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, unsigned int n) {
    unsigned int i = 0;
    if (n == 0) return 0;
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (i < n && a[i] && b[i] && a[i] == b[i]) i++;
    if (i == n) return 0;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

// Non-standard but ubiquitous in freestanding/embedded C (no sprintf
// available to build one out of "%d"). Converts `value` to a string in
// the given base and writes it (with a '-' sign for negative values in
// base 10 only, matching the common convention) into `buf`, which must
// be big enough - 34 bytes covers the worst case (base 2, INT_MIN, sign,
// and the null terminator). Returns buf, matching itoa's usual signature.
char* itoa(int value, char* buf, int base) {
    if (!buf) return buf;
    if (base < 2 || base > 16) { buf[0] = 0; return buf; }
    char tmp[34];
    int i = 0;
    unsigned int uval;
    int neg = (base == 10 && value < 0);
    uval = neg ? (unsigned int)(-(long)value) : (unsigned int)value;
    if (uval == 0) tmp[i++] = '0';
    while (uval) {
        int d = (int)(uval % (unsigned int)base);
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        uval /= (unsigned int)base;
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

char* strcpy(char* dst, const char* src) {
    char* start = dst;
    if (!dst || !src) return dst;
    while ((*dst++ = *src++)) {}
    return start;
}

char* strncpy(char* dst, const char* src, unsigned int n) {
    unsigned int i = 0;
    if (!dst || !src) return dst;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = 0;
    return dst;
}

char* strcat(char* dst, const char* src) {
    char* start = dst;
    if (!dst || !src) return dst;
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
    return start;
}

char* strchr(const char* s, int c) {
    if (!s) return 0;
    while (*s) {
        if ((unsigned char)*s == (unsigned char)c) return (char*)s;
        s++;
    }
    return c == 0 ? (char*)s : 0;
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    if (!s) return 0;
    while (*s) {
        if ((unsigned char)*s == (unsigned char)c) last = s;
        s++;
    }
    if (c == 0) return (char*)s;
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    int i, j;
    if (!haystack || !needle) return 0;
    if (!needle[0]) return (char*)haystack;
    for (i = 0; haystack[i]; i++) {
        for (j = 0; needle[j] && haystack[i + j] == needle[j]; j++) {}
        if (!needle[j]) return (char*)&haystack[i];
    }
    return 0;
}

void* memset(void* dst, int value, unsigned int count) {
    unsigned char* p = (unsigned char*)dst;
    if (!p) return dst;
    while (count-- > 0) *p++ = (unsigned char)value;
    return dst;
}

void* memcpy(void* dst, const void* src, unsigned int count) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (!d || !s) return dst;
    while (count-- > 0) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, unsigned int count) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned int i;
    if (!d || !s || d == s) return dst;
    if (d < s) {
        for (i = 0; i < count; i++) d[i] = s[i];
    } else {
        for (i = count; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void* a, const void* b, unsigned int count) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    unsigned int i;
    if (!x || !y) return x == y ? 0 : (x ? 1 : -1);
    for (i = 0; i < count; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

int atoi(const char* s) {
    int sign = 1, value = 0, i = 0;
    if (!s) return 0;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v') i++;
    if (s[i] == '-') { sign = -1; i++; }
    else if (s[i] == '+') i++;
    while (s[i] >= '0' && s[i] <= '9') {
        value = value * 10 + (s[i] - '0');
        i++;
    }
    return value * sign;
}

int abs(int n) { return n < 0 ? -n : n; }

static unsigned int c_rand_state = 1;
int rand(void) {
    c_rand_state = c_rand_state * 1103515245u + 12345u;
    return (int)((c_rand_state >> 16) & 0x7FFF);
}
void srand(unsigned int seed) { c_rand_state = seed ? seed : 1; }

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }

// Non-standard but extremely common (POSIX / most C libraries) - case-
// insensitive string comparison. Same shape as strcmp/strncmp above,
// just folding both sides through tolower() before comparing.
int strcasecmp(const char* a, const char* b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, unsigned int n) {
    unsigned int i = 0;
    if (n == 0) return 0;
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    while (i < n && a[i] && b[i] && tolower((unsigned char)a[i]) == tolower((unsigned char)b[i])) i++;
    if (i == n) return 0;
    return tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
}

// Non-standard (MSVC/POSIX-ish convention, not ISO C) but widely used -
// in-place case conversion, returning the same pointer for chaining.
char* strlwr(char* s) {
    char* p = s;
    while (p && *p) { *p = (char)tolower((unsigned char)*p); p++; }
    return s;
}

char* strupr(char* s) {
    char* p = s;
    while (p && *p) { *p = (char)toupper((unsigned char)*p); p++; }
    return s;
}




// ============================================================
// COMMAND SYSTEM
// ============================================================

void register_cmd(const char* name, void (*func)(char* args)) {

    if (cmd_pool_index >= CMD_POOL_SIZE)
        return;

    Command* cmd = &cmd_pool[cmd_pool_index++];

    int i = 0;
    while (name[i] && i < 127) {
        cmd->name[i] = name[i];
        i++;
    }

    cmd->name[i] = 0;
    cmd->func = func;
    cmd->next = 0;


    if (cmd_table == 0) {
        cmd_table = cmd;
    } else {

        Command* current = cmd_table;

        while (current->next)
            current = current->next;

        current->next = cmd;
    }

    cmd_count++;
}

int cmd_exists(const char* name) {
    if (!name || !*name) return 0;

    Command* cmd = cmd_table;
    while (cmd) {
        if (streq(cmd->name, name)) return 1;
        cmd = cmd->next;
    }
    return 0;
}

/* Write every registered built-in command name into `out`, one per
   line ('\n'-separated).  Returns bytes written.  Used by the help
   command to merge built-ins into its unified command list. */
int builtin_names(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    Command* cmd = cmd_table;
    while (cmd) {
        int i = 0;
        while (cmd->name[i] && n + 1 < cap)
            out[n++] = cmd->name[i++];
        if (n + 1 >= cap) break;      /* out of room - stop cleanly */
        out[n++] = '\n';
        cmd = cmd->next;
    }
    out[n] = 0;
    return n;
}

void list_commands(void) {
    print("\nAvailable commands:\n\n");

    Command* cmd = cmd_table;
    int col = 0;

    while (cmd) {
        int len = 0;
        while (cmd->name[len])
            len++;

        // Wrap to next line if this command won't fit
        if (col + len + 3 >= VGA_WIDTH) {
            print("\n");
            col = 0;
        }

        print(cmd->name);
        print(" ");

        col += len + 3;

        cmd = cmd->next;
    }

    print("\n\n");
}

extern int exec_file(const char* path, const char* args);

// ============================================================
// SHELL ENVIRONMENT VARIABLES ($VAR expansion for scripts)
// ============================================================

#define MAX_ENV_VARS 32
#define ENV_NAME_LEN 32
#define ENV_VALUE_LEN 4096

typedef struct {
    char name[ENV_NAME_LEN];
    char value[ENV_VALUE_LEN];
    int used;
} EnvVar;

static EnvVar env_vars[MAX_ENV_VARS];

// Sets (or creates) a shell variable. Used by `read` to store what the
// user typed, so a later `$NAME` in the script expands to it.
void set_env(const char* name, const char* value) {
    if (!name || !*name) return;

    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && streq(env_vars[i].name, name)) {
            strncpy(env_vars[i].value, value ? value : "", ENV_VALUE_LEN - 1);
            env_vars[i].value[ENV_VALUE_LEN - 1] = 0;
            return;
        }
    }

    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (!env_vars[i].used) {
            env_vars[i].used = 1;
            strncpy(env_vars[i].name, name, ENV_NAME_LEN - 1);
            env_vars[i].name[ENV_NAME_LEN - 1] = 0;
            strncpy(env_vars[i].value, value ? value : "", ENV_VALUE_LEN - 1);
            env_vars[i].value[ENV_VALUE_LEN - 1] = 0;
            return;
        }
    }
    // Out of variable slots - silently drop, same spirit as running out
    // of shell memory on a real system.
}

// Unset variables expand to "" (empty string), same as a POSIX shell.
const char* get_env(const char* name) {
    if (!name || !*name) return "";
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && streq(env_vars[i].name, name))
            return env_vars[i].value;
    }
    return "";
}

// Expands $VAR and ${VAR} references found in `in`, writing the result
// into `out` (which must be at least outsize bytes). A '$' not followed
// by a valid identifier or '{' is copied through literally.
void expand_vars(const char* in, char* out, int outsize) {
    int oi = 0;
    if (!in) { if (outsize > 0) out[0] = 0; return; }

    while (*in && oi < outsize - 1) {
        if (*in == '$' && (isalpha((unsigned char)in[1]) || in[1] == '_')) {
            in++;
            char varname[ENV_NAME_LEN];
            int vi = 0;
            while ((isalnum((unsigned char)*in) || *in == '_') && vi < ENV_NAME_LEN - 1)
                varname[vi++] = *in++;
            varname[vi] = 0;

            const char* val = get_env(varname);
            while (*val && oi < outsize - 1) out[oi++] = *val++;
        } else if (*in == '$' && in[1] == '{') {
            in += 2;
            char varname[ENV_NAME_LEN];
            int vi = 0;
            while (*in && *in != '}' && vi < ENV_NAME_LEN - 1)
                varname[vi++] = *in++;
            varname[vi] = 0;
            if (*in == '}') in++;

            const char* val = get_env(varname);
            while (*val && oi < outsize - 1) out[oi++] = *val++;
        } else {
            out[oi++] = *in++;
        }
    }
    out[oi] = 0;
}

void execute_command(const char* cmd_line) {

    while (*cmd_line == ' ')
        cmd_line++;

    if (!*cmd_line)
        return;

    char cmd_name[128];
    int i = 0;

    while (cmd_line[i] && cmd_line[i] != ' ' && i < 31) {
        cmd_name[i] = cmd_line[i];
        i++;
    }

    cmd_name[i] = 0;

    const char* args = cmd_line + i;

    while (*args == ' ')
        args++;

    // `read NAME` (or `read $NAME`) needs the literal variable name to
    // fill in, so it's the one command that opts out of $VAR expansion.
    static char expanded_args[INPUT_BUFFER_SIZE];
    const char* final_args = args;
    if (!streq(cmd_name, "read")) {
        expand_vars(args, expanded_args, sizeof(expanded_args));
        final_args = expanded_args;
    }

    Command* cmd = cmd_table;

    while (cmd) {

        const char* a = cmd->name;
        const char* b = cmd_name;

        int match = 1;

        while (*a && *b) {
            if (*a != *b) {
                match = 0;
                break;
            }

            a++;
            b++;
        }

        if (match && *a == 0 && *b == 0) {
            cmd->func((char*)final_args);
            return;
        }

        cmd = cmd->next;
    }

    /* Not a built-in.  Commands are standalone ELF32 binaries in
     * /bin (built by the Makefile from bin/*.c and embedded in the
     * kernel image).  Then try /Programs - any file there (ELF32
     * object or shell script) is runnable, which lets the user add
     * or override commands by dropping files into bin/. */
    static const char* dirs[] = { "/bin/", "/Programs/" };
    static const char* exts[] = { "", ".o" };
    char prog_path[320];

    for (int d = 0; d < 2; d++) {
        int pl = 0;
        const char* pd = dirs[d];
        while (pd[pl]) { prog_path[pl] = pd[pl]; pl++; }
        int pi = 0;
        while (cmd_name[pi] && pl < 316) prog_path[pl++] = cmd_name[pi++];
        int base = pl;

        for (int e = 0; e < 2; e++) {
            pl = base;
            const char* pe = exts[e];
            while (*pe && pl < 319) prog_path[pl++] = *pe++;
            prog_path[pl] = 0;

            if (fs_file_exists(prog_path)) {
                exec_file(prog_path, final_args);
                return;
            }
        }
    }

    /* Direct file execution: if the token names an existing file,
     * run it - ELF32 objects through the program driver, text files
     * as line-per-command scripts (exactly what exec_file always
     * did for the old `exec` command). Leading './' is skipped, so
     * './test.o', 'test.o' and 'dir/test.o' all work: you just type
     * the file name. */
    {
        const char* fn = cmd_name;
        while (fn[0] == '.' && fn[1] == '/') fn += 2;
        if (fn[0] && fs_file_exists(fn)) {
            exec_file(fn, final_args);
            return;
        }
    }

    print("error: Command not found: ");
    print(cmd_name);
    print("\n");
}

// ============================================================
// SHELL
// ============================================================

void setup_wizard() {
    print("Starting setup...\n\n");
    print("===== TanjaOS Setup =====\n");
    print("\n");
    print("Create a login: "); read_line(config.username, MAX_USERNAME);
    print("Create a password: "); read_line(config.password, MAX_PASSWORD);
    print("Set a hostname: "); read_line(config.hostname, MAX_HOSTNAME);
    config.is_setup = 1; print("\n");
}

void login_prompt() {
    char u[MAX_USERNAME], p[MAX_PASSWORD];
    while (1) {
        print(config.hostname); print(" login: "); read_line(u, MAX_USERNAME);
        print("Password: "); read_line(p, MAX_PASSWORD);
        if (streq(u, config.username) && streq(p, config.password)) { print("\n"); return; }
        print("Login incorrect\n\n");
    }
}

void cmd_exit(char* args) { (void)args; shell_exit_flag = 1; }
void cmd_hostname(char* args) {
    if (args && args[0]) {
        int i = 0;
        while (i < MAX_HOSTNAME-1 && args[i] && args[i] != ' ') { config.hostname[i] = args[i]; i++; }
        config.hostname[i] = 0;
        extern void store_save(void);
        store_save();
        print("Hostname updated\n");
    } else { print(config.hostname); print("\n"); }
}

void print_prompt_path() {
    char cwd[256];
    fs_get_current_path(cwd);
    /* ~/ is /home/home (the user's home directory). */
    if (streq(cwd, "/home/home")) {
        print("~");
        return;
    }
    int i = 0;
    const char* pre = "/home/home";
    while (pre[i] && cwd[i] == pre[i]) i++;
    if (i == 10 && (cwd[i] == 0 || cwd[i] == '/')) {
        /* e.g. cwd="/home/home/project" -> "~/project" */
        print("~");
        print(cwd + i);
    } else {
        /* outside the home dir the path is shown as-is */
        print(cwd);
    }
}

void shell() {
    shell_exit_flag = 0; static char buf[INPUT_BUFFER_SIZE];
    while (1) {
        print(config.username); print("@"); print(config.hostname); print(":");
        print_prompt_path();
        print("$ ");
        read_line(buf, 4096); clean(buf);
        if (buf[0]) execute_command(buf);
        if (shell_exit_flag) { clear_screen(); break; }
    }
}

void kernel_main(uint32_t mb_magic, uint32_t mb_addr)
{
    fpu_init(); // must run before any FPU instruction executes - JIT'd
                // TanjaOS-C programs can use float/double from their
                // very first instruction, so this has to come first.

    underline_cursor();
    clear_screen();

    timer_init();
    idt_init(); // enables real IRQ0-driven millisecond ticks (see idt.c)

    boot_log("Kernel starting");

    timer_delay_ms(50);

    boot_log("Initializing filesystem");
    store_init(mb_magic, mb_addr);

    timer_delay_ms(50);

    boot_log("Loading command binaries");

    if (!fs_directory_exists("/bin"))
    {
        print("warning: /bin not found - external commands are\n");
        print("unavailable (build the bin commands, see bin/)\n");
    }

    register_cmd("exit", cmd_exit);
    register_cmd("hostname", cmd_hostname);

    if (!config.is_setup) {
        print("\n");
        setup_wizard();
        extern void store_save(void);
        store_save();
    }

    timer_delay_ms(50);
    boot_log("Starting shell");
    print("\n");

    while (1)
    {
        print("The TanjaOS Project\n\n");

        login_prompt();

        /* A login always starts in the user's home directory
         * (/home/home).  The filesystem persists cwd for storage, but
         * cwd is a shell-session state and should not leak from a
         * previous login/reboot. */
        if (fs_change_directory("/home/home") != 0)
            fs_change_directory("/");

        shell();
    }
}
