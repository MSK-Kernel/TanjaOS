#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../include/fs.h"
#include "../include/store.h"
#include "../include/idt.h"
#include "../include/multiboot.h"
#include "../include/utf8.h"

// VGA CONSTANTS

#define VGA_COLOR (0x0F << 8)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

uint16_t* VGA = (uint16_t*)0xB8000;
int cursor = 0;

// FPU INIT

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
    /* SSE/SSE2 for Linux binaries (glibc's IFUNC-optimized string and
 * math functions use them): without CR4.OSFXSR every SSE insn
 * raises #UD. OSXMMEXCPT lets #XM (SIMD FP exceptions) reach the
 * handler instead of aborting. */
    {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9) | (1u << 10);   /* OSFXSR | OSXMMEXCPT */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
        __asm__ volatile("emms");
        {
            /* default MXCSR: all exception masks on */
            static uint32_t mxcsr_init = 0x1F80;
            __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr_init));
        }
    }
}

// GLOBAL VARIABLES

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

// USER CONFIGURATION

#define MAX_USERNAME 32
#define MAX_PASSWORD 32
#define MAX_HOSTNAME 64

typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    int uid;
} user_t;

#define MAX_USERS 8

/* Supplementary groups (like /etc/group): named groups with username
 * members. A "wheel" group with members gates sudo, like real Unix:
 * while wheel is absent or empty every user may sudo (the historical
 * TanjaOS behavior); once wheel has members, only they may. */
#define MAX_GROUPS 8
#define MAX_GMEM   16

typedef struct {
    char name[MAX_USERNAME];
    int count;
    char members[MAX_GMEM][MAX_USERNAME];
} group_t;

static group_t groups[MAX_GROUPS];
static int groups_count = 0;

/* hostname + setup-done flag (the login itself lives in users[]) */
typedef struct {
    char hostname[MAX_HOSTNAME];
    int is_setup;
} user_config_t;

/* The account table. users[0] is ALWAYS root (uid 0, home /root):
 * root exists from the moment the system boots - only its password
 * is set during setup. users[1..] are the created accounts. */
/* Statically initialized so root exists even on a store-less boot
 * (config_deserialize/config_reset rebuild the table too, for
 * persistent and factory-reset boots respectively). */
static user_t users[MAX_USERS] = { { "root", {0}, 0 } };
static int users_count = 1;      /* including root */
static int next_uid = 1000;

user_config_t config = { .is_setup = 0 };

int shell_exit_flag = 0;

/* Account management
 * The account table lives in the store blob next to the filesystem
 * so it survives reboots on persistent installs. Blob format v2:
 * [0] 2 (format version; anything else = legacy)
 * [1..64] hostname (64)
 * [65] is_setup
 * [66..97] root password (32)
 * [98] users_count (incl. root)
 * then per user: username(32) + password(32) + uid(4)
 * A legacy (v1) blob carries the old single-user login; it is
 * migrated on load: that user keeps their password as uid 1000
 * and root's password starts empty, so the next boot asks for
 * one before the shell starts. */
#define CFG_VERSION 2

int streq(const char* a, const char* b);
char* strncpy(char* dst, const char* src, unsigned int n);
void store_save(void);

uint32_t config_store_size(void) {
    return 99 + (uint32_t)(MAX_USERS * (MAX_USERNAME + MAX_PASSWORD + 4))
         /* groups: count + per group (name, member count, members) */
         + 1 + (uint32_t)(MAX_GROUPS * (MAX_USERNAME + 1 + MAX_GMEM * MAX_USERNAME));
}

static int group_find(const char* name);
static int group_member_add(int gi, const char* name);
static void group_member_remove(int gi, const char* name);
static int group_has_member(int gi, const char* name);
int user_find(const char* name);

int config_serialize(uint8_t* buf, uint32_t buf_size) {
    if (!buf || buf_size < config_store_size()) return -1;
    uint32_t p = 0;
    int i, j;
    buf[p++] = CFG_VERSION;
    for (i = 0; i < MAX_HOSTNAME; i++) buf[p++] = (uint8_t)config.hostname[i];
    buf[p++] = (uint8_t)(config.is_setup ? 1 : 0);
    for (i = 0; i < MAX_PASSWORD; i++) buf[p++] = (uint8_t)users[0].password[i];
    buf[p++] = (uint8_t)users_count;
    for (i = 0; i < users_count && i < MAX_USERS; i++) {
        for (j = 0; j < MAX_USERNAME; j++) buf[p++] = (uint8_t)users[i].username[j];
        for (j = 0; j < MAX_PASSWORD; j++) buf[p++] = (uint8_t)users[i].password[j];
        buf[p++] = (uint8_t)(users[i].uid & 0xFF);
        buf[p++] = (uint8_t)((users[i].uid >> 8) & 0xFF);
        buf[p++] = (uint8_t)((users[i].uid >> 16) & 0xFF);
        buf[p++] = (uint8_t)((users[i].uid >> 24) & 0xFF);
    }
    buf[p++] = (uint8_t)groups_count;
    for (i = 0; i < groups_count && i < MAX_GROUPS; i++) {
        int k;
        for (j = 0; j < MAX_USERNAME; j++) buf[p++] = (uint8_t)groups[i].name[j];
        buf[p++] = (uint8_t)groups[i].count;
        for (j = 0; j < groups[i].count && j < MAX_GMEM; j++) {
            for (k = 0; k < MAX_USERNAME; k++)
                buf[p++] = (uint8_t)groups[i].members[j][k];
        }
    }
    return 0;
}

/* internal: blank the table, root first */
static void users_reset_table(void) {
    int i, j;
    groups_count = 0;
    for (i = 0; i < MAX_GROUPS; i++) {
        int t;
        groups[i].name[0] = 0;
        groups[i].count = 0;
        for (t = 0; t < MAX_USERNAME; t++) groups[i].members[0][t] = 0;
    }
    for (i = 0; i < MAX_USERS; i++) {
        for (j = 0; j < MAX_USERNAME; j++) users[i].username[j] = 0;
        for (j = 0; j < MAX_PASSWORD; j++) users[i].password[j] = 0;
        users[i].uid = 0;
    }
    strncpy(users[0].username, "root", MAX_USERNAME);
    users[0].uid = 0;
    users_count = 1;
    next_uid = 1000;
}

