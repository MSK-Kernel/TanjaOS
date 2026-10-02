# ports/doom/Makefile - build Doom (doomgeneric) as a TanjaOS ELF32 program.
#
# Normally driven from the top-level Makefile ('make doom'), which passes
# CC/LD/ARCH_FLAGS so the arm64 cross-build (Makefile.arm64) keeps working.
#
# Outputs (in build/):
#   doom.elf   one ET_REL ELF32 object (all engine + platform code merged)
#   doom.wad   the IWAD, stripped of sound/music lumps (TanjaOS has no audio)
#   doom_elf.o / doom_wad.o   the two blobs wrapped as linkable objects; the
#              kernel (kernel/doom.c) picks them up via _binary_doom_*_start.

CC         ?= gcc
LD         ?= ld
ARCH_FLAGS ?= -m32

# Which IWAD to embed.  The default is Freedoom Phase 1 (BSD-licensed), stripped
# of audio and xz-compressed in wad/ so a plain `make` needs no download.  For the
# shareware/commercial game use e.g.
#   make doom DOOM_WAD_SRC=/path/doom1.wad DOOM_IWAD_NAME=doom1.wad
DOOM_WAD_SRC   ?= wad/freedoom1.stripped.wad.xz
DOOM_IWAD_NAME ?= freedoom1.wad

GCC_INC := $(shell $(CC) $(ARCH_FLAGS) -print-file-name=include)

CFLAGS_DOOM = $(ARCH_FLAGS) -Os -ffreestanding -nostdinc -isystem include -isystem $(GCC_INC) \
              -Iengine -I../../include \
              -fno-stack-protector -fno-builtin -fno-pic -fno-pie -fno-common \
              -fno-asynchronous-unwind-tables -w \
              -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX -fomit-frame-pointer \
              -DTANJA_IWAD_NAME='"$(DOOM_IWAD_NAME)"'

ENGINE_SRC = $(wildcard engine/*.c)
OBJS = $(ENGINE_SRC:engine/%.c=build/%.o) build/tanja_libc.o build/tanja_platform.o

all: build/doom_elf.o build/doom_wad.o

build:
	mkdir -p build

build/%.o: engine/%.c | build
	$(CC) $(CFLAGS_DOOM) -c $< -o $@

build/tanja_libc.o: tanja_libc.c | build
	$(CC) $(CFLAGS_DOOM) -c $< -o $@

build/tanja_platform.o: tanja_platform.c | build
	$(CC) $(CFLAGS_DOOM) -c $< -o $@

build/doom.elf: $(OBJS)
	$(LD) -r -m elf_i386 $(OBJS) -o $@

build/doom.wad: $(DOOM_WAD_SRC) | build
	@case "$<" in *.xz) xz -dc "$<" > $@ ;; *) cp "$<" $@ ;; esac

# `ld -r -b binary` names the symbols after the input path, so run it from
# inside build/ to get the fixed names _binary_doom_elf_* / _binary_doom_wad_*.
build/doom_elf.o: build/doom.elf
	cd build && $(LD) -r -b binary -m elf_i386 -o doom_elf.o doom.elf

build/doom_wad.o: build/doom.wad
	cd build && $(LD) -r -b binary -m elf_i386 -o doom_wad.o doom.wad


clean:
	rm -rf build

.PHONY: all clean
