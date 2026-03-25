# AscentOS 64-bit Makefile — Unified Kernel (Text + GUI)

CC = gcc
AS = nasm
LD = ld

USERLAND_CC := $(shell which x86_64-elf-gcc 2>/dev/null || echo gcc)
USERLAND_LD := $(shell which x86_64-elf-ld  2>/dev/null || echo ld)
LIBGCC := $(shell $(USERLAND_CC) -m64 --print-libgcc-file-name 2>/dev/null)
GCC_INCLUDE := $(shell $(USERLAND_CC) -m64 -print-file-name=include 2>/dev/null)

CFLAGS = -m64 -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel \
         -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fno-pic \
         -Wall -Wextra -O2

ASFLAGS  = -f elf64
BOOT_ASFLAGS = -f elf64 -DGUI_MODE

LDFLAGS  = -n -T kernel/linker64.ld -nostdlib

# Main target
all: AscentOS.iso userland install-userland
	@echo "╔═══════════════════════════════════════════════════╗"
	@echo "║  AscentOS Unified Kernel ready                    ║"
	@echo "║  Filesystem: Ext3                                 ║"
	@echo "║  Start in TEXT mode                               ║"
	@echo "║  'dhcp'     → Get IP automatically                ║"
	@echo "║  'tcptest 10.0.2.2 80' → TCP test                 ║"
	@echo "║  'tcplisten 8080'      → TCP server               ║"
	@echo "║  'gfx'      → Switch to GUI mode                  ║"
	@echo "║  make run   → Start the OS                        ║"
	@echo "╚═══════════════════════════════════════════════════╝"


# ============================================================================
# KERNEL OBJECTS
# ============================================================================

files64.o: fs/files64.c fs/files64.h fs/ext3.h drivers/ata64.h
	$(CC) $(CFLAGS) -c fs/files64.c -o $@

ata64.o: drivers/ata64.c drivers/ata64.h
	$(CC) $(CFLAGS) -c drivers/ata64.c -o $@

ext3.o: fs/ext3.c fs/ext3.h drivers/ata64.h fs/files64.h
	$(CC) $(CFLAGS) -c fs/ext3.c -o $@

journal.o: fs/journal.c fs/journal.h
	$(CC) $(CFLAGS) -c fs/journal.c -o $@

elf64.o: kernel/elf64.c kernel/elf64.h
	$(CC) $(CFLAGS) -c kernel/elf64.c -o $@

pmm.o: kernel/pmm.c kernel/pmm.h
	$(CC) $(CFLAGS) -c kernel/pmm.c -o $@

heap.o: kernel/heap.c kernel/heap.h kernel/pmm.h
	$(CC) $(CFLAGS) -c kernel/heap.c -o $@

page_fault.o: kernel/page_fault_handler.c
	$(CC) $(CFLAGS) -c kernel/page_fault_handler.c -o $@

vmm64.o: kernel/vmm64.c kernel/vmm64.h kernel/pmm.h kernel/heap.h
	$(CC) $(CFLAGS) -c kernel/vmm64.c -o $@

timer.o: kernel/timer.c kernel/timer.h
	$(CC) $(CFLAGS) -c kernel/timer.c -o $@

pcspk.o: drivers/pcspk.c drivers/pcspk.h
	$(CC) $(CFLAGS) -c drivers/pcspk.c -o $@

sb16.o: drivers/sb16.c drivers/sb16.h
	$(CC) $(CFLAGS) -c drivers/sb16.c -o $@

task.o: kernel/task.c kernel/task.h
	$(CC) $(CFLAGS) -c kernel/task.c -o $@

scheduler.o: kernel/scheduler.c kernel/scheduler.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/scheduler.c -o $@

font8x16.o: kernel/font8x16.c kernel/font8x16.h
	$(CC) $(CFLAGS) -c kernel/font8x16.c -o $@

vesa64.o: drivers/vesa64.c drivers/vesa64.h kernel/font8x16.h
	$(CC) $(CFLAGS) -c drivers/vesa64.c -o $@

