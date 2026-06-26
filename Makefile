CC=gcc
LD=ld
ASM=nasm

CFLAGS=-ffreestanding -m32 -c
LDFLAGS=-m elf_i386 -T linker/linker.ld

all: i386

i386:
	mkdir -p output/build
	$(ASM) -f elf32 boot/boot.asm -o output/build/boot.o
	$(CC) $(CFLAGS) kernel/kernel.c -o output/build/kernel.o
	mkdir -p output/image
	$(LD) $(LDFLAGS) -o output/image/msk-i386 output/build/boot.o output/build/kernel.o

clean:
	rm -rf output/* msk-i386
