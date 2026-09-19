#!/bin/sh
# ============================================================
# build-tcc.sh - build TinyCC for TanjaOS as ONE freestanding
# i386 relocatable object (tools/tcc/tcc.o -> home/bin/tcc.o).
#
# The kernel ELF loader (kernel/elf.c) loads it like any other
# /bin command and resolves its undefined symbols against the
# kernel export table; everything else (libc shim, heap, exit)
# is self-contained inside the object (tanja-libc.c +
# tanja-tcc-main.c).
#
# tcc's own main() is renamed to tcc_main_real (-Dmain=...) and
# tanja-tcc-main.c provides the TanjaOS `void main(char*)` entry.
#
# Requirements: gcc -m32 support (gcc-multilib), binutils (ld, nm).
set -e
cd "$(dirname "$0")"

SRC=tinycc
CFLAGS="-m32 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie \
 -fno-stack-protector -fno-strict-aliasing -Wno-unused-result -Wno-declaration-after-statement \
 -Os -g0 -I tanja-libc-include -I $SRC -DTCC_TARGET_I386 \
 -DCONFIG_TCC_SYSINCLUDEPATHS='\"/include\"'"

# tcc.c is the all-in-one build (ONE_SOURCE): it #includes libtcc.c ->
# tccpp/tccgen/tccelf/tccdbg/tccasm/tccrun/i386-* and tcctools.c.
# So we compile tcc.c alone, plus our libc shim and the entry wrapper.
compile() {
    src="$1"; extra="$2"
    o="build/$(echo "$src" | tr '/' '_')"
    o="${o%.c}.o"
    echo "[CC] $src $extra"
    eval i686-linux-gnu-gcc $CFLAGS $extra -c "$src" -o "$o"
    OBJS="$OBJS $o"
}

rm -f build/*.o
mkdir -p build
OBJS=""

compile "$SRC/tcc.c" "-Dmain=tcc_main_real"
compile "tanja-libc.c" ""
compile "tanja-tcc-main.c" ""

echo "[LD -r] merging -> tcc.o"
i686-linux-gnu-ld -m elf_i386 -r -o tcc.o $OBJS

echo "[SIZE]"
size tcc.o | tail -1
ls -la tcc.o

echo "[UNDEFINED SYMBOLS] (must all be kernel exports)"
nm -u tcc.o | sed 's/ *U //' | sort -u > build/undefined.txt
cat build/undefined.txt

echo "[INSTALL] -> ../../home/bin/tcc.o (embedded as /bin/tcc.o)"
mkdir -p ../../home/bin
cp tcc.o ../../home/bin/tcc.o