int config_deserialize(const uint8_t* buf, uint32_t buf_size) {
    if (!buf || buf_size < 2) return -1;
    int i, j;
    users_reset_table();
    if (buf[0] == CFG_VERSION) {
        if (buf_size < config_store_size()) return -1;
        uint32_t p = 1;
        for (i = 0; i < MAX_HOSTNAME; i++) config.hostname[i] = (char)buf[p++];
        config.is_setup = buf[p++] ? 1 : 0;
        for (i = 0; i < MAX_PASSWORD; i++) users[0].password[i] = (char)buf[p++];
        int cnt = buf[p++];
        if (cnt < 1 || cnt > MAX_USERS) cnt = 1;
        users_count = 1;   /* root was just loaded above */
        /* The serializer writes ALL users as records, root included:
 * record 0 IS root (whose password was already read above), so
 * skip over it before loading the regular users. The old code
 * started reading at record 0, re-creating root as a phantom
 * "root" account and dropping every real user. */
        p += MAX_USERNAME + MAX_PASSWORD + 4;
        for (i = 1; i < cnt; i++) {
            int ui = users_count;
            for (j = 0; j < MAX_USERNAME; j++) users[ui].username[j] = (char)buf[p++];
            for (j = 0; j < MAX_PASSWORD; j++) users[ui].password[j] = (char)buf[p++];
            users[ui].uid = (int)((uint32_t)buf[p]
                    | ((uint32_t)buf[p + 1] << 8)
                    | ((uint32_t)buf[p + 2] << 16)
                    | ((uint32_t)buf[p + 3] << 24));
            p += 4;
            if (users[ui].uid <= 0) users[ui].uid = 1000 + i;
            if (users[ui].uid >= next_uid) next_uid = users[ui].uid + 1;
            /* stores written by the buggy loader may carry a phantom
 * duplicate "root"; real users can never be named root, so
 * drop those records instead of resurrecting them */
            if (users[ui].username[0] && !streq(users[ui].username, "root"))
                users_count++;
        }
        /* groups follow the user records */
        {
            int gc = buf[p++];
            int gi;
            if (gc < 0 || gc > MAX_GROUPS) gc = 0;
            for (gi = 0; gi < gc; gi++) {
                char gname[MAX_USERNAME];
                int mj;
                for (mj = 0; mj < MAX_USERNAME; mj++) gname[mj] = (char)buf[p++];
                int mc = buf[p++];
                if (mc < 0 || mc > MAX_GMEM) mc = 0;
                if (gname[0] && groups_count < MAX_GROUPS
                    && group_find(gname) < 0) {
                    int dst = groups_count++;
                    for (mj = 0; mj < MAX_USERNAME; mj++)
                        groups[dst].name[mj] = gname[mj];
                    groups[dst].count = 0;
                    for (mj = 0; mj < mc; mj++) {
                        char mname[MAX_USERNAME];
                        int t;
                        for (t = 0; t < MAX_USERNAME; t++) mname[t] = (char)buf[p++];
                        /* drop members that no longer exist */
                        if (user_find(mname) >= 0)
                            group_member_add(dst, mname);
                    }
                } else {
                    /* duplicate or unusable group: skip its members */
                    p += mc * MAX_USERNAME;
                }
            }
        }
    } else {
        /* legacy v1 blob: username(32) password(32) hostname(64) is_setup */
        if (buf_size < MAX_USERNAME + MAX_PASSWORD + MAX_HOSTNAME + 1) return -1;
        uint32_t p = 0;
        char name[MAX_USERNAME], pass[MAX_PASSWORD];
        for (i = 0; i < MAX_USERNAME; i++) name[i] = (char)buf[p++];
        for (i = 0; i < MAX_PASSWORD; i++) pass[i] = (char)buf[p++];
        for (i = 0; i < MAX_HOSTNAME; i++) config.hostname[i] = (char)buf[p++];
        config.is_setup = buf[p++] ? 1 : 0;
        /* migrate the old login as a regular user, uid 1000 */
        if (name[0] && !streq(name, "root")) {
            strncpy(users[1].username, name, MAX_USERNAME);
            strncpy(users[1].password, pass, MAX_PASSWORD);
            users[1].uid = 1000;
            users_count = 2;
            next_uid = 1001;
        }
    }
    return 0;
}

// Reset the account configuration to the same state as a fresh install.
// This deliberately clears all fields and marks setup as incomplete.
void config_reset(void) {
    int i;
    for (i = 0; i < MAX_HOSTNAME; i++) config.hostname[i] = 0;
    config.is_setup = 0;
    users_reset_table();
}

/* account helpers used by the builtins and login */
int user_find(const char* name) {
    int i;
    for (i = 0; i < users_count; i++)
        if (users[i].username[0] && streq(users[i].username, name)) return i;
    return -1;
}

int user_add_full(const char* name, const char* password, int uid);

int user_add(const char* name, const char* password) {
    return user_add_full(name, password, 0);
}

int user_add_full(const char* name, const char* password, int uid) {
    int i, j;
    if (!name || !name[0] || !password) return -1;
    if (user_find(name) >= 0) return -1;
    if (users_count >= MAX_USERS) return -2;
    int idx = users_count;
    for (i = 0; i < MAX_USERNAME; i++) users[idx].username[i] = 0;
    for (i = 0; i < MAX_PASSWORD; i++) users[idx].password[i] = 0;
    for (i = 0; name[i] && i < MAX_USERNAME - 1; i++) users[idx].username[i] = name[i];
    for (i = 0; password[i] && i < MAX_PASSWORD - 1; i++) users[idx].password[i] = password[i];
    users[idx].uid = (uid > 0) ? uid : next_uid++;
    if (users[idx].uid >= next_uid) next_uid = users[idx].uid + 1;
    users_count++;
    /* home directory: /home/<name>, private to the user */
    {
        char home[80];
        int len = 0, k;
        const char* pre = "/home/";
        for (k = 0; pre[k] && len < 70; k++) home[len++] = pre[k];
        for (k = 0; name[k] && len < 78; k++) home[len++] = name[k];
        home[len] = 0;
        fs_create_home_tree(home, users[idx].uid);
    }
    store_save();
    return 0;
}

int user_del(const char* name) {
    int idx = user_find(name);
    int i, j;
    if (idx <= 0) return -1;      /* 0 = root: refuse */
    for (i = idx; i < users_count - 1; i++) {
        for (j = 0; j < MAX_USERNAME; j++) users[i].username[j] = users[i + 1].username[j];
        for (j = 0; j < MAX_PASSWORD; j++) users[i].password[j] = users[i + 1].password[j];
        users[i].uid = users[i + 1].uid;
    }
    users_count--;
    for (j = 0; j < MAX_USERNAME; j++) users[users_count].username[j] = 0;
    for (j = 0; j < MAX_PASSWORD; j++) users[users_count].password[j] = 0;
    users[users_count].uid = 0;
    /* drop the deleted account from every group */
    {
        int g;
        for (g = 0; g < groups_count; g++)
            group_member_remove(g, name);
    }
    store_save();
    return 0;
}

/* group helpers */

static int group_find(const char* name) {
    int i;
    for (i = 0; i < groups_count; i++)
        if (groups[i].name[0] && streq(groups[i].name, name)) return i;
    return -1;
}

static int group_has_member(int gi, const char* name) {
    int j;
    for (j = 0; j < groups[gi].count; j++)
        if (streq(groups[gi].members[j], name)) return 1;
    return 0;
}

/* add user to group; -1 = group full */
static int group_member_add(int gi, const char* name) {
    if (group_has_member(gi, name)) return 0;
    if (groups[gi].count >= MAX_GMEM) return -1;
    int j = 0;
    for (j = 0; j < MAX_USERNAME - 1; j++) groups[gi].members[groups[gi].count][j] = 0;
    for (j = 0; name[j] && j < MAX_USERNAME - 1; j++)
        groups[gi].members[groups[gi].count][j] = name[j];
    groups[gi].count++;
    return 0;
}

static void group_member_remove(int gi, const char* name) {
    int j, k;
    for (j = 0; j < groups[gi].count; j++) {
        if (streq(groups[gi].members[j], name)) {
            for (k = j; k < groups[gi].count - 1; k++)
                for (int t = 0; t < MAX_USERNAME; t++)
                    groups[gi].members[k][t] = groups[gi].members[k + 1][t];
            for (int t = 0; t < MAX_USERNAME; t++)
                groups[gi].members[groups[gi].count - 1][t] = 0;
            groups[gi].count--;
            return;
        }
    }
}

/* current logged-in user (for the prompt and cd's ~) */
static char cur_username[MAX_USERNAME] = "root";
static int cur_uid = 0;
static char cur_home[96] = "/root";

void tanja_get_home(char* out, int cap) {
    int i;
    for (i = 0; i < cap - 1 && cur_home[i]; i++) out[i] = cur_home[i];
    out[i] = 0;
}
const char* tanja_current_user(void) { return cur_username; }
int tanja_current_uid(void) { return cur_uid; }

// PORT I/O

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

// CURSOR

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

