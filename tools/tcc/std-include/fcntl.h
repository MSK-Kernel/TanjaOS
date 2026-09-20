/* fcntl.h - file control options (used by tcc's libc shim open()). */
#ifndef _FCNTL_H
#define _FCNTL_H
#define O_RDONLY   00000000
#define O_WRONLY   00000001
#define O_RDWR     00000002
#define O_CREAT    00000100
#define O_TRUNC    00001000
#define O_APPEND   00002000
#define O_BINARY   00000000
#endif
