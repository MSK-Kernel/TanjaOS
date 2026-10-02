/* sys/types.h */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stdint.h>
typedef int32_t  ssize_t;
typedef int32_t  off_t;
typedef int32_t  pid_t;
typedef uint32_t mode_t;
#ifndef _TANJA_TIME_T_DEFINED
#define _TANJA_TIME_T_DEFINED
typedef uint32_t time_t;
#endif
#endif
