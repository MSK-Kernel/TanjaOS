/* inttypes.h - printf/scanf format macros for i386 ILP32. */
#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>
#define PRId8  "hhd"  #define PRIi8  "hhi"  #define PRIo8  "hho"
#define PRIu8  "hhu"  #define PRIx8  "hhx"  #define PRIX8  "hhX"
#define PRId16 "hd"   #define PRIi16 "hi"   #define PRIo16 "ho"
#define PRIu16 "hu"   #define PRIx16 "hx"   #define PRIX16 "hX"
#define PRId32 "d"    #define PRIi32 "i"    #define PRIo32 "o"
#define PRIu32 "u"    #define PRIx32 "x"   #define PRIX32 "X"
#define PRId64 "lld"  #define PRIi64 "lli"  #define PRIo64 "llo"
#define PRIu64 "llu"  #define PRIx64 "llx"  #define PRIX64 "llX"
#define PRIdPTR "d"   #define PRIuPTR "u"   #define PRIxPTR "x"
#define SCNd32 "d"    #define SCNu32 "u"    #define SCNx32 "x"
#define SCNd64 "lld"  #define SCNu64 "llu"  #define SCNx64 "llx"
#endif
