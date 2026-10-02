#include <stdint.h>

extern int key_available(void);
extern uint8_t inb(uint16_t port);
extern int get_key(void);
extern uint8_t inb(uint16_t port);
extern int get_key(void);

void delay(unsigned int ticks)
{
    volatile unsigned int i;

    while (ticks--)
        for (i = 0; i < 1000; i++)
            asm volatile("nop");
}
