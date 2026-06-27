CC=gcc
LD=ld
ASM=nasm

CFLAGS=-ffreestanding -m32 -c \
       -fno-stack-protector \
       -fno-builtin \
       -fno-pic \
       -fno-pie

LDFLAGS=-m elf_i386 -T linker/linker.ld -nostdlib

all: i386

i386: output/image/msk-i386

output/build:
	mkdir -p output/build

output/image:
	mkdir -p output/image

output/build/boot.o: boot/boot.asm | output/build
	$(ASM) -f elf32 boot/boot.asm -o output/build/boot.o

output/build/kernel.o: kernel/kernel.c | output/build
	$(CC) $(CFLAGS) kernel/kernel.c -o output/build/kernel.o

output/image/msk-i386: output/build/boot.o output/build/kernel.o | output/image
	$(LD) $(LDFLAGS) -o output/image/msk-i386 output/build/boot.o output/build/kernel.o

clean:
	rm -rf output