syscall.o: kernel/syscall.c kernel/syscall.h drivers/sb16.h kernel/signal64.h
	$(CC) $(CFLAGS) -c kernel/syscall.c -o $@

signal64.o: kernel/signal64.c kernel/signal64.h kernel/syscall.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/signal64.c -o $@

panic64.o: kernel/panic64.c
	$(CC) $(CFLAGS) -c kernel/panic64.c -o $@

pci.o: drivers/pci.c drivers/pci.h
	$(CC) $(CFLAGS) -c drivers/pci.c -o $@

rtl8139.o: drivers/rtl8139.c drivers/rtl8139.h drivers/pci.h
	$(CC) $(CFLAGS) -c drivers/rtl8139.c -o $@

arp.o: network/arp.c network/arp.h drivers/rtl8139.h
	$(CC) $(CFLAGS) -c network/arp.c -o $@

ipv4.o: network/ipv4.c network/ipv4.h network/arp.h drivers/rtl8139.h
	$(CC) $(CFLAGS) -c network/ipv4.c -o $@

icmp.o: network/icmp.c network/icmp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/icmp.c -o $@

udp.o: network/udp.c network/udp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/udp.c -o $@

dhcp.o: network/dhcp.c network/dhcp.h network/udp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/dhcp.c -o $@

tcp.o: network/tcp.c network/tcp.h network/ipv4.h network/arp.h drivers/rtl8139.h
	$(CC) $(CFLAGS) -c network/tcp.c -o $@

http.o: network/http.c network/http.h network/tcp.h network/arp.h network/ipv4.h
	$(CC) $(CFLAGS) -c network/http.c -o $@

kernel64.o: kernel/kernel64.c drivers/mouse64.h \
            drivers/ata64.h fs/ext3.h fs/files64.h kernel/cpu64.h \
            drivers/sb16.h
	$(CC) $(CFLAGS) -c kernel/kernel64.c -o $@

cpu64.o: kernel/cpu64.c kernel/cpu64.h
	$(CC) $(CFLAGS) -c kernel/cpu64.c -o $@

spinlock64.o: kernel/spinlock64.c kernel/spinlock64.h kernel/cpu64.h
	$(CC) $(CFLAGS) -c kernel/spinlock64.c -o $@

# Boot and interrupt assembly
boot64.o: boot/boot64_unified.asm
	$(AS) $(BOOT_ASFLAGS) $< -o $@

interrupts64.o: arch/x86_64/interrupts64.asm
	$(AS) $(BOOT_ASFLAGS) $< -o $@

mouse64.o: drivers/mouse64.c drivers/mouse64.h
	$(CC) $(CFLAGS) -c $< -o $@

idt64.o: kernel/idt64.c kernel/idt64.h kernel/task.h
	$(CC) $(CFLAGS) -c $< -o $@

keyboard.o: drivers/keyboard_unified.c kernel/idt64.h
	$(CC) $(CFLAGS) -c $< -o $@

commands64.o: commands/commands64.c commands/commands64.h \
              drivers/sb16.h drivers/pcspk.h drivers/pci.h
	$(CC) $(CFLAGS) -c $< -o $@


KERNEL_OBJS = boot64.o interrupts64.o idt64.o \
              font8x16.o vesa64.o mouse64.o journal.o \
              keyboard.o kernel64.o cpu64.o spinlock64.o \
              commands64.o files64.o ata64.o ext3.o elf64.o \
              pmm.o heap.o vmm64.o timer.o pcspk.o sb16.o task.o scheduler.o \
              page_fault.o syscall.o signal64.o \
              panic64.o rtl8139.o pci.o arp.o ipv4.o icmp.o udp.o dhcp.o tcp.o http.o

kernel64.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) $^ -o $@


# ============================================================================
# DISK IMAGE (Ext3)
# ============================================================================

