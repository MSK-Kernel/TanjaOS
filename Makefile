CC=gcc
LD=ld
ASM=nasm

CFLAGS=-ffreestanding -m32 -c -fno-stack-protector -fno-builtin -fno-pic -fno-pie
LDFLAGS=-m elf_i386 -T linker/linker.ld -nostdlib

# Get all .c files in cmd/ EXCEPT init.c (which is auto-generated)
CMD_SRC = $(filter-out cmd/init.c, $(wildcard cmd/*.c))
CMD_OBJ = $(CMD_SRC:.c=.o)
CMD_NAMES = $(notdir $(basename $(CMD_SRC)))

all: output/image/msk-i386

output/build:
	mkdir -p output/build

output/image:
	mkdir -p output/image

# Auto-generate init.c from all .c files in cmd/ (excluding itself)
cmd/init.c: $(CMD_SRC)
	@echo "// Auto-generated - DO NOT EDIT" > $@
	@echo 'extern void register_cmd(const char* name, void (*func)(void));' >> $@
	@echo '' >> $@
	@echo 'void init_cmds() {' >> $@
	@for f in $(CMD_NAMES); do \
		echo "    extern void $${f}(void);" >> $@; \
		echo "    register_cmd(\"$$f\", $${f});" >> $@; \
	done
	@echo '}' >> $@

# Compile command files (but NOT init.c - it's built separately)
cmd/%.o: cmd/%.c
	$(CC) $(CFLAGS) -o $@ $<

# Build boot.o
output/build/boot.o: boot/boot.asm | output/build
	$(ASM) -f elf32 boot/boot.asm -o output/build/boot.o

# Build kernel.o
output/build/kernel.o: kernel/kernel.c | output/build
	$(CC) $(CFLAGS) kernel/kernel.c -o output/build/kernel.o

# Build init.o separately (from generated init.c)
cmd/init.o: cmd/init.c
	$(CC) $(CFLAGS) -o $@ $<

# Link everything
output/image/msk-i386: output/build/boot.o output/build/kernel.o cmd/init.o $(CMD_OBJ) | output/image
	@echo "[INFO] Linking with $(words $(CMD_NAMES)) commands: $(CMD_NAMES)"
	$(LD) $(LDFLAGS) -o output/image/msk-i386 output/build/boot.o output/build/kernel.o cmd/init.o $(CMD_OBJ)
	@echo
	@echo "[INFO] Kernel ready at output/image/msk-i386"
	@echo
clean:
	rm -rf output
	rm -f cmd/*.o
	rm -f cmd/init.c
