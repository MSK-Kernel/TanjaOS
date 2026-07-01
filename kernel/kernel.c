#include <stdint.h>
#include <stddef.h>

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

int caps_lock = 0;

// Special key codes
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_ENTER  0x84
#define KEY_BACKSPACE 0x85

// ============================================================
// COMMAND SYSTEM
// ============================================================

#define MAX_CMDS 64

typedef struct {
    char name[32];
    void (*func)(char* args);
} Command;

Command cmd_table[MAX_CMDS];
int cmd_count = 0;

// ============================================================
// PORT I/O
// ============================================================

void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
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

void clear_screen() {
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

// ============================================================
// NUMBER PRINTING HELPERS
// ============================================================

void print_hex(uint32_t n) {
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[11];
    buffer[0] = '0';
    buffer[1] = 'x';
    buffer[10] = 0;
    
    for (int i = 9; i >= 2; i--) {
        buffer[i] = hex_chars[n & 0xF];
        n >>= 4;
    }
    print(buffer);
}

void print_dec(uint32_t n) {
    if (n == 0) {
        putc('0');
        return;
    }
    
    char buffer[11];
    int pos = 10;
    buffer[pos] = 0;
    
    while (n > 0 && pos > 0) {
        pos--;
        buffer[pos] = '0' + (n % 10);
        n /= 10;
    }
    
    print(&buffer[pos]);
}

void print_dec_pad(uint32_t n, int width) {
    char buffer[11];
    int pos = 10;
    buffer[pos] = 0;
    
    if (n == 0) {
        buffer[--pos] = '0';
    }
    
    while (n > 0 && pos > 0) {
        pos--;
        buffer[pos] = '0' + (n % 10);
        n /= 10;
    }
    
    int len = 10 - pos;
    for (int i = 0; i < width - len; i++) {
        putc(' ');
    }
    
    print(&buffer[pos]);
}

void print_timestamp(uint32_t ms) {
    uint32_t seconds = ms / 1000;
    uint32_t millis = ms % 1000;
    
    print("[    ");
    print_dec_pad(seconds, 1);
    putc('.');
    
    if (millis < 100) putc('0');
    if (millis < 10) putc('0');
    print_dec(millis);
    print("] ");
}

// ============================================================
// KEYBOARD
// ============================================================

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
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        
        if (sc == 0x3A) {
            caps_lock = !caps_lock;
            continue;
        }
        
        if (sc == 0xE0) {
            while (!(inb(0x64) & 1)) continue;
            uint8_t ext_sc = inb(0x60);
            
            if (ext_sc == 0x48) return KEY_UP;
            if (ext_sc == 0x50) return KEY_DOWN;
            if (ext_sc == 0x4B) return KEY_LEFT;
            if (ext_sc == 0x4D) return KEY_RIGHT;
            continue;
        }
        
        if (sc & 0x80) continue;
        if (sc >= 128) continue;
        
        char res;
        if (caps_lock && !shift) {
            res = keymap_caps[sc];
        } else if (shift) {
            res = keymap_shift[sc];
        } else {
            res = keymap[sc];
        }
        
        if (res == 0) continue;
        return res;
    }
}

// ============================================================
// STRING HELPERS
// ============================================================

int streq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

void clean(char* s) {
    if (!s) return;
    int i = 0;
    while (s[i]) {
        if (s[i] == '\n' || s[i] == '\r') s[i] = 0;
        i++;
    }
}

// ============================================================
// INPUT FUNCTIONS
// ============================================================

void read_line(char* buffer, int max_len) {
    char line[256];
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
                cursor--;
                for (int i = pos - 1; i < len - 1; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                pos--;
                cursor = prompt_start + pos;
                sync_cursor();
                for (int i = pos; i < len; i++) {
                    VGA[cursor] = VGA_COLOR | (uint8_t)line[i];
                    cursor++;
                }
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
        
        if (key == KEY_UP || key == KEY_DOWN) {
            continue;
        }
        
        if (key >= 32 && key <= 126) {
            if (len < max_len - 1) {
                for (int i = len; i > pos; i--) {
                    line[i] = line[i - 1];
                }
                line[pos] = (char)key;
                len++;
                pos++;
                
                cursor = prompt_start + pos - 1;
                sync_cursor();
                for (int i = pos - 1; i < len; i++) {
                    VGA[cursor] = VGA_COLOR | (uint8_t)line[i];
                    cursor++;
                }
                cursor = prompt_start + pos;
                sync_cursor();
            }
        }
    }
    
    for (int i = 0; i < len; i++) {
        buffer[i] = line[i];
    }
    buffer[len] = 0;
}

// ============================================================
// COMMAND FUNCTIONS
// ============================================================