disk.img:
	@echo "📀 Creating Ext3 disk image (2048MB)..."
	@if ! command -v mkfs.ext3 >/dev/null 2>&1; then \
	    echo "ERROR: mkfs.ext3 not found. Please install e2fsprogs."; \
	    exit 1; \
	fi
	@rm -f disk.img
	dd if=/dev/zero of=disk.img bs=1M count=2048 status=none
	mkfs.ext3 -b 1024 -L "AscentOS" -m 0 -O ^metadata_csum,^has_journal disk.img
	@$(MAKE) --no-print-directory _disk_mkdirs
	@echo "✓ disk.img ready (Ext3, 2048MB)"

_disk_mkdirs:
	@if command -v debugfs >/dev/null 2>&1; then \
	    debugfs -w disk.img -R "mkdir bin"  2>/dev/null || true; \
	    debugfs -w disk.img -R "mkdir usr"  2>/dev/null || true; \
	    debugfs -w disk.img -R "mkdir etc"  2>/dev/null || true; \
	    debugfs -w disk.img -R "mkdir tmp"  2>/dev/null || true; \
	    debugfs -w disk.img -R "mkdir home" 2>/dev/null || true; \
	fi

disk-rebuild:
	@rm -f disk.img
	@$(MAKE) --no-print-directory disk.img

AscentOS.iso: kernel64.elf grub64.cfg disk.img
	@echo "📦 Building AscentOS ISO..."
	mkdir -p isodir/boot/grub
	cp kernel64.elf isodir/boot/kernel64.elf
	cp grub64.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o AscentOS.iso isodir 2>&1 | grep -v "xorriso"
	@echo "✓ AscentOS.iso created!"


# ============================================================================
# USERLAND (musl libc)
# ============================================================================

MUSL_STAMP := userland/libc/musl/lib/libc.a

$(MUSL_STAMP):
	@echo "🔨 Building musl libc (this may take a while)..."
	@chmod +x musl-build.sh && ./musl-build.sh
	@echo "✓ musl libc ready"

musl: $(MUSL_STAMP)

MUSL_INC := userland/libc/musl/include
MUSL_LIB := userland/libc/musl/lib

USERLAND_CFLAGS := -m64 -ffreestanding -nostdlib -nostdinc \
                   -isystem $(GCC_INCLUDE) -isystem $(MUSL_INC) \
                   -ffunction-sections -fdata-sections \
                   -fno-stack-protector -mno-red-zone -O2 -Wall

USERLAND_ASFLAGS := -f elf64

USERLAND_LDFLAGS := -T userland/libc/user.ld -static -nostdlib \
                    --gc-sections -u ___errno_location -u __errno_location

USERLAND_CRT0   := userland/libc/crt0.o
SYSCALLS_OBJ    := userland/out/syscalls.o
USERLAND_APPS   := hello calculator shell snake wav_player syscall_test
USERLAND_ELFS   := $(addprefix userland/out/, $(addsuffix .elf, $(USERLAND_APPS)))

userland: $(MUSL_STAMP) userland/out $(USERLAND_CRT0) $(SYSCALLS_OBJ) $(USERLAND_ELFS)
	@echo "✓ Userland built successfully → userland/out/"

userland/out:
	@mkdir -p $@

userland/libc/crt0.o: userland/libc/crt0.asm
	$(AS) $(USERLAND_ASFLAGS) -o $@ $<

$(SYSCALLS_OBJ): userland/libc/syscalls.c | userland/out
	$(USERLAND_CC) -m64 -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INCLUDE) \
	    -fno-stack-protector -ffunction-sections -fdata-sections -O2 -Wall \
	    -Wno-builtin-declaration-mismatch -fno-builtin-malloc -fno-builtin-free \
	    -c -o $@ $<

userland/out/%.o: userland/apps/%.c | $(MUSL_STAMP)
	$(USERLAND_CC) $(USERLAND_CFLAGS) -c -o $@ $<