/* Ctrl+C support: while a program runs, the timer tick sniffs the
 * keyboard (programs that never read input would otherwise be
 * unkillable). Everything typed is preserved through a small ring
 * so get_key still delivers it. Declared here, defined with the
 * keyboard code below. */
static int ctrlc_poll(void);
void print(const char* s);
extern int tanja_exit_jmp[5];
extern int tanja_exit_active;
int kb_raw_mode;

void timer_handler()
{
    boot_ticks++;

    outb(0x20, 0x20); // send EOI

    if (tanja_exit_active && !kb_raw_mode && ctrlc_poll()) {
        /* unwinds exactly like exit() / a program fault: back into
 * elf_run, which restores the program break, then the shell
 * prompt continues.  Terminal output matches bash: just ^C. */
        print("^C\n");
        asm volatile ("sti");
        __builtin_longjmp((void**)tanja_exit_jmp, 1);
    }
}

void underline_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0D);

    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
}

// SCREEN FUNCTIONS

void scroll() {
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        VGA[i] = VGA[i + VGA_WIDTH];
    for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';
    cursor = VGA_WIDTH * (VGA_HEIGHT - 1);
}

/* Draw a single already-decoded cell: control codes (newline,
 * backspace, tab) plus one printable character, then scroll if
 * needed. This is the raw drawing primitive both the byte
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
 * between console_putc(), print() and print_n() (e.g. cat piped output).
 *
 * Why: the VGA console only has CP437 glyphs, so printing UTF-8
 * one raw byte at a time showed punctuation like “ ” — as two or
 * three jumbled characters. Now a full sequence is decoded and
 * drawn as a single ASCII cell: quotes become ", dashes become -,
 * ʼ becomes ', and anything unmappable becomes '?'. Bytes that
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

void console_putc(char c) {
    putc_color(c, VGA_COLOR);
}

/* ISO C-style console functions exposed to TanjaOS C programs. */
int get_key(void);
int putchar(int c) { console_putc((char)c); return c; }
int getchar(void) { return get_key(); }
int puts(const char* s) { if (s) { while (*s) console_putc(*s++); } console_putc('\n'); return 0; }

void print(const char* s) {
    if (!s) return;
    while (*s) console_putc(*s++);
}

/* Fast bulk console output. Unlike print()/console_putc(), this only updates the
 * hardware cursor once for the whole chunk. Commands such as cat can use
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

/* printf/scanf and the rest of the C library now live in kernel/libc.c */

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

// NUMBER PRINTING

void print_hex(uint32_t n) {
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[11];
    buffer[0] = '0'; buffer[1] = 'x'; buffer[10] = 0;
    for (int i = 9; i >= 2; i--) { buffer[i] = hex_chars[n & 0xF]; n >>= 4; }
    print(buffer);
}

void print_dec(uint32_t n) {
    if (n == 0) { console_putc('0'); return; }
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
        console_putc('0');
    if (ms < 10)
        console_putc('0');

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
    for (int i = 0; i < width - len; i++) console_putc(' ');
    print(&buffer[pos]);
}

// KEYBOARD

int shift = 0;
int ctrl = 0;

/* Programs that need the RAW PS/2 stream (Doom polls 0x60/0x64
 * directly for key-up events and mouse packets) set this; while it is
 * on, the timer's Ctrl+C sniffing leaves the controller alone so the
 * program sees every byte. elf_run clears it when the program ends. */
/* defined above, next to the timer handler */

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

/* Keyboard ring + Ctrl+C sniff (timer context)
 * The timer tick consumes waiting scancodes while a program runs
 * (see ctrlc_poll); they land here and get_key drains this ring
 * BEFORE touching the ports, so nothing typed is lost. */
#define KB_RING 64
static uint8_t kb_ring[KB_RING];
static volatile int kb_head = 0, kb_tail = 0;

static int kb_ring_count(void) {
    return (kb_head - kb_tail + KB_RING) % KB_RING;
}
static void kb_ring_push(uint8_t sc) {
    if (kb_ring_count() < KB_RING) {   /* full: drop newest, beats
 * corrupting old input */
        kb_ring[kb_head] = sc;
        kb_head = (kb_head + 1) % KB_RING;
    }
}
static int kb_ring_pop(uint8_t* out) {
    if (!kb_ring_count()) return 0;
    *out = kb_ring[kb_tail];
    kb_tail = (kb_tail + 1) % KB_RING;
    return 1;
}

/* Consume waiting scancodes, keeping the shift/ctrl/caps state that
 * get_key also maintains exactly in sync, and buffering everything
 * else for get_key. Returns 1 when Ctrl+C is seen. */
static int ctrlc_poll(void) {
    int got_cc = 0;
    while (inb(0x64) & 1) {
        uint8_t sc = inb(0x60);
        switch (sc) {
        case 0x2A: case 0x36: shift = 1; break;
        case 0xAA: case 0xB6: shift = 0; break;
        case 0x1D: ctrl = 1; break;
        case 0x9D: ctrl = 0; break;
        case 0x3A: caps_lock = !caps_lock; break;
        case 0x2E:                    /* C */
            if (ctrl) got_cc = 1;
            else kb_ring_push(sc);
            break;
        default: kb_ring_push(sc); break;   /* incl. 0xE0 prefixes */
        }
    }
    return got_cc;
}

/* next raw scancode: ring first, then the ports */
static uint8_t next_scancode(void) {
    while (1) {
        uint8_t sc;
        if (kb_ring_pop(&sc)) return sc;
        if (!(inb(0x64) & 1)) {
            asm volatile ("hlt");
            continue;
        }
        return inb(0x60);
    }
}

