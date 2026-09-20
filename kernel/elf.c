#include <stdint.h>
#include <stddef.h>
#include "../include/fs.h"

/* ---- console (for error reporting) ------------------------- */
extern void print(const char* s);
extern int printf(const char* fmt, ...);
extern char* itoa(int value, char* buf, int base);
extern void print_n(const char* s, uint32_t len);

/* ---- kernel exports available to programs ------------------- */
extern void print_color(const char* s, uint16_t color);
extern void putc(char c);
extern void putc_color(char c, uint16_t color);
extern int putchar(int c);
extern int getchar(void);
extern int puts(const char* s);
extern void clear_screen(void);
extern void print_dec(uint32_t n);
extern void print_dec_pad(uint32_t n, int width);
extern void print_hex(uint32_t n);
extern uint16_t* VGA;
extern int cursor;
extern void sync_cursor(void);
extern int get_key(void);
extern int key_available(void);
extern void read_line(char* buf, int max_len);
extern int strlen(const char* s);
extern int strcmp(const char* a, const char* b);
extern int strncmp(const char* a, const char* b, unsigned int n);
extern char* strcpy(char* dst, const char* src);
extern char* strncpy(char* dst, const char* src, unsigned int n);
extern char* strcat(char* dst, const char* src);
extern char* strchr(const char* s, int c);
extern char* strrchr(const char* s, int c);
extern char* strstr(const char* haystack, const char* needle);
extern int strcasecmp(const char* a, const char* b);
extern int strncasecmp(const char* a, const char* b, unsigned int n);
extern char* strlwr(char* s);
extern char* strupr(char* s);
extern void* memset(void* dst, int value, unsigned int n);
extern void* memcpy(void* dst, const void* src, unsigned int n);
extern void* memmove(void* dst, const void* src, unsigned int n);
extern int memcmp(const void* a, const void* b, unsigned int n);
extern int streq(const char* a, const char* b);
extern int atoi(const char* s);
extern int abs(int n);
extern int rand(void);
extern void srand(unsigned int seed);
extern int isdigit(int c);
extern int isalpha(int c);
extern int isalnum(int c);
extern int islower(int c);
extern int isupper(int c);
extern int isspace(int c);
extern int isxdigit(int c);
extern int tolower(int c);
extern int toupper(int c);
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern void outw(uint16_t port, uint16_t val);
extern uint32_t get_uptime_ms(void);
extern void timer_delay_ms(uint32_t ms);
extern void execute_command(const char* cmd_line);
extern int exec_file(const char* path, const char* args);
extern void register_cmd(const char* name, void (*func)(char* args));
extern int cmd_exists(const char* name);
extern void list_commands(void);
extern int  builtin_names(char* out, int cap);
extern void set_env(const char* name, const char* value);
extern const char* get_env(const char* name);
extern void clean(char* s);
extern void store_save(void);
extern void store_autosave(void);
extern int store_is_persistent(void);
extern void config_reset(void);
extern void setup_wizard(void);

struct tanja_export {
    const char* name;
    void* addr;
};

/* The program-visible API: anything include/tanja.h declares must
 * appear here, or programs using it will fail to load with an
 * "undefined symbol" error. */
