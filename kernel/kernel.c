#include <stdint.h>
#include <stddef.h>

#define VGA_COLOR (0x0F << 8)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

#define MAX_FOLDERS 32
#define MAX_FILES 64

// ================= VGA =================
uint16_t* VGA = (uint16_t*)0xB8000;
int cursor = 0;

// VGA text mode cursor uses a position in CHARACTER CELLS.
// Our cursor is an index in the VGA word array (2 bytes per cell), so we convert with /2.

void print(const char* s);
void putc(char c);

void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ================= CURSOR =================
static inline void set_hw_cursor_cell(int cell_pos) {
    int max_cells = VGA_WIDTH * VGA_HEIGHT - 1;
    if (cell_pos < 0) cell_pos = 0;
    if (cell_pos > max_cells) cell_pos = max_cells;

    // VGA hardware cursor position is in CHARACTER CELLS.
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(cell_pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((cell_pos >> 8) & 0xFF));
}

void enable_block_cursor() {
    // 8x16 text mode: solid block cursor (start=0 end=15)
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00); // cursor start

    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F); // cursor end

    // Enable cursor by setting cursor disable bit appropriately.
    // Reg 0x0A: bits include disable at 5 in standard VGA implementations.
    uint8_t r = inb(0x3D5);
    outb(0x3D4, 0x0A);
    outb(0x3D5, (uint8_t)(r & ~(1 << 5))); // ensure enabled
}

static inline void sync_cursor_hw() {
    set_hw_cursor_cell(cursor / 2); // cursor is byte index in VGA memory words (2 bytes per cell)
}

// ================= SCREEN =================
void scroll() {
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        VGA[i] = VGA[i + VGA_WIDTH];

    for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';

    cursor -= VGA_WIDTH * 2; // cursor is in VGA word-array indices (2 bytes per cell)
    sync_cursor_hw();
}

void putc(char c) {
    if (c == '\n') {
        cursor += (VGA_WIDTH * 2) - (cursor % (VGA_WIDTH * 2));
    } else if (c == '\b') {
        if (cursor > 0) {
            cursor -= 2;
            VGA[cursor / 2] = VGA_COLOR | ' ';
        }
        sync_cursor_hw();
        return;
    } else {
        VGA[cursor / 2] = VGA_COLOR | (uint8_t)c;
        cursor += 2;
    }

    if (cursor >= VGA_WIDTH * VGA_HEIGHT * 2)
        scroll();

    sync_cursor_hw();
}

void print(const char* s) {
    while (*s) putc(*s++);
}

void clear() {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';
    cursor = 0;
    sync_cursor_hw();
}

// ================= KEYBOARD =================

int shift = 0;

char keymap[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',
    8,9,
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',
    0,'*',0,' '
};

char keymap_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',
    8,9,
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
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

        return shift ? keymap_shift[sc] : keymap[sc];
    }
}

// ================= HELPERS =================
int streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

char* skip(char* s) {
    while (*s == ' ') s++;
    return s;
}

void clean(char* s) {
    int i = 0;
    while (s[i]) {
        if (s[i] == '\n' || s[i] == '\r')
            s[i] = 0;
        i++;
    }
}

void trim(char* s) {
    int len = 0;
    while (s[len]) len++;
    while (len > 0 && s[len - 1] == ' ') {
        s[len - 1] = 0;
        len--;
    }
}

char* arg(char* s) {
    while (*s && *s != ' ') s++;
    return skip(s);
}

// ================= FILESYSTEM =================
typedef struct Folder Folder;
typedef struct { char name[32]; char content[128]; } File;

struct Folder {
    char name[32];
    Folder* parent;
    Folder* folders[MAX_FOLDERS];
    int folder_count;
    File files[MAX_FILES];
    int file_count;
};

Folder root;
Folder* current;
Folder folder_pool[MAX_FOLDERS];
int folder_used = 0;

void init_fs() {
    root.name[0] = '/';
    root.name[1] = 0;
    root.parent = 0;
    root.folder_count = 0;
    root.file_count = 0;
    current = &root;
}

// ================= COMMANDS =================
void cf(char* name) {
    if (folder_used >= MAX_FOLDERS) {
        print("\nerror: Folder limit reached");
        return;
    }

    if (current->folder_count >= MAX_FOLDERS) {
        print("\nerror: Too many folders");
        return;
    }

    name = skip(name);
    trim(name);

    Folder* f = &folder_pool[folder_used++];
    int i = 0;

    while (name[i] && i < 31) {
        f->name[i] = name[i];
        i++;
    }
    f->name[i] = 0;

    f->parent = current;
    f->folder_count = 0;
    f->file_count = 0;

    current->folders[current->folder_count++] = f;
}

void rf(char* name) {
    name = skip(name);
    trim(name);

    for (int i = 0; i < current->folder_count; i++) {
        if (streq(current->folders[i]->name, name)) {
            current->folders[i] = current->folders[current->folder_count - 1];
            current->folder_count--;
            return;
        }
    }
    print("\nerror: Folder not found");
}

