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
MUSL_USER_CFLAGS := -static -O2 -Wall -Wextra -fno-stack-protector \
	-I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: run
run: run-$(ARCH)

.PHONY: run-x86_64
run-x86_64: edk2-ovmf $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-hda disk.img \
		-smp 4 \
		-serial stdio \
		-audiodev pa,id=snd0 \
		-device rtl8139,netdev=net0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-netdev user,id=net0 \
		$(QEMUFLAGS)

.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-cdrom $(IMAGE_NAME).iso \
		-hda disk.img \
		-boot d \
		-audiodev pa,id=snd0 \
		-device AC97,audiodev=snd0 \
		$(QEMUFLAGS)

# Create a 64MB ext2 disk image with sample files for testing
disk.img: test.wav test.bmp userland/hello.elf userland/test_mmap.elf userland/test_arch_prctl.elf userland/test_io.elf userland/test_fork.elf userland/test_execve.elf userland/test_wait_exec.elf userland/test_syscalls.elf userland/test_ioctl.elf userland/test_kilo_syscalls.elf userland/test_wait4_complex.elf userland/test_kilo_asm.elf userland/kilo.elf userland/test_args.elf userland/test_stat.elf userland/ls.elf userland/readelf.elf userland/pong.elf userland/raycast.elf userland/test_mmap_shared_private.elf userland/playwav.elf userland/showbmp.elf userland/test_uname_pipe.elf userland/test_pipe_fork.elf userland/test_sys_access.elf userland/test_sys_cwd.elf userland/test_newfstatat.elf userland/test_unlink_rename.elf userland/kria.elf userland/doom.elf userland/poll_test.elf userland/test_tcc_libc.c userland/test_mm.c userland/test_dynamic.elf userland/test_dup.elf userland/test_attrib.elf userland/test_symlink.elf userland/test_cred.elf
	dd if=/dev/zero of=disk.img bs=1M count=256
	mkfs.ext3 -F disk.img
	echo "Hello from AscentOS ext2!" > /tmp/ascentos_hello.txt
	echo "This is a test document." > /tmp/ascentos_readme.txt
	debugfs -w -R "cd /" -R "mkdir tmp" disk.img
	debugfs -w -R "write /tmp/ascentos_hello.txt hello.txt" disk.img
	debugfs -w -R "mkdir docs" disk.img
	debugfs -w -R "mkdir lib" disk.img
	debugfs -w -R "write toolchain/musl-sysroot/lib/libc.so lib/libc.so" disk.img
	debugfs -w -R "write toolchain/musl-sysroot/lib/libc.so lib/ld-musl-x86_64.so.1" disk.img
	debugfs -w -R "write /tmp/ascentos_readme.txt docs/readme.txt" disk.img
	debugfs -w -R "write userland/test_mmap.elf test_mmap.elf" disk.img
	debugfs -w -R "write userland/test_arch_prctl.elf test_arch_prctl.elf" disk.img
	debugfs -w -R "write userland/test_io.elf test_io.elf" disk.img
	debugfs -w -R "write userland/test_fork.elf test_fork.elf" disk.img
	debugfs -w -R "write userland/test_execve.elf test_execve.elf" disk.img
	debugfs -w -R "write userland/test_wait_exec.elf test_wait_exec.elf" disk.img
	debugfs -w -R "write userland/test_syscalls.elf test_syscalls.elf" disk.img
	debugfs -w -R "write userland/test_ioctl.elf test_ioctl.elf" disk.img
	debugfs -w -R "write userland/test_kilo_syscalls.elf test_kilo_syscalls.elf" disk.img
	debugfs -w -R "write userland/test_wait4_complex.elf test_wait4_complex.elf" disk.img
	debugfs -w -R "write userland/test_kilo_asm.elf test_kilo_asm.elf" disk.img
	debugfs -w -R "write userland/kilo.elf kilo.elf" disk.img
	debugfs -w -R "write userland/test_args.elf test_args.elf" disk.img
	debugfs -w -R "write userland/hello.elf hello.elf" disk.img
	debugfs -w -R "write userland/test_stat.elf test_stat.elf" disk.img
	debugfs -w -R "write userland/ls.elf ls.elf" disk.img
	debugfs -w -R "write userland/readelf.elf readelf.elf" disk.img
	debugfs -w -R "write userland/pong.elf pong.elf" disk.img
	debugfs -w -R "write userland/raycast.elf raycast.elf" disk.img
	debugfs -w -R "write userland/test_mmap_shared_private.elf test_mmap_shared_private.elf" disk.img
	debugfs -w -R "write userland/kria.elf kria.elf" disk.img
	debugfs -w -R "write userland/playwav.elf playwav.elf" disk.img
	debugfs -w -R "write userland/showbmp.elf showbmp.elf" disk.img
	debugfs -w -R "write userland/test_uname_pipe.elf test_uname_pipe.elf" disk.img
	debugfs -w -R "write userland/test_pipe_fork.elf test_pipe_fork.elf" disk.img
	debugfs -w -R "write userland/test_sys_access.elf test_sys_access.elf" disk.img
	debugfs -w -R "write userland/test_sys_cwd.elf test_sys_cwd.elf" disk.img
	debugfs -w -R "write userland/test_newfstatat.elf test_newfstatat.elf" disk.img
	debugfs -w -R "write userland/test_unlink_rename.elf test_unlink_rename.elf" disk.img
	debugfs -w -R "write userland/test_ext3.elf test_ext3.elf" disk.img
	debugfs -w -R "write userland/test_dynamic.elf test_dynamic.elf" disk.img
	debugfs -w -R "write userland/test_tcc_libc.c test_tcc_libc.c" disk.img
	debugfs -w -R "write userland/test_mm.c test_mm.c" disk.img
	debugfs -w -R "write userland/test_dup.elf test_dup.elf" disk.img
	debugfs -w -R "write userland/test_attrib.elf test_attrib.elf" disk.img
	debugfs -w -R "write userland/test_symlink.elf test_symlink.elf" disk.img
	debugfs -w -R "write userland/test_cred.elf test_cred.elf" disk.img
	debugfs -w -R "write userland/test.s test.s" disk.img
	debugfs -w -R "write userland/standalone.s standalone.s" disk.img
	debugfs -w -R "write test.wav test.wav" disk.img
	debugfs -w -R "write test.bmp test.bmp" disk.img
	debugfs -w -R "write terry.bmp terry.bmp" disk.img
	debugfs -w -R "write terry.wav terry.wav" disk.img
	debugfs -w -R "write charliekirk.wav charliekirk.wav" disk.img
	debugfs -w -R "write charliekir.wav mc952.wav" disk.img
	debugfs -w -R "write userland/kria-lang/test.krx test.krx" disk.img
	debugfs -w -R "write userland/hello.krx hello.krx" disk.img
	debugfs -w -R "write userland/doom.elf doom.elf" disk.img
	debugfs -w -R "write userland/poll_test.elf poll_test.elf" disk.img
	debugfs -w -R "write doomu.wad doom1.wad" disk.img
	rm -f /tmp/ascentos_hello.txt /tmp/ascentos_readme.txt
	@if [ -d toolchain/musl-sysroot/opt/tcc ]; then \
		echo "Installing TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/opt/tcc opt/tcc; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/bin/tcc tcc.elf" disk.img; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/libc.a libc.a" disk.img; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crt1.o crt1.o" disk.img; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crti.o crti.o" disk.img; \
		debugfs -w -R "write toolchain/musl-sysroot/lib/crtn.o crtn.o" disk.img; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/lib/tcc/libtcc1.a libtcc1.a" disk.img; \
	fi
	@if [ -d toolchain/musl-sysroot/opt/coreutils ]; then \
		echo "Installing coreutils into disk image..."; \
		./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/opt/coreutils opt/coreutils; \
	fi
	@if [ -d toolchain/musl-sysroot/opt/bash ]; then \
		echo "Installing bash into disk image..."; \
		debugfs -w -R "mkdir opt" disk.img 2>/dev/null || true; \
		./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/opt/bash opt/bash; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/bash/bin/bash bash.elf" disk.img; \
		echo "root:x:0:0:root:/root:/bash.elf" > /tmp/passwd; \
		echo "PS1='\033[0;32mRoot@AscentOS\033[0m:\w\\$$ '" > /tmp/bashrc; \
		echo "PATH=/opt/coreutils/bin:/opt/bash/bin:/opt/tcc/bin:/" >> /tmp/bashrc; \
		echo "HOME=/root" >> /tmp/bashrc; \
		debugfs -w -R "mkdir etc" disk.img 2>/dev/null || true; \
		debugfs -w -R "write /tmp/passwd etc/passwd" disk.img; \
		debugfs -w -R "mkdir root" disk.img 2>/dev/null || true; \
		debugfs -w -R "write /tmp/bashrc root/.bashrc" disk.img; \
		rm -f /tmp/passwd /tmp/bashrc; \
	fi

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
clean: clean-musl clean-doom clean-coreutils
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd

.PHONY: clean-coreutils
clean-coreutils:
	rm -rf build/coreutils-9.5
	rm -rf toolchain/musl-sysroot/opt/coreutils

.PHONY: clean-musl
clean-musl:
	rm -rf build/musl-1.2.5 build/musl-cross-make
	rm -rf toolchain/musl-sysroot toolchain/x86_64-linux-musl
	rm -f userland/hello.elf userland/test_syscalls.elf userland/test_kilo_syscalls.elf userland/test_kilo_asm.elf userland/kilo.elf userland/test_args.elf userland/kilo.c userland/test_mmap_shared_private.elf userland/playwav.elf userland/kria.elf userland/test_uname_pipe.elf userland/test_pipe_fork.elf userland/test_sys_access.elf userland/test_sys_cwd.elf userland/test_newfstatat.elf userland/ls.elf userland/readelf.elf userland/test_ext3.elf userland/poll_test.elf
	rm -rf userland/kria-lang/target

.PHONY: clean-disk
clean-disk:
	rm -f disk.img

.PHONY: distclean
distclean: clean-musl clean-doom
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso *.hdd limine edk2-ovmf doomgeneric
	rm -rf userland/kria-lang

# ── Userland test programs ──────────────────────────────────────────────────
$(MUSL_LIBC):
	chmod +x scripts/musl-toolchain.sh
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" ./scripts/musl-toolchain.sh