int get_key() {
    while (1) {
        uint8_t sc = next_scancode();
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0x1D) { ctrl = 1; continue; }
        if (sc == 0x9D) { ctrl = 0; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc == 0x3A) { caps_lock = !caps_lock; continue; }
        if (sc == 0xE0) {
            uint8_t ext = next_scancode();
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

// STRING HELPERS

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

// INPUT

#define INPUT_BUFFER_SIZE 65536

static char input_line[INPUT_BUFFER_SIZE];

int read_line_eof = 0;

void read_line(char* buffer, int max_len) {
    char* line = input_line;
    int pos = 0;
    int len = 0;
    int prompt_start = cursor;
    int eof = 0;

    read_line_eof = 0;

    while (1) {

        int key = get_key();

        if (key == '\n' || key == KEY_ENTER) {
            console_putc('\n');
            break;
        }

        if (key == 4) { /* Ctrl-D: EOT - end of input, like a real tty */
            eof = 1;
            read_line_eof = 1;
            break;
        }

        if (key == 3) { /* Ctrl+C: cancel the line, like a real tty */
            line[0] = 0;
            len = 0;
            console_putc('^');
            console_putc('C');
            console_putc('\n');
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

    /* Ctrl-D marks end-of-input with a trailing '\x04' (stripped by
 * callers that care; grep uses it to stop reading stdin). */
    if (eof && copy_len < max_len - 1) {
        buffer[copy_len] = 4;
        buffer[copy_len + 1] = 0;
    }
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

// COMMAND SYSTEM

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

// SHELL ENVIRONMENT VARIABLES

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

// SHELL WILDCARD (GLOB) EXPANSION
//
// `rm *.o`, `ls *.c`, `cat /docs/*` ... Every whitespace-separated
// argument containing a `*` is expanded at the shell level into the
// matching entries of the directory it names, alphabetically sorted
// like a POSIX shell. The command never sees the `*`; it receives
// plain file names as separate arguments.

#define GLOB_MAX_MATCHES 128   /* MAX_D + MAX_F in fs.c is 64 + 64 */
#define GLOB_NAME_LEN   256    /* matches MAX_FILENAME in fs.h */
#define GLOB_LIST_CAP   16384  /* fs_list_directory fills at most this */

static char glob_list_buf[GLOB_LIST_CAP];
static char glob_names[GLOB_MAX_MATCHES][GLOB_NAME_LEN];

/* Does `name` match `pat`? A `*` in the pattern stands for any run
 * of zero or more characters. Classic backtracking matcher: try the
 * longest run for each star, shrink it on mismatch. */
static int glob_match(const char* pat, const char* name) {
    const char *p = pat, *n = name;
    const char *star = 0, *resume = 0;
    while (*n) {
        if (*p == '*') { star = p++; resume = n; }
        else if (*p == *n) { p++; n++; }
        else if (star) { p = star + 1; n = ++resume; }
        else return 0;
    }
    while (*p == '*') p++;
    return *p == 0;
}

static int glob_name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        a++; b++;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;
}

/* Expands `*` wildcards in `in`, writing the result into `out`.
 *
 * A pattern may carry a directory part ("dir/*.c", "/home/*.o"); the
 * base pattern is matched against that directory's entries. A pattern
 * that matches nothing is passed through unchanged so the command
 * reports "No such file or directory" itself, like bash. Dot-prefixed
 * names only match patterns that start with a dot, and tokens holding
 * quotes or backslashes are never expanded (the user suppressed the
 * wildcard). */
void expand_globs(const char* in, char* out, int outsize) {
    int oi = 0;
    if (!in) { if (outsize > 0) out[0] = 0; return; }
    if (outsize <= 0) return;

    const char* p = in;
    int first = 1;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char* tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int tlen = (int)(p - tok);

        if (!first && oi < outsize - 1) out[oi++] = ' ';
        first = 0;

        int has_star = 0, quoted = 0;
        for (int i = 0; i < tlen; i++) {
            if (tok[i] == '*') has_star = 1;
            if (tok[i] == '\'' || tok[i] == '"' || tok[i] == '\\') quoted = 1;
        }

        int expanded = 0;
        if (has_star && !quoted) {
            /* split at the last '/', so "dir/*.c" lists "dir" and
 * matches "*.c"; a bare "*.c" lists the cwd. */
            int slash = -1;
            for (int i = 0; i < tlen; i++) if (tok[i] == '/') slash = i;
            int dlen = (slash < 0) ? 0 : slash + 1;
            int plen = tlen - dlen;

            if (dlen < 256 && plen < 256 && plen > 0) {
                char dirpart[256], pat[256];
                for (int i = 0; i < dlen; i++) dirpart[i] = tok[i];
                dirpart[dlen] = 0;
                for (int i = 0; i < plen; i++) pat[i] = tok[dlen + i];
                pat[plen] = 0;

                uint32_t lsize = 0;
                if (fs_list_directory(dlen ? dirpart : 0,
                                      glob_list_buf, &lsize) == 0) {
                    int count = 0;
                    char* q = glob_list_buf;
                    while (*q && count < GLOB_MAX_MATCHES) {
                        char* nl = q;
                        while (*nl && *nl != '\n') nl++;
                        int nlen = (int)(nl - q);

                        /* directories come back with a trailing '/' */
                        while (nlen > 0 && q[nlen - 1] == '/') nlen--;

                        if (nlen > 0 && nlen < GLOB_NAME_LEN) {
                            char nm[GLOB_NAME_LEN];
                            for (int i = 0; i < nlen; i++) nm[i] = q[i];
                            nm[nlen] = 0;

                            /* hidden names need a dot-prefixed pattern */
                            if (!(nm[0] == '.' && pat[0] != '.') &&
                                glob_match(pat, nm)) {
                                for (int i = 0; i <= nlen; i++)
                                    glob_names[count][i] = nm[i];
                                count++;
                            }
                        }
                        q = nl + (*nl ? 1 : 0);
                    }

                    if (count > 0) {
                        /* alphabetical, like a POSIX shell */
                        for (int a = 1; a < count; a++) {
                            char tmp[GLOB_NAME_LEN];
                            for (int k = 0; k < GLOB_NAME_LEN; k++)
                                tmp[k] = glob_names[a][k];
                            int b = a - 1;
                            while (b >= 0 &&
                                   glob_name_cmp(glob_names[b], tmp) > 0) {
                                for (int k = 0; k < GLOB_NAME_LEN; k++)
                                    glob_names[b + 1][k] = glob_names[b][k];
                                b--;
                            }
                            for (int k = 0; k < GLOB_NAME_LEN; k++)
                                glob_names[b + 1][k] = tmp[k];
                        }

                        int dlen2 = 0;
                        while (dirpart[dlen2]) dlen2++;

                        expanded = 1;
                        for (int a = 0; a < count; a++) {
                            int nlen2 = 0;
                            while (glob_names[a][nlen2]) nlen2++;
                            if (oi + dlen2 + nlen2 >= outsize - 1) break;
                            for (int k = 0; k < dlen2; k++)
                                out[oi++] = dirpart[k];
                            for (int k = 0; k < nlen2; k++)
                                out[oi++] = glob_names[a][k];
                            if (a + 1 < count && oi < outsize - 1)
                                out[oi++] = ' ';
                        }
                    }
                }
            }
        }

        if (!expanded) {
            /* no star, quoted, or nothing matched: pass it through */
            if (oi + tlen < outsize - 1) {
                for (int i = 0; i < tlen; i++) out[oi++] = tok[i];
            } else {
                break;
            }
        }
    }

    out[oi] = 0;
}

/* Linux-style execute gate: running a file requires an x bit for the
 * current user. Root follows the real Linux rule: root may execute
 * anything that has at least one x bit, but not a file with none.
 * Returns 1 (allowed) or 0 after printing the denial. */
static int exec_perm_ok(const char* path, const char* display) {
    int mode = fs_get_mode(path);
    if (mode < 0) return 1;               /* not a plain file: let exec_file report */
    int ok;
    if ((mode & 0111) == 0) ok = 0;       /* no x bit at all: nobody may run it */
    else if (fs_cur_uid == 0) ok = 1;
    else if (fs_cur_uid == fs_get_owner(path)) ok = (mode & 0100) != 0;
    else ok = (mode & 0001) != 0;         /* other (group bits unenforced) */
    if (!ok) {
        print(display);
        print(": Permission denied\n");
    }
    return ok;
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
    static char globbed_args[INPUT_BUFFER_SIZE];
    const char* final_args = args;
    if (!streq(cmd_name, "read")) {
        expand_vars(args, expanded_args, sizeof(expanded_args));
        expand_globs(expanded_args, globbed_args, sizeof(globbed_args));
        final_args = globbed_args;
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

    /* Not a built-in. Commands are standalone ELF32 binaries in
 * /bin (built by the Makefile from bin/*.c and embedded in the
 * kernel image). Then try /Programs - any file there (ELF32
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
                if (exec_perm_ok(prog_path, cmd_name)) {
                    exec_file(prog_path, final_args);
                }
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
            if (exec_perm_ok(fn, cmd_name)) {
                exec_file(fn, final_args);
            }
            return;
        }
    }

    print("error: Command not found: ");
    print(cmd_name);
    print("\n");
}

// SHELL

void setup_wizard() {
    print("Starting setup...\n\n");
    print("===== TanjaOS Setup =====\n");
    print("\n");

    /* root exists from boot; only its password is chosen here. This
 * runs BEFORE the account step, and also on upgraded installs
 * where the legacy config had no root password yet. */
    while (1) {
        char pw1[MAX_PASSWORD], pw2[MAX_PASSWORD];
        print("Set a root password: "); read_line(pw1, MAX_PASSWORD);
        if (!pw1[0]) { print("Password cannot be empty\n"); continue; }
        print("Confirm root password: "); read_line(pw2, MAX_PASSWORD);
        if (!streq(pw1, pw2)) { print("Passwords do not match\n"); continue; }
        strncpy(users[0].password, pw1, MAX_PASSWORD);
        break;
    }

    /* Creating regular users is optional and repeatable: the prompt
 * loops so as many accounts as wanted can be made during setup,
 * and only n/N (or Enter, the default) moves on. Each password
 * is confirmed like the root password above. */
    {
        char ans[8];
        while (1) {
            print("Do you want to create a user account? (y/N): ");
            read_line(ans, sizeof(ans));
            if (ans[0] == 'n' || ans[0] == 'N' || !ans[0]) break;
            if (ans[0] != 'y' && ans[0] != 'Y') {
                print("Please answer y or n.\n");
                continue;
            }
            {
                char name[MAX_USERNAME];
                /* login name */
                while (1) {
                    print("Create a login: "); read_line(name, MAX_USERNAME);
                    if (!name[0]) { print("Login cannot be empty\n"); continue; }
                    if (streq(name, "root")) { print("user 'root' already exists\n"); continue; }
                    if (user_find(name) >= 0) {
                        print("user '"); print(name); print("' already exists\n");
                        continue;
                    }
                    break;
                }
                /* password: a mismatch re-asks only the password pair
 * for this login, not the name */
                while (1) {
                    char pass[MAX_PASSWORD], conf[MAX_PASSWORD];
                    print("Create a password: "); read_line(pass, MAX_PASSWORD);
                    if (!pass[0]) { print("Password cannot be empty\n"); continue; }
                    print("Confirm password: "); read_line(conf, MAX_PASSWORD);
                    if (!streq(pass, conf)) { print("Passwords do not match\n"); continue; }
                    int r = user_add(name, pass);
                    if (r == -2) print("user table full\n");
                    break;
                }
            }
        }
    }

    print("Set a hostname: "); read_line(config.hostname, MAX_HOSTNAME);
    config.is_setup = 1; print("\n");
}

static void prompt_root_password(void) {
    /* upgrade path: an account table migrated from the old single-
 * user config has no root password yet. Ask for it once. */
    if (users[0].password[0]) return;
    print("\nThis system has no root password yet.\n");
    while (1) {
        char pw1[MAX_PASSWORD], pw2[MAX_PASSWORD];
        print("Set a root password: "); read_line(pw1, MAX_PASSWORD);
        if (!pw1[0]) { print("Password cannot be empty\n"); continue; }
        print("Confirm root password: "); read_line(pw2, MAX_PASSWORD);
        if (!streq(pw1, pw2)) { print("Passwords do not match\n"); continue; }
        strncpy(users[0].password, pw1, MAX_PASSWORD);
        break;
    }
    store_save();
}

/* record `idx` as the logged-in session user: prompt name, effective
 * uid, and home directory (~). Shared by login and su. */
static void set_session_user(int idx) {
    strncpy(cur_username, users[idx].username, MAX_USERNAME);
    cur_uid = users[idx].uid;
    fs_cur_uid = cur_uid;
    if (cur_uid == 0) {
        strncpy(cur_home, "/root", sizeof(cur_home));
    } else {
        int len = 0, i;
        const char* pre = "/home/";
        for (i = 0; pre[i] && len < 80; i++) cur_home[len++] = pre[i];
        for (i = 0; users[idx].username[i] && len < 90; i++)
            cur_home[len++] = users[idx].username[i];
        cur_home[len] = 0;
    }
}

void login_prompt() {
    char u[MAX_USERNAME], p[MAX_PASSWORD];
    while (1) {
        print(config.hostname); print(" login: "); read_line(u, MAX_USERNAME);
        int idx = user_find(u);
        /* an account with an empty password (passwd with a blank new
 * password) logs straight in, like the classic Unix behavior */
        if (idx >= 0 && users[idx].password[0] == 0) {
            set_session_user(idx);
            return;
        }
        print("Password: "); read_line(p, MAX_PASSWORD);
        if (idx >= 0 && streq(p, users[idx].password)) {
            set_session_user(idx);
            return;
        }
        print("Login incorrect\n\n");
    }
}

void cmd_exit(char* args) { (void)args; shell_exit_flag = 1; }
extern void cmd_doom(char* args);   /* kernel/doom.c */

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

/* tiny atoi for flag values (-u 1500) */
static int atoi_safe(const char* p) {
    int v = 0, neg = 0;
    if (!p) return 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return neg ? -v : v;
}

/* split a writable arg string into space/tab-separated tokens */
static int split_args(char* s, char** tv, int max) {
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        tv[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

/* Account / Permission builtins
 * sudo runs one command as root; useradd/userdel/usermod manage
 * accounts (root only); chmod/chown manage file ownership. They
 * are builtins rather than /bin applets because they need the
 * kernel-internal account table and execute_command(). */
void execute_command(const char* cmd_line);

void cmd_sudo(char* args) {
    if (!args || !args[0]) { print("usage: sudo <command>\n"); return; }
    if (cur_uid == 0) { execute_command(args); return; }
    /* sudoers gate: while a "wheel" group has members, only they may
 * sudo (root always may). Without wheel (or with it empty) every
 * user may, like before. */
    {
        int w = group_find("wheel");
        if (w >= 0 && groups[w].count > 0
            && !group_has_member(w, cur_username)) {
            print(cur_username);
            print(" is not in the sudoers file.  This incident will be reported.\n");
            return;
        }
    }
    /* like real sudo: the prompt asks for the INVOKING user's own
 * password (not root's), verified against their own account,
 * with up to 3 tries */
    int self = user_find(cur_username);
    if (self < 0) { print("sudo: unknown account\n"); return; }
    int tries;
    for (tries = 0; tries < 3; tries++) {
        char pw[MAX_PASSWORD];
        print("[sudo] password for ");
        print(cur_username);
        print(": ");
        read_line(pw, MAX_PASSWORD);
        if (pw[0] == 0) { print("sudo: no password was provided\n"); return; }
        if (users[self].password[0] && streq(pw, users[self].password)) {
            int saved_uid = fs_cur_uid;
            fs_cur_uid = 0;
            execute_command(args);
            fs_cur_uid = saved_uid;
            return;
        }
        if (tries < 2) print("Sorry, try again.\n");
    }
    print("sudo: 3 incorrect password attempts\n");
}

void cmd_useradd(char* args) {
    if (cur_uid != 0) { print("useradd: only root can add users\n"); return; }
    char buf[256];
    char* tv[16];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 16);
    const char* pass = 0;
    int uid = 0;
    char* name = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-p") && i + 1 < n) pass = tv[++i];
        else if (streq(tv[i], "-u") && i + 1 < n) uid = atoi_safe(tv[++i]);
        else if (!name) name = tv[i];
        else { print("usage: useradd [-p password] [-u uid] <name>\n"); return; }
    }
    if (!name) { print("usage: useradd [-p password] [-u uid] <name>\n"); return; }
    if (streq(name, "root")) { print("useradd: user 'root' already exists\n"); return; }
    if (user_find(name) >= 0) { print("useradd: user '"); print(name); print("' already exists\n"); return; }
    char pw[MAX_PASSWORD];
    if (!pass) {
        print("New password: "); read_line(pw, MAX_PASSWORD);
        if (!pw[0]) { print("Password cannot be empty\n"); return; }
        pass = pw;
    }
    int r = user_add_full(name, pass, uid);
    if (r == -1) print("useradd: invalid name\n");
    else if (r == -2) print("useradd: user table full\n");
    /* success is silent, like shadow's useradd */
}

void cmd_userdel(char* args) {
    if (cur_uid != 0) { print("userdel: only root can delete users\n"); return; }
    char buf[256];
    char* tv[16];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 16);
    int rmhome = 0;
    char* name = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-r")) rmhome = 1;          /* remove the home dir */
        else if (tv[i][0] == '-' && tv[i][1]) { print("userdel: unknown option: "); print(tv[i]); print("\n"); return; }
        else if (!name) name = tv[i];
        else { print("usage: userdel [-r] <name>\n"); return; }
    }
    if (!name) { print("usage: userdel [-r] <name>\n"); return; }
    if (streq(name, "root")) { print("userdel: cannot delete the 'root' account\n"); return; }
    if (user_del(name) != 0) { print("userdel: user '"); print(name); print("' does not exist\n"); return; }
    if (rmhome) {
        char home[80];
        int len = 0, k;
        const char* pre = "/home/";
        for (k = 0; pre[k] && len < 70; k++) home[len++] = pre[k];
        for (k = 0; name[k] && len < 78; k++) home[len++] = name[k];
        home[len] = 0;
        if (fs_directory_exists(home))
            fs_delete_directory_recursive(home);
    }
    /* success is silent, like shadow's userdel */
}

void cmd_usermod(char* args) {
    if (cur_uid != 0) { print("usermod: only root can modify users\n"); return; }
    char buf[512];
    char* tv[24];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 24);
    const char* pass = 0;
    const char* newname = 0;
    const char* glist = 0;
    int uid = 0, lock = 0, unlock = 0, append_groups = 0;
    char* name = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (tv[i][0] != '-' || !tv[i][1]) {
            /* not a flag: the user name */
            if (!name) name = tv[i];
            else { print("usage: usermod [-p pass] [-l new] [-u uid] [-aG groups] [-L] [-U] <name>\n"); return; }
            continue;
        }
        /* combined short flags, like real usermod: -aG wheel, -aGwheel,
 * -LU, -p pass ... */
        const char* fl = tv[i] + 1;
        while (*fl) {
            if (*fl == 'a') { append_groups = 1; fl++; }
            else if (*fl == 'L') { lock = 1; fl++; }
            else if (*fl == 'U') { unlock = 1; fl++; }
            else if (*fl == 'p' || *fl == 'l' || *fl == 'u' || *fl == 'G') {
                const char* val;
                if (fl[1]) val = fl + 1;              /* attached: -Gdev */
                else if (i + 1 < n) val = tv[++i];     /* separate: -G dev */
                else {
                    print("usermod: option requires an argument -- '");
                    print_n(fl, 1); print("'\n");
                    return;
                }
                if (*fl == 'p') pass = val;
                else if (*fl == 'l') newname = val;
                else if (*fl == 'u') uid = atoi_safe(val);
                else glist = val;
                break;   /* the rest of the token was the value */
            } else {
                print("usermod: invalid option -- '");
                print_n(fl, 1); print("'\n");
                return;
            }
        }
    }
    if (!name) { print("usage: usermod [-p pass] [-l new] [-u uid] [-aG groups] [-L] [-U] <name>\n"); return; }
    if (append_groups && !glist) {
        print("usermod: -a flag is only allowed with the -G flag\n");
        return;
    }
    /* validate the whole -G list first: all groups must exist */
    if (glist) {
        char gb[256];
        char* gv[16];
        strncpy(gb, glist, sizeof(gb));
        /* split on commas by rewriting them in place */
        {
            int k;
            for (k = 0; gb[k]; k++) if (gb[k] == ',') gb[k] = ' ';
        }
        int gn = split_args(gb, gv, 16);
        for (i = 0; i < gn; i++) {
            if (group_find(gv[i]) < 0) {
                print("usermod: group '"); print(gv[i]); print("' does not exist\n");
                return;
            }
        }
    }
    int idx = user_find(name);
    if (idx < 0) { print("usermod: user '"); print(name); print("' does not exist\n"); return; }

    int changed = 0;
    if (newname) {
        if (streq(newname, "root")) { print("usermod: name 'root' is reserved\n"); return; }
        if (user_find(newname) >= 0) { print("usermod: user '"); print(newname); print("' already exists\n"); return; }
        int j = 0;
        for (j = 0; j < MAX_USERNAME - 1; j++) users[idx].username[j] = 0;
        for (j = 0; newname[j] && j < MAX_USERNAME - 1; j++) users[idx].username[j] = newname[j];
        users[idx].username[j] = 0;
        /* carry group memberships over to the new name */
        {
            int gi;
            for (gi = 0; gi < groups_count; gi++) {
                if (group_has_member(gi, name))
                    { group_member_remove(gi, name); group_member_add(gi, newname); }
            }
        }
        changed = 1;
    }
    if (pass) {
        int j = 0;
        for (j = 0; j < MAX_PASSWORD - 1; j++) users[idx].password[j] = 0;
        for (j = 0; pass[j] && j < MAX_PASSWORD - 1; j++) users[idx].password[j] = pass[j];
        users[idx].password[j] = 0;
        changed = 1;
    }
    if (uid > 0) {
        int old = users[idx].uid;
        users[idx].uid = uid;
        if (uid >= next_uid) next_uid = uid + 1;
        if (old > 0) fs_reassign_uid(old, uid);
        changed = 1;
    }
    if (lock) {
        if (users[idx].uid == 0) { print("usermod: cannot lock root\n"); return; }
        if (users[idx].password[0] != '!') {
            char tmp[MAX_PASSWORD];
            int j = 0;
            tmp[j++] = '!';
            for (j = 0; users[idx].password[j] && j < MAX_PASSWORD - 2; j++)
                tmp[j + 1] = users[idx].password[j];
            tmp[j + 1] = 0;
            strncpy(users[idx].password, tmp, MAX_PASSWORD);
        }
        changed = 1;
    }
    if (unlock) {
        if (users[idx].password[0] == '!') {
            int j;
            for (j = 0; j < MAX_PASSWORD - 1; j++)
                users[idx].password[j] = users[idx].password[j + 1];
            users[idx].password[MAX_PASSWORD - 1] = 0;
        }
        changed = 1;
    }
    /* supplementary groups: -a appends, plain -G replaces */
    if (glist) {
        char gb[256];
        char* gv[16];
        strncpy(gb, glist, sizeof(gb));
        int k, gi;
        for (k = 0; gb[k]; k++) if (gb[k] == ',') gb[k] = ' ';
        int gn = split_args(gb, gv, 16);
        if (!append_groups) {
            for (gi = 0; gi < groups_count; gi++)
                group_member_remove(gi, users[idx].username);
        }
        for (i = 0; i < gn; i++) {
            gi = group_find(gv[i]);
            if (gi >= 0) group_member_add(gi, users[idx].username);
        }
        changed = 1;
    }
    if (!changed) { print("usermod: no changes specified\n"); return; }
    /* success is silent, like shadow's usermod */
    store_save();
}

