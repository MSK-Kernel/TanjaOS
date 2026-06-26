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

// ================= FORWARD DECLARATIONS =================
void print(const char* s);
void putc(char c);
void clear();
void shell();
void execute(char* cmd);
void backspace();

void cf(char* name);
void rf(char* name);
void ct(char* name, char* content);
void rt(char* name);
void cd(char* name);
void fl(char* name);
void echo(char* s);
void ls();
void pwd();
void mv(char* src, char* dst);
void cp(char* src, char* dst);
void init_fs();

// ================= PORT I/O =================
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

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(cell_pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((cell_pos >> 8) & 0xFF));
}

static inline void sync_cursor_hw() {
    set_hw_cursor_cell(cursor / 2);
}

void enable_block_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x00);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);

    uint8_t r = inb(0x3D5);
    outb(0x3D4, 0x0A);
    outb(0x3D5, (uint8_t)(r & ~(1 << 5)));
}

// ================= SCREEN =================
void scroll() {
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        VGA[i] = VGA[i + VGA_WIDTH];

    for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = VGA_COLOR | ' ';

    cursor -= VGA_WIDTH * 2;
    sync_cursor_hw();
}

void putc(char c) {
    if (c == '\n') {
        cursor += (VGA_WIDTH * 2) - (cursor % (VGA_WIDTH * 2));
    } else if (c == '\b') {
        backspace();
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

void backspace() {
    if (cursor > 0) {
        cursor -= 2;
        VGA[cursor / 2] = VGA_COLOR | ' ';
    }
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

int name_exists(const char* name) {
    // Check folders
    for (int i = 0; i < current->folder_count; i++) {
        if (streq(current->folders[i]->name, name))
            return 1;
    }

    // Check files
    for (int i = 0; i < current->file_count; i++) {
        if (streq(current->files[i].name, name))
            return 1;
    }

    return 0;
}

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
    if (folder_used >= MAX_FOLDERS || current->folder_count >= MAX_FOLDERS) {
        print("\nerror: Folder limit reached");
        return;
    }

    name = skip(name);
    trim(name);

    if (name[0] == 0) {
        print("\nerror: Invalid folder name");
        return;
    }

    if (name_exists(name)) {
        print("\nerror: Name already exists");
        return;
    }

    Folder* f = &folder_pool[folder_used++];

    for (int i = 0; i < MAX_FOLDERS; i++)
        f->folders[i] = 0;

    f->folder_count = 0;
    f->file_count = 0;
    f->parent = current;

    int i = 0;
    while (name[i] && i < 31) {
        f->name[i] = name[i];
        i++;
    }
    while (i < 32)
        f->name[i++] = 0;

    current->folders[current->folder_count++] = f;
}

void rf(char* name) {
    name = skip(name);
    trim(name);

    if (name[0] == 0) {
        print("\nerror: Invalid folder name");
        return;
    }

    for (int i = 0; i < current->folder_count; i++) {

        Folder* f = current->folders[i];

        if (!f)
            continue;

        if (streq(f->name, name)) {

            if (f->folder_count > 0 || f->file_count > 0) {
                print("\nerror: Folder not empty");
                return;
            }

            current->folders[i] =
                current->folders[current->folder_count - 1];

            current->folders[current->folder_count - 1] = 0;

            current->folder_count--;

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

    if (name[0] == 0) {
        print("\nerror: Invalid file name");
        return;
    }

    if (name_exists(name)) {
        print("\nerror: Name already exists");
        return;
    }

    File* f = &current->files[current->file_count++];

    int i = 0;

    while (name[i] && i < 31) {
        f->name[i] = name[i];
        i++;
    }

    while (i < 32)
        f->name[i++] = 0;

    i = 0;

    while (content[i] && i < 127) {
        f->content[i] = content[i];
        i++;
    }

    while (i < 128)
        f->content[i++] = 0;
}

void rt(char* name) {
    name = skip(name);
    trim(name);

    if (name[0] == 0) {
        print("\nerror: Invalid file name");
        return;
    }

    if (current->file_count > MAX_FILES) {
        print("\nerror: Filesystem corrupted");
        current->file_count = 0;
        return;
    }

    for (int i = 0; i < current->file_count; i++) {

        if (streq(current->files[i].name, name)) {

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

    if (streq(name, "..")) {
        if (current->parent)
            current = current->parent;
        return;
    }

    if (current->folder_count > MAX_FOLDERS) {
        print("\nerror: Filesystem corrupted");
        current->folder_count = 0;
        return;
    }

    for (int i = 0; i < current->folder_count; i++) {

        Folder* f = current->folders[i];

        if (!f)
            continue;

        if (streq(f->name, name)) {
            current = f;
            return;
        }
    }

    print("\nerror: Folder not found");
}

void fl(char* name) {
    name = skip(name);
    trim(name);

    if (name[0] == 0) {
        print("\nerror: Invalid file name");
        return;
    }

    if (current->file_count > MAX_FILES) {
        print("\nerror: Filesystem corrupted");
        current->file_count = 0;
        return;
    }

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

    if (!current) {
        print("\nerror: Filesystem corrupted");
        current = &root;
        return;
    }

    if (current->folder_count > MAX_FOLDERS) {
        print("\nerror: Folder list corrupted");
        current->folder_count = 0;
        return;
    }

    if (current->file_count > MAX_FILES) {
        print("\nerror: File list corrupted");
        current->file_count = 0;
        return;
    }

    print("\n");

    if (current->folder_count == 0 && current->file_count == 0) {
        print("(empty)\n");
        return;
    }

    for (int i = 0; i < current->folder_count; i++) {

        Folder* f = current->folders[i];

        if (!f)
            continue;

        print("[DIR] ");
        print(f->name);
        print("\n");
    }

    for (int i = 0; i < current->file_count; i++) {
        print("[FILE] ");
        print(current->files[i].name);
        print("\n");
    }
}

void pwd() {
    if (!current) {
        print("\nerror: Filesystem corrupted");
        return;
    }

    if (current == &root) {
        print("\n/");
        return;
    }

    char temp[256];
    int pos = 255;
    temp[pos] = 0;

    Folder* f = current;

    while (f && f != &root) {

        int len = 0;
        while (f->name[len]) len++;

        if (len == 0 || pos - len - 1 < 0)
            break;

        pos -= len;
        for (int i = 0; i < len; i++)
            temp[pos + i] = f->name[i];

        pos--;
        temp[pos] = '/';

        f = f->parent;
    }

    print("\n");
    print(&temp[pos]);
}

void mv(char* src, char* dst) {
    src = skip(src);
    dst = skip(dst);
    trim(src);
    trim(dst);

    if (!src[0] || !dst[0]) {
        print("\nerror: Invalid arguments");
        return;
    }

    if (streq(src, dst)) {
        print("\nerror: Same name");
        return;
    }

    // file collision check FIRST
    for (int i = 0; i < current->file_count; i++) {
        if (streq(current->files[i].name, dst)) {
            print("\nerror: Name exists");
            return;
        }
    }

    // =========================
    // MOVE INTO FOLDER
    // =========================
    for (int i = 0; i < current->folder_count; i++) {
        Folder* target = current->folders[i];

        if (!target) continue;

        if (streq(target->name, dst)) {

            // find source folder
            for (int j = 0; j < current->folder_count; j++) {
                Folder* f = current->folders[j];

                if (f && streq(f->name, src)) {

                    // move
                    f->parent = target;
                    target->folders[target->folder_count++] = f;

                    // remove from current
                    current->folders[j] =
                        current->folders[current->folder_count - 1];

                    current->folder_count--;

                    return;
                }
            }

            print("\nerror: Not found");
            return;
        }
    }

    // =========================
    // RENAME FILE
    // =========================
    for (int i = 0; i < current->file_count; i++) {
        if (streq(current->files[i].name, src)) {

            int k = 0;
            while (dst[k] && k < 31) {
                current->files[i].name[k] = dst[k];
                k++;
            }
            current->files[i].name[k] = 0;

            return;
        }
    }

    // =========================
    // RENAME FOLDER
    // =========================
    for (int i = 0; i < current->folder_count; i++) {
        Folder* f = current->folders[i];

        if (f && streq(f->name, src)) {

            int k = 0;
            while (dst[k] && k < 31) {
                f->name[k] = dst[k];
                k++;
            }
            f->name[k] = 0;

            return;
        }
    }

    print("\nerror: Not found");
}

void cp(char* src, char* dst) {
    src = skip(src);
    dst = skip(dst);
    trim(src);
    trim(dst);

    if (!src[0] || !dst[0]) {
        print("\nerror: Invalid arguments");
        return;
    }

    if (streq(src, dst)) {
        print("\nerror: Same name");
        return;
    }

    // block file name collision
    for (int i = 0; i < current->file_count; i++) {
        if (streq(current->files[i].name, dst)) {
            print("\nerror: Name exists");
            return;
        }
    }

    // =========================
    // COPY INTO FOLDER
    // =========================
    for (int i = 0; i < current->folder_count; i++) {
        Folder* target = current->folders[i];

        if (target && streq(target->name, dst)) {

            // ---- copy file into folder ----
            for (int j = 0; j < current->file_count; j++) {
                if (streq(current->files[j].name, src)) {

                    if (target->file_count >= MAX_FILES) {
                        print("\nerror: Target folder full");
                        return;
                    }

                    File* nf = &target->files[target->file_count++];

                    int k = 0;
                    while (current->files[j].name[k] && k < 31) {
                        nf->name[k] = current->files[j].name[k];
                        k++;
                    }
                    nf->name[k] = 0;

                    for (int c = 0; c < 128; c++) {
                        nf->content[c] = current->files[j].content[c];
                    }

                    return;
                }
            }

            print("\nerror: Not found");
            return;
        }
    }

    // =========================
    // COPY FILE
    // =========================
    for (int i = 0; i < current->file_count; i++) {
        if (streq(current->files[i].name, src)) {

            if (current->file_count >= MAX_FILES) {
                print("\nerror: File limit reached");
                return;
            }

            File* nf = &current->files[current->file_count++];

            int k = 0;
            while (dst[k] && k < 31) {
                nf->name[k] = dst[k];
                k++;
            }
            nf->name[k] = 0;

            for (int c = 0; c < 128; c++) {
                nf->content[c] = current->files[i].content[c];
            }

            return;
        }
    }

    // =========================
    // COPY FOLDER
    // =========================
    for (int i = 0; i < current->folder_count; i++) {
        Folder* f = current->folders[i];

        if (f && streq(f->name, src)) {

            if (folder_used >= MAX_FOLDERS) {
                print("\nerror: Folder limit reached");
                return;
            }

            Folder* nf = &folder_pool[folder_used++];

            nf->parent = current;
            nf->folder_count = 0;
            nf->file_count = f->file_count;

            int k = 0;
            while (dst[k] && k < 31) {
                nf->name[k] = dst[k];
                k++;
            }
            nf->name[k] = 0;

            // copy files safely
            for (int x = 0; x < f->file_count; x++) {
                nf->files[x] = f->files[x];
            }

            current->folders[current->folder_count++] = nf;

            return;
        }
    }

    print("\nerror: Not found");
}

// ================= SHELL =================
static int starts_with(const char* s, const char* cmd) {
    int i = 0;
    while (cmd[i]) {
        if (s[i] != cmd[i]) return 0;
        i++;
    }
    return 1;
}

static char* get_args(char* cmd) {
    while (*cmd && *cmd != ' ') cmd++;
    return skip(cmd);
}

static char* next_arg(char* s, char* out) {
    s = skip(s);

    int i = 0;
    while (s[i] && s[i] != ' ' && i < 31) {
        out[i] = s[i];
        i++;
    }

    out[i] = 0;

    while (*s && *s != ' ') s++;

    return s;
}

void execute(char* cmd) {
    cmd = skip(cmd);
    clean(cmd);

    if (starts_with(cmd, "ls")) ls();
    else if (starts_with(cmd, "cd")) cd(get_args(cmd));
    else if (starts_with(cmd, "cf")) cf(get_args(cmd));
    else if (starts_with(cmd, "rf")) rf(get_args(cmd));
    else if (starts_with(cmd, "rt")) rt(get_args(cmd));
    else if (starts_with(cmd, "fl") || starts_with(cmd, "lf")) fl(get_args(cmd));

    else if (starts_with(cmd, "ct")) {
        char* p = get_args(cmd);
        char* eq = p;

        while (*eq && *eq != '=') eq++;

        if (*eq == 0) {
            print("\nerror: Invalid ct format");
            return;
        }

        *eq = 0;
        ct(p, eq + 1);
    }

    else if (starts_with(cmd, "echo")) {
        echo(cmd + 4);
    }

    else if (starts_with(cmd, "cl")) {
        clear();
    }

    // ================= FIXED MV =================
    else if (starts_with(cmd, "mv")) {
        char* args = get_args(cmd);

        char a[32], b[32];
        args = next_arg(args, a);
        next_arg(args, b);

        if (!a[0] || !b[0]) {
            print("\nerror: Invalid arguments");
            return;
        }

        mv(a, b);
    }

    // ================= FIXED CP =================
    else if (starts_with(cmd, "cp")) {
        char* args = get_args(cmd);

        char a[32], b[32];
        args = next_arg(args, a);
        next_arg(args, b);

        if (!a[0] || !b[0]) {
            print("\nerror: Invalid arguments");
            return;
        }

        cp(a, b);
    }

    else if (starts_with(cmd, "pwd")) {
        pwd();
    }

    else {
        print("\nerror: Unknown command");
    }
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
                putc(c);
            }
        }

        buffer[i] = 0;
        clean(buffer);

        execute(buffer);
    }
}

// ================= KERNEL =================
void kernel_main() {
    enable_block_cursor();
    clear();
    init_fs();
    shell();
}
