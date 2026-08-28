#ifndef BIN_H
#define BIN_H

#include <stdint.h>
#include <stddef.h>

// External kernel functions available to commands
extern int key_available(void);
extern int get_key(void);
extern void print(const char* s);
extern void putc(char c);
extern void putc_color(char c, uint16_t color);
extern void print_color(const char* s, uint16_t color);
extern int cmd_exists(const char* name);
extern void clear_screen(void);
extern void register_cmd(const char* name, void (*func)(char* args));
extern void list_commands(void);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);

// Timing (see kernel/kernel.c - PIT-driven, hlt-based, doesn't busy-spin)
extern uint32_t get_uptime_ms(void);
extern void timer_delay_ms(uint32_t ms);

// Shell environment variables ($VAR expansion happens in execute_command
// before a command's args are handed to it; `read` is the exception,
// since it needs the literal variable name rather than its expansion)
extern void set_env(const char* name, const char* value);
extern const char* get_env(const char* name);

// String/number helpers already implemented in kernel/kernel.c
extern int strlen(const char* s);
extern char* strcpy(char* dst, const char* src);
extern char* strncpy(char* dst, const char* src, unsigned int n);
extern int streq(const char* a, const char* b);
extern int atoi(const char* s);
extern void print_dec(uint32_t n);

// Extended key codes returned by get_key() - must match kernel/kernel.c
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83
#define KEY_ENTER     0x84
#define KEY_BACKSPACE 0x85

// VGA text-mode colors (attribute already shifted into the high byte,
// same convention as VGA_COLOR in kernel.c: (fg << 8) with black bg).
#define COLOR_WHITE       (0x0F << 8) // normal files
#define COLOR_LIGHT_GREEN (0x0A << 8) // files that look like scripts
#define COLOR_DIR         (0x09 << 8) // directories (light magenta - the
                                       // closest the 16-color VGA palette
                                       // gets to a blue-ish mauve)

#endif
