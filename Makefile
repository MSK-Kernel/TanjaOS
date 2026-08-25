CC=gcc
LD=ld
ASM=nasm

CFLAGS=-ffreestanding -m32 -c -fno-stack-protector -fno-builtin -fno-pic -fno-pie -I.
LDFLAGS=-m elf_i386 -T linker/linker.ld -nostdlib

BIN_SRC = $(filter-out bin/init.c, $(wildcard bin/*.c))
BIN_OBJ = $(BIN_SRC:.c=.o)
BIN_NAMES = $(notdir $(basename $(BIN_SRC)))

KERNEL_OBJ = kernel/kernel.o kernel/game.o kernel/ata.o kernel/ahci.o kernel/store.o kernel/idt.o kernel/cc.o

all: arch/x86/boot/tanja-base

arch/x86/boot:
	mkdir -p arch/x86/boot

bin/init.c: $(BIN_SRC)
	@rm -f bin/init.c
	@echo "// Auto-generated" > bin/init.c
	@echo '#include "bin.h"' >> bin/init.c
	@echo '' >> bin/init.c
	@echo 'void init_cmds() {' >> bin/init.c
	@for name in $(BIN_NAMES); do \
		echo "    extern void cmd_$$name(char* args);" >> bin/init.c; \
		echo "    register_cmd(\"$$name\", cmd_$$name);" >> bin/init.c; \
	done
	@echo '}' >> bin/init.c


bin/%.o: bin/%.c
	@echo "[CC] bin/$(notdir $<)"
	$(CC) $(CFLAGS) -o $@ $<


kernel/kernel.o: kernel/kernel.c
	@echo "[CC] kernel/kernel.c"
	$(CC) $(CFLAGS) -o kernel/kernel.o kernel/kernel.c


kernel/game.o: kernel/game.c
	@echo "[CC] kernel/game.c"
	$(CC) $(CFLAGS) -o kernel/game.o kernel/game.c


kernel/ata.o: kernel/ata.c
	@echo "[CC] kernel/ata.c"
	$(CC) $(CFLAGS) -o kernel/ata.o kernel/ata.c


kernel/ahci.o: kernel/ahci.c
	@echo "[CC] kernel/ahci.c"
	$(CC) $(CFLAGS) -o kernel/ahci.o kernel/ahci.c


kernel/store.o: kernel/store.c
	@echo "[CC] kernel/store.c"
	$(CC) $(CFLAGS) -o kernel/store.o kernel/store.c


kernel/idt.o: kernel/idt.c
	@echo "[CC] kernel/idt.c"
	$(CC) $(CFLAGS) -o kernel/idt.o kernel/idt.c


kernel/cc.o: kernel/cc.c
	@echo "[CC] kernel/cc.c"
	$(CC) $(CFLAGS) -o kernel/cc.o kernel/cc.c


arch/x86/idt_asm.o: arch/x86/idt_asm.asm
	@echo "[ASM] idt_asm.asm"
	$(ASM) -f elf32 arch/x86/idt_asm.asm -o arch/x86/idt_asm.o


fs/fs.o: fs/fs.c
	@echo "[CC] fs/fs.c"
	$(CC) $(CFLAGS) -o fs/fs.o fs/fs.c


bin/init.o: bin/init.c
	@echo "[CC] bin/init.c"
	$(CC) $(CFLAGS) -o bin/init.o bin/init.c


arch/x86/boot/boot.o: arch/x86/boot/boot.asm | arch/x86/boot
	@echo "[ASM] boot.asm"
	$(ASM) -f elf32 arch/x86/boot/boot.asm -o arch/x86/boot/boot.o


arch/x86/boot/tanja-base: arch/x86/boot/boot.o arch/x86/idt_asm.o $(KERNEL_OBJ) fs/fs.o bin/init.o $(BIN_OBJ) | arch/x86/boot
	@echo "[LD] Linking..."
	$(LD) $(LDFLAGS) -o arch/x86/boot/tanja-base \
		arch/x86/boot/boot.o \
		arch/x86/idt_asm.o \
		$(KERNEL_OBJ) \
		fs/fs.o \
		bin/init.o \
		$(BIN_OBJ)

	@echo
	@echo "[INFO] Kernel image ready at arch/x86/boot/tanja-base"
	@echo "[INFO] Commands: $(BIN_NAMES)"
	@echo

clean:
	rm -f kernel/*.o fs/*.o
	rm -f bin/*.o bin/init.c
	rm -f arch/x86/boot/*.o arch/x86/boot/tanja-base
	rm -f arch/x86/idt_asm.o

distclean: clean
	rm -f bin/*.c

.PHONY: all clean distclean