.PHONY: musl-toolchain
musl-toolchain: $(MUSL_LIBC)

userland/hello.elf: userland/hello.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
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

userland/test_syscalls.elf: userland/test_syscalls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_syscalls.c -o userland/test_syscalls.elf

userland/test_ioctl.elf: userland/test_ioctl.asm
	nasm -f elf64 userland/test_ioctl.asm -o userland/test_ioctl.o
	ld -o userland/test_ioctl.elf userland/test_ioctl.o

userland/test_kilo_syscalls.elf: userland/test_kilo_syscalls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_kilo_syscalls.c -o userland/test_kilo_syscalls.elf

userland/test_kilo_asm.elf: userland/test_kilo_asm.asm
	nasm -f elf64 userland/test_kilo_asm.asm -o userland/test_kilo_asm.o
	ld -o userland/test_kilo_asm.elf userland/test_kilo_asm.o

userland/kilo.c:
	curl -L https://raw.githubusercontent.com/antirez/kilo/master/kilo.c -o userland/kilo.c

userland/kilo.elf: userland/kilo.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/kilo.c -o userland/kilo.elf

userland/test_args.elf: userland/test_args.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_args.c -o userland/test_args.elf

userland/test_stat.elf: userland/test_stat.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_stat.c -o userland/test_stat.elf