static const struct tanja_export tanja_exports[] = {
    { "print", (void*)print },
    { "printf", (void*)printf },
    { "itoa", (void*)itoa },
    { "print_n", (void*)print_n },
    { "print_color", (void*)print_color },
    { "putc", (void*)putc },
    { "putc_color", (void*)putc_color },
    { "putchar", (void*)putchar },
    { "getchar", (void*)getchar },
    { "puts", (void*)puts },
    { "clear_screen", (void*)clear_screen },
    { "print_dec", (void*)print_dec },
    { "print_dec_pad", (void*)print_dec_pad },
    { "print_hex", (void*)print_hex },
    { "VGA", (void*)&VGA },
    { "cursor", (void*)&cursor },
    { "sync_cursor", (void*)sync_cursor },
    { "get_key", (void*)get_key },
    { "key_available", (void*)key_available },
    { "read_line", (void*)read_line },
    { "strlen", (void*)strlen },
    { "strcmp", (void*)strcmp },
    { "strncmp", (void*)strncmp },
    { "strcpy", (void*)strcpy },
    { "strncpy", (void*)strncpy },
    { "strcat", (void*)strcat },
    { "strchr", (void*)strchr },
    { "strrchr", (void*)strrchr },
    { "strstr", (void*)strstr },
    { "strcasecmp", (void*)strcasecmp },
    { "strncasecmp", (void*)strncasecmp },
    { "strlwr", (void*)strlwr },
    { "strupr", (void*)strupr },
    { "memset", (void*)memset },
    { "memcpy", (void*)memcpy },
    { "memmove", (void*)memmove },
    { "memcmp", (void*)memcmp },
    { "streq", (void*)streq },
    { "atoi", (void*)atoi },
    { "abs", (void*)abs },
    { "rand", (void*)rand },
    { "srand", (void*)srand },
    { "isdigit", (void*)isdigit },
    { "isalpha", (void*)isalpha },
    { "isalnum", (void*)isalnum },
    { "islower", (void*)islower },
    { "isupper", (void*)isupper },
    { "isspace", (void*)isspace },
    { "isxdigit", (void*)isxdigit },
    { "tolower", (void*)tolower },
    { "toupper", (void*)toupper },
    { "inb", (void*)inb },
    { "outb", (void*)outb },
    { "outw", (void*)outw },
    { "get_uptime_ms", (void*)get_uptime_ms },
    { "timer_delay_ms", (void*)timer_delay_ms },
    { "execute_command", (void*)execute_command },
    { "exec_file", (void*)exec_file },
    { "register_cmd", (void*)register_cmd },
    { "cmd_exists", (void*)cmd_exists },
    { "list_commands", (void*)list_commands },
    { "builtin_names", (void*)builtin_names },
    { "set_env", (void*)set_env },
    { "get_env", (void*)get_env },
    { "clean", (void*)clean },
    { "store_save", (void*)store_save },
    { "store_autosave", (void*)store_autosave },
    { "store_is_persistent", (void*)store_is_persistent },
    { "fs_init", (void*)fs_init },
    { "fs_seed_home", (void*)fs_seed_home },
    { "fs_create_file", (void*)fs_create_file },
    { "fs_create_directory", (void*)fs_create_directory },
    { "fs_delete_file", (void*)fs_delete_file },
    { "fs_delete_directory", (void*)fs_delete_directory },
    { "fs_write_file", (void*)fs_write_file },
    { "fs_read_file", (void*)fs_read_file },
    { "fs_read_file_prefix", (void*)fs_read_file_prefix },
    { "fs_read_file_range", (void*)fs_read_file_range },
    { "fs_get_file_size", (void*)fs_get_file_size },
    { "fs_file_exists", (void*)fs_file_exists },
    { "fs_directory_exists", (void*)fs_directory_exists },
    { "fs_list_directory", (void*)fs_list_directory },
    { "fs_change_directory", (void*)fs_change_directory },
    { "fs_get_current_path", (void*)fs_get_current_path },
    { "parse_path", (void*)parse_path },
    { "find_in_directory", (void*)find_in_directory },
    { "fs_seed_home", (void*)fs_seed_home },
    { "config_reset", (void*)config_reset },
    { "setup_wizard", (void*)setup_wizard },
};

/* ---- ELF32 definitions ------------------------------------- */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

#define ET_REL         1
#define EM_386         3
#define ELFDATA2LSB   1
#define ELFCLASS32    1

#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_RELA       4
#define SHT_NOBITS     8
#define SHT_REL        9

#define SHF_ALLOC      0x2

#define SHN_UNDEF      0
#define SHN_ABS        0xFFF1
#define SHN_COMMON     0xFFF2

#define R_386_32       1
#define R_386_PC32     2

/* ---- program region ----------------------------------------- */
/* Real ELF executables on x86-32 traditionally load at 0x40000000
 * (1 GiB).  Nothing else in the kernel lives there, so programs get
 * the whole 16 MiB window.  Loading is stack-disciplined: nested
 * loads (a program running execute_command() which loads another
 * program) each reclaim their own memory on return. */
#define PROGRAM_BASE   0x40000000u
#define PROGRAM_LIMIT  (PROGRAM_BASE + 0x1000000u)

static uint8_t* program_brk = (uint8_t*)PROGRAM_BASE;

static void* tanja_export_lookup(const char* name)
{
    int i;
    for (i = 0; i < (int)(sizeof(tanja_exports) / sizeof(tanja_exports[0])); i++)
        if (strcmp(tanja_exports[i].name, name) == 0)
            return tanja_exports[i].addr;
    return 0;
}

