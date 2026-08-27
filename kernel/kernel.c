#include <stdint.h>
#include <stddef.h>
#include "../include/fs.h"
#include "../include/store.h"
#include "../include/idt.h"

// ============================================================
// VGA CONSTANTS
// ============================================================

#define VGA_COLOR (0x0F << 8)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

uint16_t* VGA = (uint16_t*)0xB8000;
int cursor = 0;

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

void putc_color(char c, uint16_t color) {
    if (c == '\n') {
        cursor = ((cursor / VGA_WIDTH) + 1) * VGA_WIDTH;
    } else if (c == '\b') {
        if (cursor > 0) {
            cursor--;
            VGA[cursor] = color | ' ';
        }
    } else if (c >= ' ') {
        VGA[cursor] = color | (uint8_t)c;
        cursor++;
    }
    if (cursor >= VGA_WIDTH * VGA_HEIGHT) scroll();
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
        if (!(inb(0x64) & 1)) continue;
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
        char res;
        if (caps_lock && !shift) res = keymap_caps[sc];
        else if (shift) res = keymap_shift[sc];
        else res = keymap[sc];
        if (res == 0) continue;
        if (ctrl) {
            if (res == 'x') return 24;
        }
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

#define INPUT_BUFFER_SIZE 4096

void read_line(char* buffer, int max_len) {
    char line[INPUT_BUFFER_SIZE];
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



static void cc_print_uint(unsigned int n, unsigned int base, int upper) {
    char buf[33];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 32;
    buf[i] = 0;
    if (n == 0) { putc('0'); return; }
    while (n && i > 0) { buf[--i] = digits[n % base]; n /= base; }
    while (buf[i]) putc(buf[i++]);
}

static int cc_printf_impl(const char* fmt, int argc, const unsigned int* args) {
    int ai = 0, count = 0;
    if (!fmt) return 0;
    while (*fmt) {
        if (*fmt != '%') { putc(*fmt++); count++; continue; }
        fmt++;
        if (*fmt == '%') { putc('%'); fmt++; count++; continue; }
        if (ai >= argc) { putc('%'); count++; continue; }
        if (*fmt == 'd' || *fmt == 'i') {
            int v = (int)args[ai++];
            if (v < 0) { putc('-'); count++; v = -v; }
            cc_print_uint((unsigned int)v, 10, 0);
            fmt++;
        } else if (*fmt == 'u') {
            cc_print_uint(args[ai++], 10, 0); fmt++;
        } else if (*fmt == 'x' || *fmt == 'X') {
            cc_print_uint(args[ai++], 16, *fmt == 'X'); fmt++;
        } else if (*fmt == 'c') {
            putc((char)args[ai++]); fmt++; count++;
        } else if (*fmt == 's') {
            const char* str = (const char*)args[ai++];
            if (str) while (*str) { putc(*str++); count++; }
            fmt++;
        } else {
            putc('%'); count++;
        }
    }
    return count;
}

int cc_printf_1(const char* fmt) { return cc_printf_impl(fmt, 0, 0); }
int cc_printf_2(unsigned int a1, const char* fmt) { unsigned int a[1]={a1}; return cc_printf_impl(fmt,1,a); }
int cc_printf_3(unsigned int a2, unsigned int a1, const char* fmt) { unsigned int a[2]={a1,a2}; return cc_printf_impl(fmt,2,a); }
int cc_printf_4(unsigned int a3, unsigned int a2, unsigned int a1, const char* fmt) { unsigned int a[3]={a1,a2,a3}; return cc_printf_impl(fmt,3,a); }
int cc_printf_5(unsigned int a4, unsigned int a3, unsigned int a2, unsigned int a1, const char* fmt) { unsigned int a[4]={a1,a2,a3,a4}; return cc_printf_impl(fmt,4,a); }
int cc_printf_6(unsigned int a5, unsigned int a4, unsigned int a3, unsigned int a2, unsigned int a1, const char* fmt) { unsigned int a[5]={a1,a2,a3,a4,a5}; return cc_printf_impl(fmt,5,a); }
int cc_printf_7(unsigned int a6, unsigned int a5, unsigned int a4, unsigned int a3, unsigned int a2, unsigned int a1, const char* fmt) { unsigned int a[6]={a1,a2,a3,a4,a5,a6}; return cc_printf_impl(fmt,6,a); }
int cc_printf_8(unsigned int a7, unsigned int a6, unsigned int a5, unsigned int a4, unsigned int a3, unsigned int a2, unsigned int a1, const char* fmt) { unsigned int a[7]={a1,a2,a3,a4,a5,a6,a7}; return cc_printf_impl(fmt,7,a); }
int cc_printf_9(unsigned int a8, unsigned int a7, unsigned int a6, unsigned int a5, unsigned int a4, unsigned int a3, unsigned int a2, unsigned int a1, const char* fmt) { unsigned int a[8]={a1,a2,a3,a4,a5,a6,a7,a8}; return cc_printf_impl(fmt,8,a); }

static int cc_scanf_one(const char* fmt, unsigned int ptr) {
    char buffer[128];
    int value = 0, sign = 1, i = 0;
    read_line(buffer, sizeof(buffer));
    if (!fmt || !ptr) return 0;
    while (buffer[i] == ' ' || buffer[i] == '\t') i++;
    if (*fmt == '%') fmt++;
    if (*fmt == 'c') { *(char*)ptr = buffer[0]; return 1; }
    if (*fmt == 's') { strcpy((char*)ptr, buffer); return 1; }
    if (buffer[i] == '-') { sign = -1; i++; }
    if (fmt[0] == 'x' || fmt[0] == 'X') {
        unsigned int v=0, d;
        while (buffer[i]) {
            char c=buffer[i++];
            if (c>='0'&&c<='9') d=(unsigned int)(c-'0');
            else if (c>='a'&&c<='f') d=(unsigned int)(c-'a'+10);
            else if (c>='A'&&c<='F') d=(unsigned int)(c-'A'+10);
            else break;
            v=(v<<4)|d;
        }
        *(unsigned int*)ptr=v; return 1;
    }
    while (buffer[i] >= '0' && buffer[i] <= '9') { value=value*10+(buffer[i]-'0'); i++; }
    if (i == 0) return 0;
    *(int*)ptr=value*sign;
    return 1;
}
int cc_scanf_2(unsigned int ptr, const char* fmt) { return cc_scanf_one(fmt, ptr); }
/*
 * The tiny C compiler uses a historical left-to-right argument push
 * convention.  These private bridges reverse arguments before calling the
 * real ISO C functions, so programs still see normal C argument order.
 */
int cc_strcmp_bridge(const char* b, const char* a) { return strcmp(a, b); }
int cc_strncmp_bridge(unsigned int n, const char* b, const char* a) { return strncmp(a, b, n); }
char* cc_strcpy_bridge(const char* src, char* dst) { return strcpy(dst, src); }
char* cc_strncpy_bridge(unsigned int n, const char* src, char* dst) { return strncpy(dst, src, n); }
char* cc_strcat_bridge(const char* src, char* dst) { return strcat(dst, src); }
char* cc_strchr_bridge(int c, const char* s) { return strchr(s, c); }
char* cc_strrchr_bridge(int c, const char* s) { return strrchr(s, c); }
char* cc_strstr_bridge(const char* needle, const char* haystack) { return strstr(haystack, needle); }
void* cc_memset_bridge(unsigned int n, int value, void* dst) { return memset(dst, value, n); }
void* cc_memcpy_bridge(unsigned int n, const void* src, void* dst) { return memcpy(dst, src, n); }
void* cc_memmove_bridge(unsigned int n, const void* src, void* dst) { return memmove(dst, src, n); }
int cc_memcmp_bridge(unsigned int n, const void* b, const void* a) { return memcmp(a, b, n); }


// ============================================================
// COMMAND SYSTEM
// ============================================================

void register_cmd(const char* name, void (*func)(char* args)) {

    if (cmd_pool_index >= CMD_POOL_SIZE)
        return;

    Command* cmd = &cmd_pool[cmd_pool_index++];

    int i = 0;
    while (name[i] && i < 31) {
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

extern int exec_file(const char* path);

void cmd_exit(char* args);

/* Interactive-only C-style command calls.
 * Examples: cmd_mkdir("games");  cmd_ls("");  clear_screen();
 * These are deliberately NOT used by exec_file(), so shell scripts keep
 * their normal line-oriented shell syntax. */
static int execute_c_style_command(const char* line) {
    int len = 0;
    while (line[len]) len++;
    if (len < 4 || line[len - 1] != ';') return 0;

    int p = 0;
    while (line[p] == ' ' || line[p] == '\t') p++;
    char name[40];
    int n = 0;
    while (line[p] && line[p] != '(' && n < 39) name[n++] = line[p++];
    name[n] = 0;
    if (!line[p] || line[len - 1] != ';') return 0;
    if (p == 0 || line[p] != '(') return 0;

    /* Only accept identifiers and C-style calls; ordinary shell commands
     * still go through the normal command table. */
    for (int i = 0; name[i]; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            return 0;

    p++;
    while (line[p] == ' ' || line[p] == '\t') p++;

    char args[256];
    int a = 0;
    if (line[p] == ')') {
        p++;
    } else if (line[p] == '"') {
        p++;
        while (line[p] && line[p] != '"' && a < 255) {
            if (line[p] == '\\' && line[p+1]) {
                p++;
                if (line[p] == 'n') args[a++] = '\n';
                else if (line[p] == 't') args[a++] = '\t';
                else args[a++] = line[p];
                p++;
            } else args[a++] = line[p++];
        }
        if (line[p] != '"') return 0;
        p++;
        while (line[p] == ' ' || line[p] == '\t') p++;
        if (line[p] != ')') return 0;
        p++;
    } else {
        return 0;
    }

    while (line[p] == ' ' || line[p] == '\t') p++;
    if (line[p] != ';' || line[p+1] != 0) return 0;
    args[a] = 0;

    if (streq(name, "clear_screen")) { clear_screen(); return 1; }
    if (streq(name, "print")) { print(args); return 1; }
    if (streq(name, "puts")) { puts(args); return 1; }
    if (streq(name, "cmd_exit")) { cmd_exit(args); return 1; }
    if (name[0]=='c' && name[1]=='m' && name[2]=='d' && name[3]=='_') {
        Command* cmd = cmd_table;
        while (cmd) {
            if (streq(cmd->name, name + 4)) { cmd->func(args); return 1; }
            cmd = cmd->next;
        }
    }
    return 0;
}

void execute_command(const char* cmd_line) {

    while (*cmd_line == ' ')
        cmd_line++;

    if (!*cmd_line)
        return;

    char cmd_name[32];
    int i = 0;

    while (cmd_line[i] && cmd_line[i] != ' ' && i < 31) {
        cmd_name[i] = cmd_line[i];
        i++;
    }

    cmd_name[i] = 0;

    const char* args = cmd_line + i;

    while (*args == ' ')
        args++;

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
            cmd->func((char*)args);
            return;
        }

        cmd = cmd->next;
    }

    /* Like a normal shell, fall back to executing a file when the command
     * name is not a built-in. Compiled TanjaOS binaries and text scripts
     * both go through exec_file(). */
    if (fs_file_exists(cmd_name)) {
        exec_file(cmd_name);
        return;
    }

    print("error: Command not found: ");
    print(cmd_name);
    print("\n");
}

// ============================================================
// SHELL
// ============================================================

void setup_wizard() {
    print("===== TanjaOS Setup =====\n");
    print("\n");
    print("Create a login: "); read_line(config.username, MAX_USERNAME);
    print("Create a password: "); read_line(config.password, MAX_PASSWORD);
    print("Set a hostname: "); read_line(config.hostname, MAX_HOSTNAME);
    config.is_setup = 1; clear_screen();
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
    if (cwd[0] == '/' && cwd[1] == 0) {
        // TanjaOS starts each login at the filesystem root.
        print("~");
    } else {
        // e.g. cwd="/folder/project" -> "~/folder/project"
        print("~");
        print(cwd);
    }
}

static void execute_interactive_command(const char* cmd_line) {
    if (execute_c_style_command(cmd_line)) return;
    execute_command(cmd_line);
}

void shell() {
    shell_exit_flag = 0; char buf[4096];
    while (1) {
        print(config.username); print("@"); print(config.hostname); print(":");
        print_prompt_path();
        print("$ ");
        read_line(buf, 4096); clean(buf);
        if (buf[0]) execute_interactive_command(buf);
        if (shell_exit_flag) { clear_screen(); break; }
    }
}

extern void init_cmds(void);

void kernel_main(uint32_t mb_magic, uint32_t mb_addr)
{
    underline_cursor();
    clear_screen();

    timer_init();
    idt_init(); // enables real IRQ0-driven millisecond ticks (see idt.c)

    boot_log("Kernel starting");

    timer_delay_ms(50);

    boot_log("Initializing filesystem");
    store_init(mb_magic, mb_addr);

    timer_delay_ms(50);

    boot_log("Loading commands");
    init_cmds();

    timer_delay_ms(50);

    boot_log("Checking command table");

    if (cmd_count == 0)
    {
        print("panic: due to: Unable to load commands\n");
        print("command count=0\n");
        print("Please provide commands in bin\n");
        print("panic: due to: Unable to load commands\n");

        while (1);
    }

    register_cmd("exit", cmd_exit);
    register_cmd("hostname", cmd_hostname);

    timer_delay_ms(50);

    boot_log("Starting setup");

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

        /* A login always starts at the filesystem root.  The filesystem
         * persists cwd for storage, but cwd is a shell-session state and
         * should not leak from a previous login/reboot. */
        fs_change_directory("/");

        shell();
    }
}