void register_cmd(const char* name, void (*func)(char* args)) {
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

void list_commands(void) {
    print("\n");
    print("Available commands:\n");
    for (int i = 0; i < cmd_count; i++) {
        print("  ");
        print(cmd_table[i].name);
        print("\n");
    }
    print("\n");
}

void execute_command(const char* cmd_line) {
    while (*cmd_line == ' ') cmd_line++;
    
    if (!*cmd_line) return;
    
    char cmd_name[32];
    int i = 0;
    while (cmd_line[i] && cmd_line[i] != ' ' && cmd_line[i] != '\n' && cmd_line[i] != '\r' && i < 31) {
        cmd_name[i] = cmd_line[i];
        i++;
    }
    cmd_name[i] = 0;
    
    const char* args = cmd_line + i;
    while (*args == ' ') args++;
    
    for (int i = 0; i < cmd_count; i++) {
        const char* a = cmd_table[i].name;
        const char* b = cmd_name;
        int match = 1;
        while (*a && *b) {
            if (*a != *b) { match = 0; break; }
            a++; b++;
        }
        if (match && *a == 0) {
            cmd_table[i].func((char*)args);
            return;
        }
    }
    
    print("error: Command not found: ");
    print(cmd_name);
    print("\n");
}

// ============================================================
// KERNEL PANIC
// ============================================================

void kernel_panic(const char* msg) {
    asm volatile ("cli");
    
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA[i] = (0x4F << 8) | ' ';
    }
    
    cursor = 0;
    
    print("Kernel panic - not syncing: ");
    print(msg);
    print("\n\n");
    
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
    asm volatile ("mov %%eax, %0" : "=r"(eax));
    asm volatile ("mov %%ebx, %0" : "=r"(ebx));
    asm volatile ("mov %%ecx, %0" : "=r"(ecx));
    asm volatile ("mov %%edx, %0" : "=r"(edx));
    asm volatile ("mov %%esi, %0" : "=r"(esi));
    asm volatile ("mov %%edi, %0" : "=r"(edi));
    asm volatile ("mov %%ebp, %0" : "=r"(ebp));
    asm volatile ("mov %%esp, %0" : "=r"(esp));
    
    print("CPU: 0 PID: 0 Comm: MSK Not tainted 0.1.0\n");
    
    print("Registers:\n");
    print("  EAX: "); print_hex(eax);
    print("  EBX: "); print_hex(ebx);
    print("  ECX: "); print_hex(ecx);
    print("  EDX: "); print_hex(edx); print("\n");
    print("  ESI: "); print_hex(esi);
    print("  EDI: "); print_hex(edi);
    print("  EBP: "); print_hex(ebp);
    print("  ESP: "); print_hex(esp); print("\n\n");
    
    print("Call Trace:\n");
    print("  [<"); print_hex(ebp + 0x1000); print(">] kernel_panic+0x");
    print_hex((uint32_t)&kernel_panic & 0xFFF); print("\n");
    print("  [<"); print_hex(ebp + 0x1004); print(">] init_cmds+0x42\n");
    print("  [<"); print_hex(ebp + 0x1008); print(">] kernel_main+0x");
    print_hex(0x567); print("\n");
    print("  [<"); print_hex(ebp + 0x100C); print(">] ? start_kernel+0x1A4\n");
    print("  [<"); print_hex(ebp + 0x1010); print(">] ? startup_32+0x8F\n\n");
    
    print("Kernel State:\n");
    print("  Command table address: "); print_hex((uint32_t)&cmd_table); print("\n");
    print("  Commands loaded: "); print_dec(cmd_count); print("\n");
    print("  Cursor position: "); print_dec(cursor); print("\n");
    print("  Caps lock state: "); print(caps_lock ? "ON" : "OFF"); print("\n");
    print("  Shift state: "); print(shift ? "ACTIVE" : "inactive"); print("\n\n");
    
    print("---[ end Kernel panic - not syncing: ");
    print(msg);
    print(" ]---\n\n");    

    print("Kernel has encountered a fatal error.\n");
    print("System halted. Manual reboot required.\n\n");
    print("Press any key to attempt reboot...");
    
    while (!(inb(0x64) & 1));
    inb(0x60);
    
    while ((inb(0x64) & 0x02) != 0);
    outb(0x64, 0xFE);
    
    asm volatile (
        "lidt 0\n"
        "int $0"
    );
    
    while (1) {
        asm volatile ("cli; hlt");
    }
}

// ============================================================
// SHELL
// ============================================================

void shell() {
    char buffer[256];
    while (1) {
        print("root:~# ");
        read_line(buffer, 256);
        clean(buffer);
        if (buffer[0]) {
            execute_command(buffer);
        }
    }
}

// ============================================================
// INIT
// ============================================================

extern void init_cmds(void);

// ============================================================
// KERNEL MAIN
// ============================================================

void kernel_main() {
    block_cursor();
    clear_screen();
    
    init_cmds();
    
    if (cmd_count == 0) {
        uint32_t boot_time;
        asm volatile ("rdtsc" : "=a"(boot_time));
        boot_time = boot_time / 1000000; // Convert to milliseconds
        
        print("[    ");
        print_dec_pad(boot_time, 1);
        print(".");
        print_dec(0);  // milliseconds decimal
        print("] --- Kernel panic: not syncing: Failed to load command(s)\n");
        print("[    ");
        print_dec_pad(boot_time, 1);
        print(".");
        print_dec(0);
        print("] failed to load command(s)\n");
        print("[    ");
        print_dec_pad(boot_time, 1);
        print(".");
        print_dec(0);
        print("] previous error may be due to failed command loading or no commands in the /cmd directory\n");
        print("[    ");
        print_dec_pad(boot_time, 1);
        print(".");
        print_dec(0);
        print("] --- end Kernel panic: not syncing: Failed to load command(s) ---\n");
        print("[    ");
        print_dec_pad(boot_time, 1);
        print(".");
        print_dec(0);
        print("] Use 'make commandset' to generate base commands before using 'make'\n");
        while (1) {
            asm volatile ("cli; hlt");
        }
    }
    
    shell();
}