void cmd_su(char* args) {
    /* su [-] [name] - switch the session user. Without a name it
 * goes to root. "-" is the classic login-shell form and behaves
 * the same. root may become anyone without a password; everyone
 * else must give the TARGET user's password (real su rules) -
 * switching to root therefore always asks for root's password.
 * The change lasts until exit logs out; the prompt and ~ follow
 * the new user. */
    char buf[128];
    char* tv[8];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 8);
    const char* name = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-") || streq(tv[i], "-l") || streq(tv[i], "-login"))
            continue;                     /* login form: same behavior */
        if (!name) name = tv[i];
        else { print("usage: su [-] [name]\n"); return; }
    }
    if (!name) name = "root";
    int idx = user_find(name);
    if (idx < 0) { print("su: user "); print(name); print(" does not exist\n"); return; }
    /* an empty target password (passwd with a blank new password)
 * needs no prompt, mirroring the login screen */
    if (fs_cur_uid != 0 && users[idx].password[0]) {
        char pw[MAX_PASSWORD];
        print("Password: "); read_line(pw, MAX_PASSWORD);
        if (!streq(pw, users[idx].password)) {
            print("su: Authentication failure\n");
            return;
        }
    }
    set_session_user(idx);
    fs_cur_uid = 0;                       /* reachability check as root */
    if (fs_change_directory(cur_home) != 0)
        fs_change_directory("/");
    fs_cur_uid = cur_uid;
}

