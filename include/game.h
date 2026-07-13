#ifndef GAME_H
#define GAME_H

void delay(unsigned int ticks);

int key_available(void);
int get_key(void);

void clear_screen(void);
void putc(char c);
void print(const char *s);

#endif
