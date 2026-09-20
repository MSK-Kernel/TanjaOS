/* sys/stat.h */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
struct stat {
    off_t st_size;
    mode_t st_mode;
    time_t st_mtime;
    time_t st_atime;
    time_t st_ctime;
};
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_ISREG(m) (((m) & 0170000) == S_IFREG)
#define S_ISDIR(m) (((m) & 0170000) == S_IFDIR)
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
#endif
