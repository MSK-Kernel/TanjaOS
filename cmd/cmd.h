#ifndef CMD_H
#define CMD_H

#include <stdint.h>
#include <stddef.h>

// External kernel functions available to commands
extern int key_available(void);
extern int get_key(void);
extern void print(const char* s);
extern void putc(char c);
extern void clear_screen(void);
extern void register_cmd(const char* name, void (*func)(char* args));
extern void list_commands(void);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);

#endif
