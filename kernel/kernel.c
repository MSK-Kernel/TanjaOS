#include <stdint.h>
#include <stddef.h>
#include "../include/fs.h"

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

// ============================================================
// COMMAND SYSTEM
// ============================================================

// ============================================================
// USER CONFIGURATION
// ============================================================

#define MAX_USERNAME 32
#define MAX_PASSWORD 32
#define MAX_HOSTNAME 64

typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    char hostname[MAX_HOSTNAME];
    int is_setup;
} user_config_t;

user_config_t config = { .is_setup = 0 };

int shell_exit_flag = 0;

// ============================================================
// PORT I/O
// ============================================================

void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
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

void underline_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0D);

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
        cursor = ((cursor / VGA_WIDTH) + 1) * VGA_WIDTH;
    } else if (c == '\b') {
        if (cursor > 0) {
            cursor--;
            VGA[cursor] = VGA_COLOR | ' ';
        }
    } else if (c >= ' ') {
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

// ============================================================
// NUMBER PRINTING
// ============================================================

void print_hex(uint32_t n) {
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[11];
    buffer[0] = '0'; buffer[1] = 'x'; buffer[10] = 0;
    for (int i = 9; i >= 2; i--) { buffer[i] = hex_chars[n & 0xF]; n >>= 4; }
    print(buffer);
}

void print_dec(uint32_t n) {
    if (n == 0) { putc('0'); return; }
    char buffer[11]; int pos = 10; buffer[pos] = 0;
    while (n > 0 && pos > 0) { pos--; buffer[pos] = '0' + (n % 10); n /= 10; }
    print(&buffer[pos]);
}

void print_dec_pad(uint32_t n, int width) {
    char buffer[11]; int pos = 10; buffer[pos] = 0;
    if (n == 0) buffer[--pos] = '0';
    while (n > 0 && pos > 0) { pos--; buffer[pos] = '0' + (n % 10); n /= 10; }
    int len = 10 - pos;
    for (int i = 0; i < width - len; i++) putc(' ');
    print(&buffer[pos]);
}

// ============================================================
// KEYBOARD
// ============================================================

int shift = 0;
int ctrl = 0;

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
        if (sc == 0x1D) { ctrl = 1; continue; }
        if (sc == 0x9D) { ctrl = 0; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc == 0x3A) { caps_lock = !caps_lock; continue; }
        if (sc == 0xE0) {
            while (!(inb(0x64) & 1)) continue;
            uint8_t ext = inb(0x60);
            if (ext == 0x48) return KEY_UP;
            if (ext == 0x50) return KEY_DOWN;
            if (ext == 0x4B) return KEY_LEFT;
            if (ext == 0x4D) return KEY_RIGHT;
            continue;
        }
        if (sc & 0x80) continue;
        if (sc >= 128) continue;
        char res;
        if (caps_lock && !shift) res = keymap_caps[sc];
        else if (shift) res = keymap_shift[sc];
        else res = keymap[sc];
        if (res == 0) continue;
        if (ctrl) {
            if (res == 'x') return 24;
        }
        return res;
    }
}

int key_available(void)
{
    return (inb(0x64) & 1);
}

// ============================================================
// STRING HELPERS
// ============================================================

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

// ============================================================
// INPUT
// ============================================================

#define INPUT_BUFFER_SIZE 4096

void read_line(char* buffer, int max_len) {
    char line[INPUT_BUFFER_SIZE];
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
}

// ============================================================
// COMMAND SYSTEM
// ============================================================

void register_cmd(const char* name, void (*func)(char* args)) {

    if (cmd_pool_index >= CMD_POOL_SIZE)
        return;

    Command* cmd = &cmd_pool[cmd_pool_index++];

    int i = 0;
    while (name[i] && i < 31) {
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

void list_commands(void) {
    print("\nAvailable commands:\n");

    Command* cmd = cmd_table;

    while (cmd) {
        print("  ");
        print(cmd->name);
        print("\n");

        cmd = cmd->next;
    }

    print("\n");
}

void execute_command(const char* cmd_line) {
    while (*cmd_line == ' ')
        cmd_line++;

    if (!*cmd_line)
        return;

    char cmd_name[32];
    int i = 0;

    while (cmd_line[i] && cmd_line[i] != ' ' && i < 31) {
        cmd_name[i] = cmd_line[i];
        i++;
    }

    cmd_name[i] = 0;

    const char* args = cmd_line + i;

    while (*args == ' ')
        args++;

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
            cmd->func((char*)args);
            return;
        }

        cmd = cmd->next;
    }

    print("error: Command not found: ");
    print(cmd_name);
    print("\n");
}

// ============================================================
// SHELL
// ============================================================

void setup_wizard() {
    print("===== TanjaOS Setup =====\n");
    print("\n");
    print("Create a login: "); read_line(config.username, MAX_USERNAME);
    print("Create a password: "); read_line(config.password, MAX_PASSWORD);
    print("Set a hostname: "); read_line(config.hostname, MAX_HOSTNAME);
    config.is_setup = 1; clear_screen();
}

void login_prompt() {
    char u[MAX_USERNAME], p[MAX_PASSWORD];
    while (1) {
        print("The TanjaOS Project\n\n");
        print(config.hostname); print(" login: "); read_line(u, MAX_USERNAME);
        print("Password: "); read_line(p, MAX_PASSWORD);
        if (streq(u, config.username) && streq(p, config.password)) { print("\n"); return; }
        print("Invalid credentials. Try again\n");
    }
}

void cmd_exit(char* args) { (void)args; shell_exit_flag = 1; }
void cmd_hostname(char* args) {
    if (args && args[0]) {
        int i = 0;
        while (i < MAX_HOSTNAME-1 && args[i] && args[i] != ' ') { config.hostname[i] = args[i]; i++; }
        config.hostname[i] = 0; print("Hostname updated\n");
    } else { print(config.hostname); print("\n"); }
}

void print_prompt_path() {
    char cwd[256];
    fs_get_current_path(cwd);
    if (cwd[0] == '/' && cwd[1] == 0) {
        // at home ("/")
        print("~");
    } else {
        // e.g. cwd="/folder/project" -> "~/folder/project"
        print("~");
        print(cwd);
    }
}

void shell() {
    shell_exit_flag = 0; char buf[4096];
    while (1) {
        print(config.username); print("@"); print(config.hostname); print(":");
        print_prompt_path();
        print("$ ");
        read_line(buf, 4096); clean(buf);
        if (buf[0]) execute_command(buf);
        if (shell_exit_flag) { clear_screen(); break; }
    }
}

extern void init_cmds(void);

void kernel_main() {
    underline_cursor(); clear_screen();
    fs_init(); init_cmds();
    if (cmd_count == 0) { print("---[ Kernel panic - not syncing: Unable to load commands ]---\n    command count=0\n    Please use 'make commandset' to generate base commands\n---[ end Kernel panic - not syncing: Unable to load commands ]---\n"); while (1); }
    register_cmd("exit", cmd_exit);
    register_cmd("hostname", cmd_hostname);
    if (!config.is_setup) setup_wizard();
    while (1) { login_prompt(); clear_screen(); print("Welcome to TanjaOS!\nType 'help' for commands.\n\n"); shell(); }
}
