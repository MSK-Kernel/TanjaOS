#ifndef CMD_H
#define CMD_H

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

// VGA text-mode colors (attribute already shifted into the high byte,
// same convention as VGA_COLOR in kernel.c: (fg << 8) with black bg).
#define COLOR_WHITE       (0x0F << 8) // normal files
#define COLOR_LIGHT_GREEN (0x0A << 8) // files that look like scripts
#define COLOR_DIR         (0x09 << 8) // directories (light magenta - the
                                       // closest the 16-color VGA palette
                                       // gets to a blue-ish mauve)

#endif
