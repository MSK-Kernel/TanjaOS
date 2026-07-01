CC=gcc
LD=ld
ASM=nasm

CFLAGS=-ffreestanding -m32 -c -fno-stack-protector -fno-builtin -fno-pic -fno-pie
LDFLAGS=-m elf_i386 -T linker/linker.ld -nostdlib

# Get all .c files in cmd/ EXCEPT init.c
CMD_SRC = $(filter-out cmd/init.c, $(wildcard cmd/*.c))
CMD_OBJ = $(CMD_SRC:.c=.o)
CMD_NAMES = $(notdir $(basename $(CMD_SRC)))

all: output/image/msk-i386

output/build:
	mkdir -p output/build

output/image:
	mkdir -p output/image

# Create default command set
commandset:
	@echo "Creating default command set..."
	@if [ ! -f cmd/help.c ]; then \
		printf '#include "cmd.h"\n\n' > cmd/help.c; \
		printf 'void cmd_help(char* args) {\n' >> cmd/help.c; \
		printf '    (void)args;\n' >> cmd/help.c; \
		printf '    list_commands();\n' >> cmd/help.c; \
		printf '}\n' >> cmd/help.c; \
		echo "[INFO] Created cmd/help.c"; \
	else \
		echo "[INFO] cmd/help.c already exists"; \
	fi
	@if [ ! -f cmd/reboot.c ]; then \
		printf '#include "cmd.h"\n\n' > cmd/reboot.c; \
		printf 'void cmd_reboot(char* args) {\n' >> cmd/reboot.c; \
		printf '    (void)args;\n' >> cmd/reboot.c; \
		printf '    print("Rebooting...\\n");\n' >> cmd/reboot.c; \
		printf '    for (volatile int i = 0; i < 1000000; i++);\n' >> cmd/reboot.c; \
		printf '    while ((inb(0x64) & 0x02) != 0);\n' >> cmd/reboot.c; \
		printf '    outb(0x64, 0xFE);\n' >> cmd/reboot.c; \
		printf '    asm volatile ("lidt 0\\n" "int $$0");\n' >> cmd/reboot.c; \
		printf '    while (1);\n' >> cmd/reboot.c; \
		printf '}\n' >> cmd/reboot.c; \
		echo "[INFO] Created cmd/reboot.c"; \
	else \
		echo "[INFO] cmd/reboot.c already exists"; \
	fi
	@if [ ! -f cmd/echo.c ]; then \
		printf '#include "cmd.h"\n\n' > cmd/echo.c; \
		printf 'void cmd_echo(char* args) {\n' >> cmd/echo.c; \
		printf '    if (!args || !*args) {\n' >> cmd/echo.c; \
		printf '        print("\\n");\n' >> cmd/echo.c; \
		printf '        return;\n' >> cmd/echo.c; \
		printf '    }\n' >> cmd/echo.c; \
		printf '\n' >> cmd/echo.c; \
		printf '    char* start = args;\n' >> cmd/echo.c; \
		printf '    char* end = args;\n' >> cmd/echo.c; \
		printf '\n' >> cmd/echo.c; \
		printf "    while (*start == ' ') start++;\n" >> cmd/echo.c; \
		printf '\n' >> cmd/echo.c; \
		printf "    if (*start == '\"') {\n" >> cmd/echo.c; \
		printf '        start++;\n' >> cmd/echo.c; \
		printf '        end = start;\n' >> cmd/echo.c; \
		printf "        while (*end && *end != '\"') end++;\n" >> cmd/echo.c; \
		printf '        for (char* p = start; p < end; p++) {\n' >> cmd/echo.c; \
		printf '            putc(*p);\n' >> cmd/echo.c; \
		printf '        }\n' >> cmd/echo.c; \
		printf '    } else if (*start == 39) {\n' >> cmd/echo.c; \
		printf '        start++;\n' >> cmd/echo.c; \
		printf '        end = start;\n' >> cmd/echo.c; \
		printf '        while (*end && *end != 39) end++;\n' >> cmd/echo.c; \
		printf '        for (char* p = start; p < end; p++) {\n' >> cmd/echo.c; \
		printf '            putc(*p);\n' >> cmd/echo.c; \
		printf '        }\n' >> cmd/echo.c; \
		printf '    } else {\n' >> cmd/echo.c; \
		printf '        end = start;\n' >> cmd/echo.c; \
		printf '        while (*end) end++;\n' >> cmd/echo.c; \
		printf "        while (end > start && *(end - 1) == ' ') end--;\n" >> cmd/echo.c; \
		printf '        for (char* p = start; p < end; p++) {\n' >> cmd/echo.c; \
		printf '            putc(*p);\n' >> cmd/echo.c; \
		printf '        }\n' >> cmd/echo.c; \
		printf '    }\n' >> cmd/echo.c; \
		printf '\n' >> cmd/echo.c; \
		printf '    print("\\n");\n' >> cmd/echo.c; \
		printf '}\n' >> cmd/echo.c; \
		echo "[INFO] Created cmd/echo.c"; \
	else \
		echo "[INFO] cmd/echo.c already exists"; \
	fi
	@if [ ! -f cmd/clear.c ]; then \
		printf '#include "cmd.h"\n\n' > cmd/clear.c; \
		printf 'void cmd_clear(char* args) {\n' >> cmd/clear.c; \
		printf '    (void)args;\n' >> cmd/clear.c; \
		printf '    clear_screen();\n' >> cmd/clear.c; \
		printf '}\n' >> cmd/clear.c; \
		echo "[INFO] Created cmd/clear.c"; \
	else \
		echo "[INFO] cmd/clear.c already exists"; \
	fi
	@echo
	@echo "[INFO] Base commands ready at cmd"
	@echo

# Auto-generate init.c from all .c files in cmd/ (excluding itself)
cmd/init.c: $(CMD_SRC)
	@rm -f cmd/init.c
	@echo "// Auto-generated - DO NOT EDIT" > cmd/init.c
	@echo '#include "cmd.h"' >> cmd/init.c
	@echo '' >> cmd/init.c
	@echo 'void init_cmds() {' >> cmd/init.c
	@for name in $(CMD_NAMES); do \
		printf "    extern void cmd_%s(char* args);\n" "$$name" >> cmd/init.c; \
		printf "    register_cmd(\"%s\", cmd_%s);\n" "$$name" "$$name" >> cmd/init.c; \
	done
	@echo '}' >> cmd/init.c
	@echo "[INFO] cmd/init.c with commands: $(CMD_NAMES)"

# Compile command files (but NOT init.c - it's built separately)
cmd/%.o: cmd/%.c
	@echo "[CC] $<"
	$(CC) $(CFLAGS) -I. -o $@ $<

# Build boot.o
output/build/boot.o: boot/boot.asm | output/build
	@echo "[ASM] boot.asm"
	$(ASM) -f elf32 boot/boot.asm -o output/build/boot.o

# Build kernel.o
output/build/kernel.o: kernel/kernel.c | output/build
	@echo "[CC] kernel.c"
	$(CC) $(CFLAGS) -I. kernel/kernel.c -o output/build/kernel.o

# Build ata.o
output/build/ata.o: kernel/ata.c | output/build
	@echo "[CC] ata.c"
	$(CC) $(CFLAGS) -I. kernel/ata.c -o output/build/ata.o

# Build init.o separately (from generated init.c)
cmd/init.o: cmd/init.c
	@echo "[CC] init.c (auto-generated)"
	$(CC) $(CFLAGS) -I. -o $@ $<

# Link everything
output/image/msk-i386: output/build/boot.o output/build/kernel.o output/build/ata.o cmd/init.o $(CMD_OBJ) | output/image
	@echo "[LD] Linking kernel with $(words $(CMD_NAMES)) commands..."
	$(LD) $(LDFLAGS) -o output/image/msk-i386 output/build/boot.o output/build/kernel.o output/build/ata.o cmd/init.o $(CMD_OBJ)
	@echo
	@echo "[INFO] Kernel ready at output/image/msk-i386"
	@echo "[INFO] Commands included: $(CMD_NAMES)"
	@echo

clean:
	rm -rf output
	rm -f cmd/*.o
	rm -f cmd/init.c

distclean: clean
	rm -f cmd/*.c

.PHONY: all clean distclean commandset