void cmd_groupadd(char* args) {
    if (cur_uid != 0) { print("groupadd: only root can create groups\n"); return; }
    char buf[128];
    char* tv[4];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 4);
    const char* name = (n > 0) ? tv[0] : 0;
    if (!name || !name[0]) { print("usage: groupadd <group>\n"); return; }
    if (group_find(name) >= 0) {
        print("groupadd: group '"); print(name); print("' already exists\n");
        return;
    }
    if (groups_count >= MAX_GROUPS) { print("groupadd: group table full\n"); return; }
    {
        int i, dst = groups_count++;
        for (i = 0; i < MAX_USERNAME - 1; i++) groups[dst].name[i] = 0;
        for (i = 0; name[i] && i < MAX_USERNAME - 1; i++) groups[dst].name[i] = name[i];
        groups[dst].count = 0;
    }
    /* success is silent, like shadow's groupadd */
    store_save();
}

void cmd_groupdel(char* args) {
    if (cur_uid != 0) { print("groupdel: only root can delete groups\n"); return; }
    char buf[128];
    char* tv[4];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 4);
    const char* name = (n > 0) ? tv[0] : 0;
    if (!name || !name[0]) { print("usage: groupdel <group>\n"); return; }
    int gi = group_find(name);
    if (gi < 0) { print("groupdel: group '"); print(name); print("' does not exist\n"); return; }
    {
        int k;
        for (k = gi; k < groups_count - 1; k++)
            groups[k] = groups[k + 1];
        groups_count--;
        groups[groups_count].name[0] = 0;
        groups[groups_count].count = 0;
    }
    /* success is silent, like shadow's groupdel */
    store_save();
}

