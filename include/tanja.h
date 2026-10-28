#ifndef TANJA_H
#define TANJA_H

#include <stdint.h>
#include <stddef.h>

#include "fs.h"
#include "net.h"

void main(char* args);

extern void print(const char* s);
extern int printf(const char* fmt, ...);
extern char* itoa(int value, char* buf, int base);
extern void print_n(const char* s, uint32_t len);
extern void print_color(const char* s, uint16_t color);
extern void console_putc(char c);
extern void putc_color(char c, uint16_t color);
extern int putchar(int c);
extern int getchar(void);
extern int puts(const char* s);
extern void clear_screen(void);
extern void print_dec(uint32_t n);
extern void print_dec_pad(uint32_t n, int width);
extern void print_hex(uint32_t n);
extern void boot_log(const char* msg);

/* Direct VGA access for full-screen programs (the editor is the
 * main user). cursor is the current cell index into VGA[]. */
extern uint16_t* VGA;
extern int cursor;
extern void sync_cursor(void);

extern int get_key(void);          /* blocks; returns ASCII or a KEY_* code */
extern int key_available(void);
extern void read_line(char* buf, int max_len);

/* Extended key codes returned by get_key() - must match kernel.c */
#define KEY_UP        0x80
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83
#define KEY_ENTER     0x84
#define KEY_BACKSPACE 0x85

/* VGA text-mode colors (attribute already shifted into the high byte,
 * black background: (fg << 8)). */
#define COLOR_WHITE       (0x0F << 8)
#define COLOR_LIGHT_GREEN (0x0A << 8)
#define COLOR_DIR         (0x09 << 8)
#define COLOR_RED         (0x0C << 8)

extern int strlen(const char* a);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, unsigned int n);
extern char* strcpy(char* dst, const char* src);
extern char* strncpy(char* dst, const char* src, unsigned int n);
extern char* strcat(char* dst, const char* src);
extern char* strchr(const char* s, int c);
extern char* strrchr(const char* s, int c);
extern char* strstr(const char* haystack, const char* needle);
extern int strcasecmp(const char* a, const char* b);
extern int strncasecmp(const char* a, const char* b, unsigned int n);
extern char* strlwr(char* s);
extern char* strupr(char* s);
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

extern int streq(const char* a, const char* b);
extern void clean(char* s);

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void outw(uint16_t port, uint16_t val);

extern uint32_t get_uptime_ms(void);
extern void timer_delay_ms(uint32_t ms);

extern void execute_command(const char* cmd_line);
extern int exec_file(const char* path, const char* args);
extern void register_cmd(const char* name, void (*func)(char* args));
extern int cmd_exists(const char* name);
extern void list_commands(void);
extern int  builtin_names(char* out, int cap);

extern void set_env(const char* name, const char* value);
extern const char* get_env(const char* name);
/* multi-user support: the logged-in user's home directory (root's is
 * /root, everyone else's /home/<name>) - cd's bare and ~ paths */
extern void tanja_get_home(char* out, int cap);
/* file mode (UNIX bits) of a path, for ls-style executable coloring */
extern int fs_get_mode(const char* path);
/* owner uid of a path (-1 if missing) */
extern int fs_get_owner(const char* path);
/* 13 after a permission refusal on the last FS call; rc == -2 */
extern int fs_errno;
/* set to 1 by programs that read the PS/2 controller directly (Doom):
 * disables the kernel's Ctrl+C scancode sniffing while it runs */
extern int kb_raw_mode;

extern void store_save(void);
extern void store_autosave(void);
extern int store_is_persistent(void);
extern void fs_init(void);
extern void fs_seed_home(void);
extern void config_reset(void);
extern void setup_wizard(void);

#endif
