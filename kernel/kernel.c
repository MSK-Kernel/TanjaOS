#include <stdint.h>
#include <stddef.h>

#define VGA_COLOR (0x0F << 8)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define MAX_CMDS 32

uint16_t* VGA = (uint16_t*)0xB8000;
int cursor = 0;

// Command registry
typedef void (*cmd_func_t)(char* args);

typedef struct {
    char name[32];
    cmd_func_t func;
} Command;

Command cmd_table[MAX_CMDS];
int cmd_count = 0;

void register_cmd(const char* name, cmd_func_t func) {
    if (cmd_count < MAX_CMDS) {
        int i = 0;
        while (name[i] && i < 31) {
            cmd_table[cmd_count].name[i] = name[i];
            i++;
        }
        cmd_table[cmd_count].name[i] = 0;
        cmd_table[cmd_count].func = func;
        cmd_count++;
    }
}

// Port I/O
void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Cursor
void sync_cursor() {
    int max = VGA_WIDTH * VGA_HEIGHT - 1;
    if (cursor < 0) cursor = 0;
    if (cursor > max) cursor = max;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(cursor & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((cursor >> 8) & 0xFF));
}

void block_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
}

// Screen
void scroll() {
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        VGA[i] = VGA[i + VGA_WIDTH];
    for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';
    cursor = VGA_WIDTH * (VGA_HEIGHT - 1);
}

void putc(char c) {
    if (c == '\n') {
        cursor += VGA_WIDTH - (cursor % VGA_WIDTH);
    } else if (c == '\b') {
        if (cursor > 0) {
            cursor--;
            VGA[cursor] = VGA_COLOR | ' ';
        }
    } else {
        VGA[cursor] = VGA_COLOR | (uint8_t)c;
        cursor++;
    }
    if (cursor >= VGA_WIDTH * VGA_HEIGHT) scroll();
    sync_cursor();
}

void print(const char* s) {
    if (!s) return;
    while (*s) putc(*s++);
}

void clear() {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';
    cursor = 0;
    sync_cursor();
}

void backspace() {
    if (cursor > 0) {
        cursor--;
        VGA[cursor] = VGA_COLOR | ' ';
    }
    sync_cursor();
}

// Keyboard
int shift = 0;

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

char get_key() {
    while (1) {
        if (!(inb(0x64) & 1)) continue;
        uint8_t sc = inb(0x60);
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc & 0x80) continue;
        if (sc >= 128) continue;
        char res = shift ? keymap_shift[sc] : keymap[sc];
        if (res == 0) continue;
        return res;
    }
}

// String helpers
int streq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

char* skip(char* s) {
    if (!s) return NULL;
    while (*s == ' ') s++;
    return s;
}

void clean(char* s) {
    if (!s) return;
    int i = 0;
    while (s[i]) {
        if (s[i] == '\n' || s[i] == '\r') s[i] = 0;
        i++;
    }
}

// Shell
extern void init_cmds();

void execute(char* cmd) {
    cmd = skip(cmd);
    clean(cmd);
    if (!cmd[0]) return;
    
    char name[32];
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < 31) {
        name[i] = cmd[i];
        i++;
    }
    name[i] = 0;
    
    char* args = cmd + i;
    if (*args == ' ') args++;
    
    for (int j = 0; j < cmd_count; j++) {
        if (streq(cmd_table[j].name, name)) {
            cmd_table[j].func(args);
            return;
        }
    }
    
    print("error: Command not found: ");
    print(name);
}

void shell() {
    char buffer[256];
    while (1) {
        print("\nroot:~# ");
        int i = 0;
        while (1) {
            char c = get_key();
            if (c == '\n') { putc('\n'); break; }
            if (c == 8) { if (i > 0) { i--; backspace(); } continue; }
            if (i < 254) { buffer[i++] = c; putc(c); }
        }
        buffer[i] = 0;
        if (i > 0) execute(buffer);
    }
}

void kernel_main() {
    block_cursor();
    clear();
    
    init_cmds();
    
    if (cmd_count == 0) {
        print("warning: No commands in /cmd\n");
    }
    
    shell();
}