void cmd_groups(char* args) {
    char buf[128];
    char* tv[4];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 4);
    const char* name = (n > 0) ? tv[0] : cur_username;
    if (user_find(name) < 0) {
        print("groups: user '"); print(name); print("' does not exist\n");
        return;
    }
    print(name); print(" :");
    {
        int gi;
        for (gi = 0; gi < groups_count; gi++) {
            if (group_has_member(gi, name)) {
                print(" "); print(groups[gi].name);
            }
        }
    }
    print("\n");
}

void cmd_passwd(char* args) {
    /* passwd [name] - mirrors util-linux passwd:
 * Changing password for carl.
 * Current password:
 * New password:
 * Retype new password:
 * passwd: password updated successfully
 * A blank new password erases the password (this system's
 * passwordless-account feature, worded like real `passwd -d`). */
    char buf[128];
    char* tv[4];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 4);
    const char* name = (n > 0) ? tv[0] : cur_username;
    int idx = user_find(name);
    if (idx < 0) { print("passwd: user '"); print(name); print("' does not exist\n"); return; }
    if (!streq(name, cur_username) && cur_uid != 0) {
        print("passwd: You may not view or modify password information for ");
        print(name); print(".\n");
        return;
    }
    print("Changing password for ");
    print(users[idx].username);
    print(".\n");
    /* current password, unless root or the account has none */
    if (cur_uid != 0 && users[idx].password[0]) {
        char cur_pw[MAX_PASSWORD];
        print("Current password: "); read_line(cur_pw, MAX_PASSWORD);
        if (!streq(cur_pw, users[idx].password)) {
            print("passwd: Authentication token manipulation error\n");
            print("passwd: password unchanged\n");
            return;
        }
    }
    {
        char pw1[MAX_PASSWORD], pw2[MAX_PASSWORD];
        int attempt, j;
        for (attempt = 0; attempt < 2; attempt++) {
            print("New password: "); read_line(pw1, MAX_PASSWORD);
            for (j = 0; j < MAX_PASSWORD; j++) users[idx].password[j] = 0;
            if (!pw1[0]) {
                /* blank: erase the password (passwordless login) */
                print("passwd: password erased\n");
                store_save();
                return;
            }
            print("Retype new password: "); read_line(pw2, MAX_PASSWORD);
            if (streq(pw1, pw2)) {
                for (j = 0; pw1[j] && j < MAX_PASSWORD - 1; j++)
                    users[idx].password[j] = pw1[j];
                print("passwd: password updated successfully\n");
                store_save();
                return;
            }
            print("Sorry, passwords do not match.\n");
        }
        print("passwd: Authentication token manipulation error\n");
        print("passwd: password unchanged\n");
    }
}

static int parse_octal(const char* p) {
    int v = 0, i = 0;
    while (p[i] >= '0' && p[i] <= '7' && i < 4) { v = (v << 3) + (p[i] - '0'); i++; }
    return (i > 0) ? v : -1;
}

static void print_octal(int v) {
    static const char oc[] = "01234567";
    char b[5];
    b[0] = oc[(v >> 6) & 7];
    b[1] = oc[(v >> 3) & 7];
    b[2] = oc[v & 7];
    b[3] = 0;
    print(b);
}

/* symbolic mode clauses like +x, -x, u+x, go-rwx, a=rw, "u+x,go-w".
 * Applies each clause to `mode` (the file's current mode) and
 * returns the new mode, or -1 on a malformed spec. */
static int chmod_symbolic(const char* spec, int mode) {
    if (!spec || !spec[0]) return -1;
    while (*spec) {
        int who = 0;                    /* 1=u 2=g 4=o */
        while (*spec == 'u' || *spec == 'g' || *spec == 'o' || *spec == 'a') {
            if (*spec == 'u') who |= 1;
            if (*spec == 'g') who |= 2;
            if (*spec == 'o') who |= 4;
            if (*spec == 'a') who |= 7;
            spec++;
        }
        if (!who) who = 7;              /* bare +x applies to all */
        char op = *spec++;
        if (op != '+' && op != '-' && op != '=') return -1;
        int perm = 0;
        while (*spec == 'r' || *spec == 'w' || *spec == 'x') {
            if (*spec == 'r') perm |= 4;
            if (*spec == 'w') perm |= 2;
            if (*spec == 'x') perm |= 1;
            spec++;
        }
        int mask = 0;
        if (who & 1) mask |= perm << 6;
        if (who & 2) mask |= perm << 3;
        if (who & 4) mask |= perm;
        if (op == '+') mode |= mask;
        else if (op == '-') mode &= ~mask;
        else {
            int clr = 0;
            if (who & 1) clr |= 0700;
            if (who & 2) clr |= 0070;
            if (who & 4) clr |= 0007;
            mode = (mode & ~clr) | mask;
        }
        if (*spec == ',') spec++;
        else if (*spec) return -1;
    }
    return mode & 0777;
}

void cmd_chmod(char* args) {
    char buf[512];
    char* tv[24];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 24);
    int recursive = 0, i, first = 0;
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-R")) recursive = 1;
        else { first = i; break; }
    }
    if (n - first < 2) { print("usage: chmod [-R] <mode> <file>...\n"); return; }
    const char* mode = tv[first];
    for (i = first + 1; i < n; i++) {
        const char* path = tv[i];
        int m;
        if (mode[0] >= '0' && mode[0] <= '7') {
            m = parse_octal(mode);
            if (m < 0) { print("chmod: invalid mode: "); print(mode); print("\n"); return; }
        } else {
            int cur = fs_get_mode(path);
            if (cur < 0) {
                print("chmod: cannot access '"); print(path);
                print("': No such file or directory\n");
                continue;
            }
            m = chmod_symbolic(mode, cur);
            if (m < 0) { print("chmod: invalid mode: "); print(mode); print("\n"); return; }
        }
        int rc = recursive ? fs_chmod_recursive(path, m) : fs_chmod(path, m);
        if (rc != 0) {
            /* GNU wording */
            if (fs_get_mode(path) < 0)
                print("chmod: cannot access '"),
                print(path),
                print("': No such file or directory\n");
            else
                print("chmod: changing permissions of '"),
                print(path),
                print("': Operation not permitted\n");
            continue;
        }
        /* success is silent, like GNU chmod */
    }
}