int elf_is_elf32(const uint8_t* image, uint32_t size)
{
    return image && size >= 5 &&
           image[0] == 0x7F && image[1] == 'E' &&
           image[2] == 'L' && image[3] == 'F';
}

/* Round p up to the next multiple of a (a must be a power of two,
 * or 0/1 for "no alignment requirement"). */
static uint32_t align_up(uint32_t p, uint32_t a)
{
    if (a <= 1) return p;
    return (p + a - 1) & ~(a - 1);
}

int elf_run(const uint8_t* image, uint32_t size, const char* args)
{
    const Elf32_Ehdr* eh;
    const Elf32_Shdr* sh;
    uint32_t sec_addr[96];      /* load address of each section (0 = none) */
    uint32_t sec_size[96];
    int nsec, i;

    if (!elf_is_elf32(image, size)) {
        print("elf: not an ELF file\n");
        return -1;
    }
    eh = (const Elf32_Ehdr*)image;

    if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB) {
        print("elf: only little-endian ELF32 is supported\n");
        return -1;
    }
    if (eh->e_type != ET_REL) {
        print("elf: only relocatable objects (ET_REL) are supported -\n");
        print("elf: build programs with tcc inside TanjaOS\n");
        return -1;
    }
    if (eh->e_machine != EM_386) {
        print("elf: wrong machine type (need i386)\n");
        return -1;
    }
    if (eh->e_shentsize != sizeof(Elf32_Shdr) || eh->e_shoff == 0 ||
        eh->e_shnum == 0) {
        print("elf: missing section table\n");
        return -1;
    }
    nsec = eh->e_shnum;
    if (nsec > 96) {
        print("elf: too many sections\n");
        return -1;
    }
    if (eh->e_shoff + (uint32_t)nsec * sizeof(Elf32_Shdr) > size) {
        print("elf: section table out of bounds\n");
        print("elf: shoff=");
        print_hex(eh->e_shoff);
        print(" nsec=");
        print_dec(nsec);
        print(" size=");
        print_dec(size);
        print("\n");
        return -1;
    }
    sh = (const Elf32_Shdr*)(image + eh->e_shoff);

    /* ---- pass 1: place every allocatable section in the region ---- */
    uint8_t* save_brk = program_brk;
    uint32_t cur = (uint32_t)program_brk;

    for (i = 0; i < nsec; i++) {
        sec_addr[i] = 0;
        sec_size[i] = sh[i].sh_size;

        if (!(sh[i].sh_flags & SHF_ALLOC))
            continue;

        cur = align_up(cur, sh[i].sh_addralign ? sh[i].sh_addralign : 1);

        if (sh[i].sh_type == SHT_NOBITS) {
            /* .bss: zero the space (the region is plain RAM; nothing
             * guarantees it was ever zeroed for us). */
            uint32_t k;
            for (k = 0; k < sh[i].sh_size; k += 4) {
                /* word-wise when possible, byte tail */
                if (k + 4 <= sh[i].sh_size)
                    *(uint32_t*)(cur + k) = 0;
                else
                    *(uint8_t*)(cur + k) = 0;
            }
        } else {
            /* Contents come from the file.  Zero-size PROGBITS
             * sections (empty .data etc) are legal - skip them. */
            if (sh[i].sh_size > 0) {
                if (sh[i].sh_offset + sh[i].sh_size > size) {
                    print("elf: section contents out of bounds\n");
                    program_brk = save_brk;
                    return -1;
                }
                memcpy((void*)cur, image + sh[i].sh_offset, sh[i].sh_size);
            }
        }

        sec_addr[i] = cur;
        cur += sh[i].sh_size;
    }

    if (cur > PROGRAM_LIMIT) {
        print("elf: program region exhausted\n");
        program_brk = save_brk;
        return -1;
    }
    program_brk = (uint8_t*)cur;

    /* ---- find the symbol table and its string table ---- */
    const Elf32_Sym* symtab = 0;
    uint32_t nsyms = 0;
    const char* symstr = 0;

    for (i = 0; i < nsec; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = (const Elf32_Sym*)(image + sh[i].sh_offset);
            nsyms = sh[i].sh_size / sizeof(Elf32_Sym);
            if (sh[i].sh_link < (uint32_t)nsec &&
                sh[sh[i].sh_link].sh_type == SHT_STRTAB)
                symstr = (const char*)(image + sh[sh[i].sh_link].sh_offset);
            break;
        }
    }
    if (!symtab || !symstr) {
        print("elf: no symbol table (stripped?)\n");
        program_brk = save_brk;
        return -1;
    }

    /* ---- pass 2: apply relocations ---- */
    uint32_t rel_ok = 1;
    for (i = 0; i < nsec && rel_ok; i++) {
        if (sh[i].sh_type != SHT_REL)
            continue;
        /* target section of this relocation group */
        uint32_t t = sh[i].sh_info;
        if (t >= (uint32_t)nsec || !sec_addr[t])
            continue;   /* relocs against a discarded/non-alloc section */

        const Elf32_Rel* rel = (const Elf32_Rel*)(image + sh[i].sh_offset);
        uint32_t nrel = sh[i].sh_size / sizeof(Elf32_Rel);

        for (uint32_t r = 0; r < nrel; r++) {
            uint32_t sym = rel[r].r_info >> 8;
            uint32_t type = rel[r].r_info & 0xFF;
            uint32_t place = sec_addr[t] + rel[r].r_offset;
            uint32_t S;
            int32_t  A;

            if (rel[r].r_offset >= sh[t].sh_size) {
                print("elf: relocation offset out of range\n");
                rel_ok = 0;
                break;
            }

            if (sym >= nsyms) {
                print("elf: relocation references a bad symbol index\n");
                rel_ok = 0;
                break;
            }

            if (symtab[sym].st_shndx == SHN_UNDEF) {
                /* the whole point: an undefined symbol is a kernel API call */
                const char* nm = symstr + symtab[sym].st_name;
                void* a = tanja_export_lookup(nm);
                if (!a) {
                    print("elf: undefined symbol: ");
                    print(nm);
                    print("\n");
                    rel_ok = 0;
                    break;
                }
                S = (uint32_t)(uintptr_t)a;
            } else if (symtab[sym].st_shndx == SHN_ABS) {
                S = symtab[sym].st_value;
            } else if (symtab[sym].st_shndx == SHN_COMMON) {
                /* -fno-common makes these impossible; refuse rather
                 * than silently misbinding */
                print("elf: COMMON symbols not supported "
                      "(recompile with -fno-common)\n");
                rel_ok = 0;
                break;
            } else if (symtab[sym].st_shndx < (uint32_t)nsec) {
                uint32_t base = sec_addr[symtab[sym].st_shndx];
                if (!base) {
                    print("elf: symbol references a non-allocated section\n");
                    rel_ok = 0;
                    break;
                }
                S = base + symtab[sym].st_value;
            } else {
                print("elf: unsupported symbol section index\n");
                rel_ok = 0;
                break;
            }

            switch (type) {
            case R_386_32:
                A = *(int32_t*)place;
                *(uint32_t*)place = (uint32_t)(S + A);
                break;
            case R_386_PC32:
                A = *(int32_t*)place;
                *(int32_t*)place = (int32_t)(S + A - place);
                break;
            default:
                print("elf: unsupported relocation type ");
                print_dec(type);
                print("\n");
                rel_ok = 0;
                break;
            }
            if (!rel_ok) break;
        }
    }

    /* ---- find the entry point ---- */
    uint32_t entry = 0;
    for (i = 0; i < (int)nsyms; i++) {
        const char* nm = symstr + symtab[i].st_name;
        if (nm[0] == 'm' && nm[1] == 'a' && nm[2] == 'i' && nm[3] == 'n' &&
            nm[4] == 0 && symtab[i].st_shndx != SHN_UNDEF) {
            uint32_t base = (symtab[i].st_shndx == SHN_ABS)
                                ? 0
                                : sec_addr[symtab[i].st_shndx];
            entry = base + symtab[i].st_value;
            break;
        }
    }

    if (!rel_ok || !entry) {
        if (rel_ok)
            print("elf: no 'main' symbol in program\n");
        program_brk = save_brk;
        return -1;
    }

    /* ---- run it ---- */
    ((void(*)(const char*))entry)(args ? args : "");

    /* Stack discipline: the program's memory is dead once it returns
     * (all persistent state lives in the filesystem), so nested
     * program loads can reuse the same region. */
    program_brk = save_brk;
    return 0;
}
