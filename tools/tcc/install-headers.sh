#!/bin/sh
# install-headers.sh - assemble the in-OS C headers into home/include/.
#
# TanjaOS ships a real, standard C header set at /include so that
# `tcc` inside the OS compiles programs the way a hosted toolchain
# would.  ONLY standard header names are shipped (no tanja.h):
#   - freestanding headers from the TinyCC distribution
#     (stddef.h, stdarg.h, stdbool.h, float.h, tccdefs.h)
#     plus a hand-written stdint.h
#   - the standard hosted headers in std-include/ (stdio.h,
#     stdlib.h, string.h, unistd.h, time.h, ...).  These declare
#     the C-standard surface plus the TanjaOS kernel exports
#     (console, keyboard, filesystem, environment) so user programs
#     need nothing outside the normal C headers.
#
# (Re-running overwrites; to start clean, delete home/include first.)
set -e
cd "$(dirname "$0")"
DEST=../../home/include

rm -rf "$DEST"
mkdir -p "$DEST/sys"

# 1. freestanding headers from the TinyCC distribution
for h in stddef.h stdarg.h stdbool.h float.h tccdefs.h; do
    cp "tinycc/include/$h" "$DEST/$h"
done

cat > "$DEST/stdint.h" <<'STDINTEOF'
/* Minimal freestanding stdint.h for TanjaOS (i386, ILP32). */
#ifndef _TANJA_STDINT_H
#define _TANJA_STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;

typedef signed char        int_least8_t;
typedef unsigned char      uint_least8_t;
typedef short              int_least16_t;
typedef unsigned short     uint_least16_t;
typedef int                int_least32_t;
typedef unsigned int       uint_least32_t;
typedef long long          int_least64_t;
typedef unsigned long long uint_least64_t;

typedef signed char        int_fast8_t;
typedef unsigned char      uint_fast8_t;
typedef int                int_fast16_t;
typedef unsigned int       uint_fast16_t;
typedef int                int_fast32_t;
typedef unsigned int       uint_fast32_t;
typedef long long          int_fast64_t;
typedef unsigned long long uint_fast64_t;

typedef __INTPTR_TYPE__     intptr_t;   /* matches stddef.h */
typedef __UINTPTR_TYPE__    uintptr_t;
typedef long long          intmax_t;
typedef unsigned long long uintmax_t;

#define INT8_MIN    (-128)
#define INT16_MIN   (-32768)
#define INT32_MIN   (-2147483647 - 1)
#define INT8_MAX    127
#define INT16_MAX   32767
#define INT32_MAX   2147483647
#define UINT8_MAX   255
#define UINT16_MAX  65535
#define UINT32_MAX  4294967295U
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL
#define UINT64_MAX  18446744073709551615ULL
#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#define PTRDIFF_MIN INT32_MIN
#define PTRDIFF_MAX INT32_MAX
#define SIZE_MAX    UINT32_MAX

#define INT8_C(v)   v
#define INT16_C(v)  v
#define INT32_C(v)  v
#define INT64_C(v)  v ## LL
#define UINT8_C(v)  v
#define UINT16_C(v) v
#define UINT32_C(v) v ## U
#define UINT64_C(v) v ## ULL
#define INTMAX_C(v) v ## LL
#define UINTMAX_C(v) v ## ULL

#endif /* _TANJA_STDINT_H */
STDINTEOF

# 2. the standard hosted headers (stdlib surface + TanjaOS exports)
cp std-include/*.h "$DEST/"
cp std-include/sys/*.h "$DEST/sys/"

echo "[HEADERS] installed into home/include (shipped as /include):"
find "$DEST" -type f | sort
