# MiniOS Phase 11 Makefile
CC      = gcc
AS      = gcc
LD      = ld
CFLAGS  = -m32 -fno-pie -fno-stack-protector -std=gnu11 -ffreestanding -O2 \
          -Wall -Wextra -fno-builtin -nostdlib -I.
ASFLAGS = -m32 -fno-pie -fno-stack-protector
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib -z noexecstack

C_SRC  := $(shell find kernel lib shell commands -name '*.c')
S_SRC  := $(shell find boot -name '*.S')
OBJS   := $(C_SRC:.c=.o) $(S_SRC:.S=.o)

all: minios.bin

minios.bin: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@grub-file --is-x86-multiboot $@ && echo "multiboot OK" || echo "(grub-file not found or multiboot check failed)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(AS) $(ASFLAGS) -c $< -o $@

iso: minios.bin
	mkdir -p iso/boot/grub
	cp minios.bin iso/boot/minios.bin
	printf 'set timeout=0\nset default=0\nmenuentry "MiniOS" {\n  multiboot /boot/minios.bin\n  boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o minios.iso iso

run-kernel: minios.bin
	qemu-system-i386 -kernel minios.bin -m 128M

run: iso
	qemu-system-i386 -cdrom minios.iso -m 128M

test:
	@bash tests/run_tests.sh

clean:
	find . -name '*.o' -delete; rm -rf build; rm -f minios.bin minios.iso

.PHONY: all iso run run-kernel test gen clean

# rebuild everything when a header changes
$(OBJS): $(wildcard include/*.h)
