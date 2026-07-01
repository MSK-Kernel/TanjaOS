#ifndef CMD_H
#define CMD_H

#include <stdint.h>

// ============================================================
// COMMAND FUNCTION TYPE
// ============================================================
typedef void (*cmd_func_t)(char* args);

// ============================================================
// KERNEL FUNCTIONS AVAILABLE TO COMMANDS
// ============================================================

// I/O Port functions
void outb(uint16_t port, uint8_t val);
void outw(uint16_t port, uint16_t val);
uint8_t inb(uint16_t port);

// Screen functions
void print(const char* s);
void putc(char c);
void clear_screen(void);
void print_hex(uint32_t n);
void print_dec(uint32_t n);

// Command registration (called by auto-generated init.c)
void register_cmd(const char* name, void (*func)(char* args));
void list_commands(void);

#endif // CMD_H