userland/out/%.elf: userland/out/%.o $(USERLAND_CRT0) $(SYSCALLS_OBJ) | $(MUSL_STAMP)
	$(USERLAND_LD) $(USERLAND_LDFLAGS) -m elf_x86_64 --allow-multiple-definition \
	    $(USERLAND_CRT0) $< $(SYSCALLS_OBJ) -L$(MUSL_LIB) -lc $(LIBGCC) -o $@
	@echo "  ✓ $@ built"

# Short targets
hello calculator shell snake wav_player syscall_test: userland/out/$@.elf
	@echo "  ✓ $@.elf ready"

# Install userland binaries to Ext3 disk
install-userland: userland disk.img
	@echo "📦 Installing userland binaries to disk.img..."
	@if command -v debugfs >/dev/null 2>&1; then \
	    debugfs -w disk.img -R "write userland/out/hello.elf      bin/hello.elf"      2>/dev/null || true; \
	    debugfs -w disk.img -R "write userland/out/calculator.elf bin/calc.elf"        2>/dev/null || true; \
	    debugfs -w disk.img -R "write userland/out/snake.elf      bin/snake.elf"       2>/dev/null || true; \
	    debugfs -w disk.img -R "write userland/out/shell.elf      bin/shell.elf"       2>/dev/null || true; \
	    debugfs -w disk.img -R "write userland/out/wav_player.elf bin/wav_player.elf"  2>/dev/null || true; \
	    debugfs -w disk.img -R "write userland/out/syscall_test.elf bin/syscall_test.elf" 2>/dev/null || true; \
	    echo "✓ Binaries installed to /bin/ using debugfs"; \
	else \
	    echo "⚠ debugfs not found. Install e2fsprogs."; \
	fi

disk-ls:
	@if command -v debugfs >/dev/null 2>&1; then \
	    debugfs disk.img -R "ls bin" 2>/dev/null; \
	else \
	    echo "debugfs not found"; \
	fi


# ============================================================================
# RUN / DEBUG
# ============================================================================

run: AscentOS.iso disk.img install-userland
	@echo "▶ Starting AscentOS..."
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 1024M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
	  -device sb16,audiodev=snd0 \
	  -netdev user,id=net0,restrict=off,ipv6=off,hostname=AscentOS,\
hostfwd=udp::5000-:5000,hostfwd=udp::5001-:5001,hostfwd=udp::5002-:5002,\
hostfwd=tcp::8080-:8080,hostfwd=tcp::8081-:8081 \
	  -device rtl8139,netdev=net0 \
	  -display gtk,zoom-to-fit=off

debug: AscentOS.iso disk.img
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
	  -device sb16,audiodev=snd0 \
	  -netdev user,id=net0,restrict=off \
	  -device rtl8139,netdev=net0 \
	  -s -S

gdb:
	gdb -ex "target remote :1234" \
	    -ex "symbol-file kernel64.elf" \
	    -ex "set architecture i386:x86-64"


# ============================================================================
# CLEAN
# ============================================================================

clean:
	rm -rf *.o *.elf isodir AscentOS.iso disk.img userland/out userland/libc/crt0.o

musl-clean:
	rm -rf userland/libc/musl build-musl musl-*.tar.gz

clean-all: clean musl-clean

# ============================================================================
# INFO & HELP
# ============================================================================

info:
	@echo "AscentOS Build Information"
	@echo "  Filesystem : Ext3"
	@echo "  Userland   : musl libc + syscalls"
	@echo "  Use 'make run' to start"

help:
	@echo "Common commands:"
	@echo "  make                Build everything"
	@echo "  make musl           Build musl libc (first time only)"
	@echo "  make run            Build + run in QEMU"
	@echo "  make clean          Clean build files"
	@echo "  make clean-all      Full clean (including musl)"

.PHONY: all run debug gdb musl userland install-userland disk-ls \
        disk-rebuild hello calculator shell snake wav_player syscall_test \
        clean musl-clean clean-all info help