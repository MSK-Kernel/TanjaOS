#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

#define MAX_PATH 256
#define MAX_FILENAME 64
#define MAX_FILE_SIZE 1024

typedef enum {
    FS_FILE,
    FS_DIRECTORY
} fs_type_t;

void fs_init(void);
int fs_create_file(const char* path);
int fs_create_directory(const char* path);
int fs_delete_file(const char* path);
int fs_delete_directory(const char* path);
int fs_write_file(const char* path, const char* data, uint32_t size);
int fs_read_file(const char* path, char* buffer, uint32_t* size);
int fs_file_exists(const char* path);
int fs_directory_exists(const char* path);
int fs_list_directory(const char* path, char* buffer, uint32_t* size);
int fs_change_directory(const char* path);
void fs_get_current_path(char* path);
int parse_path(const char* path, char* parent_path, char* name);
int find_in_directory(int dir_index, const char* name, fs_type_t type);

#endif
