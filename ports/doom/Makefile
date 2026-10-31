# ports/doom/Makefile - build Doom (doomgeneric) as a TanjaOS ELF32 program.
#
# Built by the top-level default 'make' target; 'make clean' removes build outputs.
#
# Outputs (in build/):
#   doom.elf   one ET_REL ELF32 object (all engine + platform code merged)
#   doom.wad   the IWAD, stripped of sound/music lumps (TanjaOS has no audio)
#   doom          standalone ET_REL package installed as /usr/bin/doom.o

CC         ?= gcc
LD         ?= ld
ARCH_FLAGS ?= -m32

# Which IWAD to embed.  The default is Freedoom Phase 1 (BSD-licensed), stripped
# of audio and xz-compressed in wad/ so a plain `make` needs no download.  For the
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

all: build/doom

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

# Combine the real relocatable program sections and IWAD into one ET_REL
# applet. Do not wrap the engine object in a binary blob: elf_run needs main.
build/doom: build/doom.elf build/doom_wad.o
	$(LD) -r -m elf_i386 -o $@ build/doom.elf build/doom_wad.o


clean:
	rm -rf build

.PHONY: all clean