userland/test_mmap_shared_private.elf: userland/test_mmap_shared_private.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_mmap_shared_private.c -o userland/test_mmap_shared_private.elf

userland/ls.elf: userland/ls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/ls.c -o userland/ls.elf

userland/readelf.elf: userland/readelf.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/readelf.c -o userland/readelf.elf

userland/pong.elf: userland/pong.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/pong.c -o userland/pong.elf

userland/raycast.elf: userland/raycast.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/raycast.c -lm -o userland/raycast.elf

userland/playwav.elf: userland/playwav.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/playwav.c -o userland/playwav.elf

userland/test_ext3.elf: userland/test_ext3.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_ext3.c -o userland/test_ext3.elf

userland/showbmp.elf: userland/showbmp.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/showbmp.c -o userland/showbmp.elf

userland/test_uname_pipe.elf: userland/test_uname_pipe.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_uname_pipe.c -o userland/test_uname_pipe.elf

userland/test_pipe_fork.elf: userland/test_pipe_fork.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_pipe_fork.c -o userland/test_pipe_fork.elf

userland/test_sys_access.elf: userland/test_sys_access.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_sys_access.c -o userland/test_sys_access.elf

userland/test_sys_cwd.elf: userland/test_sys_cwd.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_sys_cwd.c -o userland/test_sys_cwd.elf

userland/test_newfstatat.elf: userland/test_newfstatat.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_newfstatat.c -o userland/test_newfstatat.elf

userland/test_unlink_rename.elf: userland/test_unlink_rename.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_unlink_rename.c -o userland/test_unlink_rename.elf

userland/test_dup.elf: userland/test_dup.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_dup.c -o userland/test_dup.elf

userland/test_attrib.elf: userland/test_attrib.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_attrib.c -o userland/test_attrib.elf

userland/test_symlink.elf: userland/test_symlink.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_symlink.c -o userland/test_symlink.elf

userland/test_cred.elf: userland/test_cred.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_cred.c -o userland/test_cred.elf

userland/test_wait4_complex.elf: userland/test_wait4_complex.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_wait4_complex.c -o userland/test_wait4_complex.elf

userland/test_dynamic.elf: userland/test_dynamic.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) -O2 -Wall -Wextra -fno-stack-protector -I$(MUSL_SYSROOT)/include -L$(MUSL_SYSROOT)/lib \
		userland/test_dynamic.c -o userland/test_dynamic.elf

userland/poll_test.elf: userland/poll_test.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/poll_test.c -o userland/poll_test.elf

# Kria programming language (Rust-based, compiled with musl for static linking)
userland/kria-lang:
	rm -rf userland/kria-lang
	git clone https://github.com/Piotriox/kria-lang.git userland/kria-lang
	mkdir -p userland/kria-lang/.cargo
	echo '[build]' > userland/kria-lang/.cargo/config.toml
	echo 'target = "x86_64-unknown-linux-musl"' >> userland/kria-lang/.cargo/config.toml

userland/kria.elf: userland/kria-lang
	cd userland/kria-lang && cargo build --release
	cp userland/kria-lang/target/x86_64-unknown-linux-musl/release/kria userland/kria.elf

.PHONY: kria
kria: userland/kria.elf

test.wav:
	python3 gen_wav.py

# ── DOOM (doomgeneric) ──────────────────────────────────────────────────────
.PHONY: doom
doom: userland/doom.elf

doomgeneric:
	rm -rf doomgeneric
	git clone https://github.com/ozkl/doomgeneric.git --depth=1

userland/doom.elf: doomgeneric $(MUSL_LIBC)
	$(MAKE) -C userland -f Makefile.ascentos \
		MUSL_CC="$(MUSL_CC)" \
		MUSL_SYSROOT="$(MUSL_SYSROOT)"

.PHONY: clean-doom
clean-doom:
	$(MAKE) -C userland -f Makefile.ascentos clean
