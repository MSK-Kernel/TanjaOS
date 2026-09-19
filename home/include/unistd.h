/* unistd.h - system interface for TanjaOS (i386).
 *
 * open/read/write/... are provided by tcc's in-OS libc shim; the
 * TanjaOS filesystem API (fs_*), hardware access and persistence
 * helpers are kernel exports and link in user programs. */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* --- tcc libc shim (not kernel exports) --- */
int    open(const char *path, int flags, ...);
int    close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t  lseek(int fd, off_t offset, int whence);
int    unlink(const char *path);
int    remove(const char *path);
int    rename(const char *oldpath, const char *newpath);
int    execv(const char *path, char *const argv[]);
int    execvp(const char *file, char *const argv[]);
char  *getcwd(char *buf, size_t size);
char  *realpath(const char *path, char *resolved_path);

/* --- hardware port access (kernel exports) --- */
uint8_t  inb(uint16_t port);
void     outb(uint16_t port, uint8_t val);
void     outw(uint16_t port, uint16_t val);

/* =================================================================
 * TanjaOS filesystem API (kernel exports)
 * ================================================================= */
#define MAX_PATH     256
#define MAX_FILENAME 256
/* Public max file size; kept in sync with MAX_FILE_DATA in fs/fs.c. */
#define MAX_FILE_SIZE 327680

typedef enum {
    FS_FILE,
    FS_DIRECTORY
} fs_type_t;

int fs_create_file(const char* path);
int fs_create_directory(const char* path);
int fs_delete_file(const char* path);
int fs_delete_directory(const char* path);
int fs_write_file(const char* path, const char* data, uint32_t size);
int fs_read_file(const char* path, char* buffer, uint32_t* size);
int fs_read_file_prefix(const char* path, char* buffer,
                        uint32_t capacity, uint32_t* size);
int fs_read_file_range(const char* path, uint32_t offset, char* buffer,
                       uint32_t capacity, uint32_t* size);
uint32_t fs_get_file_size(const char* path);
int fs_file_exists(const char* path);
int fs_directory_exists(const char* path);
int fs_list_directory(const char* path, char* buffer, uint32_t* size);
int fs_change_directory(const char* path);
void fs_get_current_path(char* path);
int parse_path(const char* path, char* parent_path, char* name);
int find_in_directory(int dir_index, const char* name, fs_type_t type);

/* --- persistence / system (kernel exports) --- */
void fs_init(void);
void fs_seed_home(void);
void store_save(void);
void store_autosave(void);
int  store_is_persistent(void);
void config_reset(void);
void setup_wizard(void);

#endif /* _UNISTD_H */
