CC=gcc
LD=ld
ASM=nasm

CFLAGS=-ffreestanding -m32 -c -fno-stack-protector -fno-builtin -fno-pic -fno-pie -I.
LDFLAGS=-m elf_i386 -T linker/linker.ld -nostdlib

CMD_SRC = $(filter-out cmd/init.c, $(wildcard cmd/*.c))
CMD_OBJ = $(CMD_SRC:.c=.o)
CMD_NAMES = $(notdir $(basename $(CMD_SRC)))

all: arch/x86/boot/tanja-base

arch/x86/boot:
	mkdir -p arch/x86/boot

commandset:
	python3 scripts/gencmds.py

cmd/init.c: $(CMD_SRC)
	@rm -f cmd/init.c
	@echo "// Auto-generated" > cmd/init.c
	@echo '#include "cmd.h"' >> cmd/init.c
	@echo '' >> cmd/init.c
	@echo 'void init_cmds() {' >> cmd/init.c
	@echo '    extern void fs_init(void);' >> cmd/init.c
	@echo '    fs_init();' >> cmd/init.c
	@for name in $(CMD_NAMES); do \
		echo "    extern void cmd_$$name(char* args);" >> cmd/init.c; \
		echo "    register_cmd(\"$$name\", cmd_$$name);" >> cmd/init.c; \
	done
	@echo '}' >> cmd/init.c

cmd/%.o: cmd/%.c
	@echo "[CC] cmd/$(notdir $<)"
	$(CC) $(CFLAGS) -o $@ $<

kernel/kernel.o: kernel/kernel.c
	@echo "[CC] kernel/kernel.c"
	$(CC) $(CFLAGS) -o kernel/kernel.o kernel/kernel.c

fs/fs.o: fs/fs.c
	@echo "[CC] fs/fs.c"
	$(CC) $(CFLAGS) -o fs/fs.o fs/fs.c

cmd/init.o: cmd/init.c
	@echo "[CC] cmd/init.c"
	$(CC) $(CFLAGS) -o cmd/init.o cmd/init.c

arch/x86/boot/boot.o: arch/x86/boot/boot.asm | arch/x86/boot
	@echo "[ASM] boot.asm"
	$(ASM) -f elf32 arch/x86/boot/boot.asm -o arch/x86/boot/boot.o

arch/x86/boot/tanja-base: arch/x86/boot/boot.o kernel/kernel.o fs/fs.o cmd/init.o $(CMD_OBJ) | arch/x86/boot
	@echo "[LD] Linking..."
	$(LD) $(LDFLAGS) -o arch/x86/boot/tanja-base arch/x86/boot/boot.o kernel/kernel.o fs/fs.o cmd/init.o $(CMD_OBJ)
	@echo
	@echo "[INFO] Kernel image ready at arch/x86/boot/tanja-base"
	@echo "[INFO] Commands: $(CMD_NAMES)"
	@echo

clean:
	rm -f kernel/*.o fs/*.o
	rm -f cmd/*.o cmd/init.c
	rm -f arch/x86/boot/*.o arch/x86/boot/tanja-base

distclean: clean
	rm -f cmd/help.c cmd/reboot.c cmd/echo.c cmd/clear.c
	rm -f cmd/mkdir.c cmd/rmdir.c cmd/touch.c cmd/rm.c cmd/cat.c
	rm -f cmd/ls.c cmd/pwd.c cmd/cd.c cmd/editor.c

.PHONY: all clean distclean commandset