void rt(char* name) {
    name = skip(name);
    trim(name);

    if (current == 0) {
        print("\nerror: current folder invalid");
        return;
    }

    if (current->file_count <= 0 || current->file_count > MAX_FILES) {
        print("\nerror: filesystem corrupted");
        current->file_count = 0;
        return;
    }

    for (int i = 0; i < current->file_count; i++) {
        if (streq(current->files[i].name, name)) {

            // Shift remaining files down
            for (int j = i; j < current->file_count - 1; j++)
                current->files[j] = current->files[j + 1];

            current->file_count--;

            return;
        }
    }

    print("\nerror: File not found");
}

void cd(char* name) {
    name = skip(name);
    trim(name);

    if (name[0] == 0) {
        current = &root;
        return;
    }

    if (name[0] == '.' && name[1] == '.') {
        if (current->parent)
            current = current->parent;
        return;
    }

    for (int i = 0; i < current->folder_count; i++) {
        if (streq(current->folders[i]->name, name)) {
            current = current->folders[i];
            return;
        }
    }

    print("\nerror: Folder not found");
}

void ct(char* name, char* content) {
    if (current->file_count >= MAX_FILES) {
        print("\nerror: File limit reached");
        return;
    }

    name = skip(name);
    trim(name);

    content = skip(content);
    trim(content);

    File* f = &current->files[current->file_count++];

    int i = 0;
    while (name[i] && i < 31) {
        f->name[i] = name[i];
        i++;
    }
    f->name[i] = 0;

    i = 0;
    while (content[i] && i < 127) {
        f->content[i] = content[i];
        i++;
    }
    f->content[i] = 0;
}

void fl(char* name) {
    name = skip(name);
    trim(name);

    for (int i = 0; i < current->file_count; i++) {
        if (streq(current->files[i].name, name)) {
            print("\n");
            print(current->files[i].content);
            return;
        }
    }
    print("\nerror: File not found");
}

void echo(char* s) {
    print("\n");
    print(skip(s));
}

void ls() {
    if (current == 0) {
        print("\nerror: Current folder is invalid");
        return;
    }

    if (current->folder_count < 0 || current->folder_count > MAX_FOLDERS) {
        print("\nerror: Folder list corrupted");
        current->folder_count = 0;
    }

    if (current->file_count < 0 || current->file_count > MAX_FILES) {
        print("\nerror: File list corrupted");
        current->file_count = 0;
    }

    print("\n");

    for (int i = 0; i < current->folder_count; i++) {
        if (current->folders[i]) {
            print("[DIR] ");
            print(current->folders[i]->name);
            print("\n");
        }
    }

    for (int i = 0; i < current->file_count; i++) {
        print("[FILE] ");
        print(current->files[i].name);
        print("\n");
    }
}

// ================= SHELL =================
void backspace() {
    if (cursor > 0) {
        cursor -= 2;
        VGA[cursor / 2] = VGA_COLOR | ' ';
    }
    sync_cursor_hw();
}

void shell() {
    char buffer[256];

    while (1) {
        print("\n> ");
        int i = 0;

        while (1) {
            char c = get_key();

            if (c == '\n') {
                putc('\n');
                break;
            }

            if (c == 8) {
                if (i > 0) {
                    i--;
                    backspace();
                }
                continue;
            }

            if (i < 255) {
                buffer[i++] = c;
            }

            putc(c);
        }

        buffer[i] = 0;
        clean(buffer);
        char* cmd = skip(buffer);

        if (cmd[0]=='l' && cmd[1]=='s') ls();
        else if (cmd[0]=='c' && cmd[1]=='d') cd(arg(cmd));
        else if (cmd[0]=='c' && cmd[1]=='f') cf(arg(cmd));
        else if (cmd[0]=='r' && cmd[1]=='f') rf(arg(cmd));
        else if (cmd[0]=='r' && cmd[1]=='t') rt(arg(cmd));
        else if ((cmd[0]=='f' && cmd[1]=='l') || (cmd[0]=='l' && cmd[1]=='f')) fl(arg(cmd));
        else if (cmd[0]=='c' && cmd[1]=='t') {
            char* p = arg(cmd);
            char* eq = p;

            while (*eq && *eq != '=') eq++;

            if (*eq == 0) {
                print("\nerror: Invalid ct format");
                continue;
            }

            *eq = 0;
            ct(p, eq + 1);
        }
        else if (cmd[0]=='e' && cmd[1]=='c' && cmd[2]=='h' && cmd[3]=='o')
            echo(cmd + 4);
        else if (cmd[0]=='c' && cmd[1]=='l')
            clear();
        else
            print("\nerror: Unknown command");
    }
}

// ================= KERNEL =================
void kernel_main() {
    enable_block_cursor();
    clear();
    init_fs();
    shell();
}
