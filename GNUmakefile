.SUFFIXES:

ARCH := x86_64
QEMUFLAGS := -m 2G

override IMAGE_NAME := ascentos-$(ARCH)

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

# musl static sysroot (see scripts/musl-toolchain.sh). Built automatically for hello_musl / disk.img / run.
MUSL_TOOLCHAIN_BIN := $(CURDIR)/toolchain/x86_64-linux-musl/bin
MUSL_SYSROOT := $(CURDIR)/toolchain/musl-sysroot
MUSL_LIBC := $(MUSL_SYSROOT)/lib/libc.a
MUSL_CC ?= x86_64-linux-musl-gcc

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: run
run: run-$(ARCH)

.PHONY: run-x86_64
run-x86_64: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-hda disk.img \
		-smp 4 \
		-serial stdio \
		-audiodev none,id=none \
		$(QEMUFLAGS)

.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35 \
		-cdrom $(IMAGE_NAME).iso \
		-hda disk.img \
		-boot d \
		$(QEMUFLAGS)

# Create a 64MB ext2 disk image with sample files for testing
disk.img: userland/hello.elf userland/test_mmap.elf userland/test_arch_prctl.elf userland/test_io.elf userland/test_fork.elf userland/test_execve.elf userland/test_wait_exec.elf
	dd if=/dev/zero of=disk.img bs=1M count=64
	mkfs.ext2 -F disk.img
	echo "Hello from AscentOS ext2!" > /tmp/ascentos_hello.txt
	echo "This is a test document." > /tmp/ascentos_readme.txt
	debugfs -w -R "write /tmp/ascentos_hello.txt hello.txt" disk.img
	debugfs -w -R "mkdir docs" disk.img
	debugfs -w -R "write /tmp/ascentos_readme.txt docs/readme.txt" disk.img
	debugfs -w -R "write userland/test_mmap.elf test_mmap.elf" disk.img
	debugfs -w -R "write userland/test_arch_prctl.elf test_arch_prctl.elf" disk.img
	debugfs -w -R "write userland/test_io.elf test_io.elf" disk.img
	debugfs -w -R "write userland/test_fork.elf test_fork.elf" disk.img
	debugfs -w -R "write userland/test_execve.elf test_execve.elf" disk.img
	debugfs -w -R "write userland/test_wait_exec.elf test_wait_exec.elf" disk.img
	debugfs -w -R "write userland/hello.elf hello.elf" disk.img
	rm -f /tmp/ascentos_hello.txt /tmp/ascentos_readme.txt

edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

limine/limine:
	rm -rf limine
	git clone https://codeberg.org/Limine/Limine.git limine --branch=v11.x-binary --depth=1
	$(MAKE) -C limine \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

.PHONY: setup
setup:
	chmod +x bootstrap.sh
	./bootstrap.sh

.PHONY: kernel
kernel: setup
	$(MAKE) -C kernel

$(IMAGE_NAME).iso: limine/limine kernel
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin-$(ARCH)/kernel iso_root/boot/
	mkdir -p iso_root/boot/limine
	cp -v limine.conf iso_root/boot/limine/
	cp -v boo.png iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

.PHONY: clean
clean: clean-musl
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd

.PHONY: clean-musl
clean-musl:
	rm -rf build/musl-1.2.5 build/musl-cross-make
	rm -rf toolchain/musl-sysroot toolchain/x86_64-linux-musl
	rm -f userland/hello.elf

.PHONY: clean-disk
clean-disk:
	rm -f disk.img

.PHONY: distclean
distclean: clean-musl
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso *.hdd limine edk2-ovmf

# ── Userland test programs ──────────────────────────────────────────────────
$(MUSL_LIBC):
	chmod +x scripts/musl-toolchain.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" ./scripts/musl-toolchain.sh

.PHONY: musl-toolchain
musl-toolchain: $(MUSL_LIBC)

userland/hello.elf: userland/hello.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) -static -O2 -Wall -Wextra \
		-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib \
		userland/hello.c -o userland/hello.elf

userland/test_mmap.elf: userland/test_mmap.asm
	nasm -f elf64 userland/test_mmap.asm -o userland/test_mmap.o
	ld -o userland/test_mmap.elf userland/test_mmap.o

userland/test_arch_prctl.elf: userland/test_arch_prctl.asm
	nasm -f elf64 userland/test_arch_prctl.asm -o userland/test_arch_prctl.o
	ld -o userland/test_arch_prctl.elf userland/test_arch_prctl.o

userland/test_io.elf: userland/test_io.asm
	nasm -f elf64 userland/test_io.asm -o userland/test_io.o
	ld -o userland/test_io.elf userland/test_io.o

userland/test_fork.elf: userland/test_fork.asm
	nasm -f elf64 userland/test_fork.asm -o userland/test_fork.o
	ld -o userland/test_fork.elf userland/test_fork.o

userland/test_execve.elf: userland/test_execve.asm
	nasm -f elf64 userland/test_execve.asm -o userland/test_execve.o
	ld -o userland/test_execve.elf userland/test_execve.o

userland/test_wait_exec.elf: userland/test_wait_exec.asm
	nasm -f elf64 userland/test_wait_exec.asm -o userland/test_wait_exec.o
	ld -o userland/test_wait_exec.elf userland/test_wait_exec.o