/* make sure every account has its home directory with the standard
 * folders (root's /root is seeded by fs_seed_home on every boot;
 * this covers users, e.g. a login migrated from a pre-multiuser
 * install whose /home/<name> was never created). Runs as root at
 * boot, once, after the account table is loaded. */
static void users_ensure_homes(void) {
    int i;
    for (i = 1; i < users_count; i++) {
        if (!users[i].username[0]) continue;
        char home[80];
        int len = 0, k;
        const char* pre = "/home/";
        for (k = 0; pre[k] && len < 70; k++) home[len++] = pre[k];
        for (k = 0; users[i].username[k] && len < 78; k++) home[len++] = users[i].username[k];
        home[len] = 0;
        if (!fs_directory_exists(home))
            fs_create_home_tree(home, users[i].uid);
    }
}

void cmd_chown(char* args) {
    /* fs_chown refuses non-root callers; the GNU-style message comes
 * from the failure path below (like real chown's EPERM report) */
    char buf[512];
    char* tv[24];
    strncpy(buf, args ? args : "", sizeof(buf));
    int n = split_args(buf, tv, 24);
    int recursive = 0, i, first = 0;
    for (i = 0; i < n; i++) {
        if (streq(tv[i], "-R")) recursive = 1;
        else { first = i; break; }
    }
    if (n - first < 2) { print("usage: chown [-R] <user> <file>...\n"); return; }
    int idx = user_find(tv[first]);
    if (idx < 0) { print("chown: invalid user: "); print(tv[first]); print("\n"); return; }
    for (i = first + 1; i < n; i++) {
        const char* path = tv[i];
        int r = recursive ? fs_chown_recursive(path, users[idx].uid)
                          : fs_chown(path, users[idx].uid);
        if (r != 0) {
            if (fs_get_mode(path) < 0)
                print("chown: cannot access '"),
                print(path),
                print("': No such file or directory\n");
            else
                print("chown: changing ownership of '"),
                print(path),
                print("': Operation not permitted\n");
            continue;
        }
        /* success is silent, like GNU chown */
    }
}

void print_prompt_path() {
    char cwd[256];
    fs_get_current_path(cwd);

    /* The logged-in user's home directory displays as ~ (root's
 * home is /root; everyone else's is /home/<name>). */
    int hl = 0;
    while (cur_home[hl] && hl < 95) hl++;
    int cl = 0;
    while (cwd[cl] && cl < 255) cl++;

    if (streq(cwd, cur_home)) {
        print("~");
        return;
    }
    /* e.g. cwd="/root/x" with home "/root" -> "~/x" */
    if (cl > hl && cwd[hl] == '/') {
        int match = 1, i;
        for (i = 0; i < hl; i++) if (cwd[i] != cur_home[i]) { match = 0; break; }
        if (match) {
            print("~");
            print(cwd + hl);
            return;
        }
    }
    /* Outside the home directory, show the absolute path. */
    print(cwd);
}

void shell() {
    shell_exit_flag = 0; static char buf[INPUT_BUFFER_SIZE];
    while (1) {
        print(cur_username); print("@"); print(config.hostname); print(":");
        print_prompt_path();
        /* root uses #, all other users use $. */
        if (cur_uid == 0) print("# ");
        else print("$ ");
        read_line(buf, 4096); clean(buf);
        {
            int bl = strlen(buf);
            if (bl > 0 && buf[bl - 1] == 4) {
                buf[bl - 1] = 0;
                if (buf[0] == 0) { print("exit\n"); shell_exit_flag = 1; }
            }
        }
        if (buf[0]) execute_command(buf);
        if (shell_exit_flag) { clear_screen(); break; }
    }
}

void kernel_panic_storage(const char *reason) {
    clear_screen();
    print("panic: due to: Disk size too small for functional TanjaOS environment\n");
    if (reason) {
        print(reason);
        print("\n");
    }
    print("Please use a larger disk next time\n");
    print("panic: due to: Disk size too small for functional TanjaOS environment\n");
    for (;;) asm volatile("cli; hlt");
}

static void kernel_panic_no_bin(void) {
    /* This is intentionally a hard boot failure, not a recovery path.
 * If /bin has been removed or emptied, TanjaOS refuses to start
 * another shell and leaves the panic message on screen. */
    clear_screen();
    print("panic: due to: TanjaOS cannot boot because /bin is missing or empty\n");
    print("The command binaries are required for userspace.\n");
    print("Try reinstall TanjaOS to restore /bin.\n");
    print("panic: due to: TanjaOS cannot boot because /bin is missing or empty\n");
    for (;;) asm volatile("cli; hlt");
}

extern void linux_set_layout(uint32_t kernel_end, uint32_t ram_top);
extern char __kernel_end[];

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

    /* Size the Linux compat regions (ld.so, mmap, PIE exe, stack) to
 * the actual RAM of this machine. mem_upper is in KiB above 1 MiB;
 * without usable multiboot memory info assume a 128 MiB machine. */
    {
        uint32_t ram_top = 128u << 20;
        if (mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && mb_addr) {
            multiboot_info_t* mbi = (multiboot_info_t*)mb_addr;
            if ((mbi->flags & 0x00000001u) && mbi->mem_upper > 0)
                ram_top = (mbi->mem_lower + mbi->mem_upper) * 1024u;
        }
        linux_set_layout((uint32_t)__kernel_end, ram_top);
    }

    boot_log("Initializing filesystem");
    store_init(mb_magic, mb_addr);

    boot_log("Loading command binaries");

    /* /bin is mandatory at boot. Do not recreate it and do not fall back
 * to kernel-provided command binaries: if it was deleted or emptied
 * and the machine is power-cycled/rebooted, deliberately panic here. */
    if (!fs_directory_exists("/bin") || fs_directory_is_empty("/bin"))
        kernel_panic_no_bin();

    register_cmd("exit", cmd_exit);
    register_cmd("hostname", cmd_hostname);
    register_cmd("doom", cmd_doom);
    register_cmd("sudo", cmd_sudo);
    register_cmd("useradd", cmd_useradd);
    register_cmd("userdel", cmd_userdel);
    register_cmd("usermod", cmd_usermod);
    register_cmd("groupadd", cmd_groupadd);
    register_cmd("groupdel", cmd_groupdel);
    register_cmd("groups", cmd_groups);
    register_cmd("su", cmd_su);
    register_cmd("passwd", cmd_passwd);
    register_cmd("chmod", cmd_chmod);
    register_cmd("chown", cmd_chown);

    if (!config.is_setup) {
        print("\n");
        setup_wizard();
        store_save();
    } else {
        prompt_root_password();
    }
    users_ensure_homes();

    boot_log("Starting shell");
    print("\n");

    while (1)
    {
        print("The TanjaOS Project\n\n");

        login_prompt();

        /* A login always starts in the user's home directory (root's
 * is /root, everyone else's /home/<name>). The filesystem
 * persists cwd for storage, but cwd is a shell-session state
 * and should not leak from a previous login/reboot. */
        fs_cur_uid = 0;                    /* home must exist / be reachable */
        if (fs_change_directory(cur_home) != 0)
            fs_change_directory("/");
        fs_cur_uid = cur_uid;

        shell();
    }
}
