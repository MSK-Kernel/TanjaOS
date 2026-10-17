/* errno.h - error codes (kernel libc sets these) */
#ifndef _ERRNO_H
#define _ERRNO_H
extern int errno;

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EMFILE  4
#define EINTR   4
#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EEXIST  17
#define EINVAL  22
#define ENOSPC  28
#define ESPIPE  29
#define EDOM    33
#define ERANGE  34

#endif
