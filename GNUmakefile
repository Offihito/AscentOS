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
WOLFSSL_LIB := $(MUSL_SYSROOT)/lib/libwolfssl.a
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
		-device e1000,netdev=net0 \
		-device sb16,audiodev=snd0 \
		-device AC97,audiodev=snd0 \
		-netdev user,id=net0 \
		-usb \
		$(QEMUFLAGS)

.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso disk.img
	qemu-system-$(ARCH) \
		-M q35,pcspk-audiodev=snd0 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-hda disk.img \
		-boot d \
		-audiodev pa,id=snd0 \
		-device AC97,audiodev=snd0 \
		$(QEMUFLAGS)

# Create a 64MB ext2 disk image with sample files for testing
disk.img: test.wav test.bmp test.tar userland/hello.elf userland/test_cow.elf userland/test_syscalls.elf userland/test_kilo_syscalls.elf userland/test_wait4_complex.elf userland/kilo.elf userland/test_args.elf userland/test_stat.elf userland/ls.elf userland/readelf.elf userland/pong.elf userland/raycast.elf userland/test_mmap_shared_private.elf userland/playwav.elf userland/showbmp.elf userland/test_uname_pipe.elf userland/test_pipe_fork.elf userland/test_sys_access.elf userland/test_sys_cwd.elf userland/test_newfstatat.elf userland/test_unlink_rename.elf userland/wget.elf userland/kria.elf userland/doom.elf userland/poll_test.elf userland/test_tcc_libc.c userland/test_mm.c userland/test_dynamic.elf userland/test_dup.elf userland/test_attrib.elf userland/test_symlink.elf userland/test_cred.elf userland/test_time.elf userland/test_tsc_manual.elf userland/lua.elf userland/test_unix_sock.elf userland/test_unix_fdpass.elf userland/test_fb.elf userland/test_events.elf userland/test_socket_phase3.elf userland/test_socket_phase3_advanced.elf userland/test_socket_phase3_megastress.elf userland/test_socket_phase4.elf userland/test_socket_phase5.elf userland/test_socket_phase6.elf userland/test_socket_phase7.elf userland/test_socket_phase7_advanced.elf userland/test_socket_phase8.elf userland/test_socket_phase9.elf userland/test_socket_phase10.elf userland/test_socket_phase11.elf userland/xeyes.elf userland/xterm.elf userland/test_x11_simple.elf userland/xkbcomp.elf userland/test_shared_irq.elf initrd/startx.sh
	dd if=/dev/zero of=disk.img bs=1M count=256
	mkfs.ext3 -F disk.img
	echo "Hello from AscentOS ext2!" > /tmp/ascentos_hello.txt
	echo "This is a test document." > /tmp/ascentos_readme.txt
	debugfs -w -R "cd /" -R "mkdir tmp" disk.img
	debugfs -w -R "write /tmp/ascentos_hello.txt hello.txt" disk.img
	debugfs -w -R "mkdir docs" disk.img
	debugfs -w -R "mkdir bin" disk.img
	debugfs -w -R "write initrd/startx.sh bin/startx.sh" disk.img
	debugfs -w -R "mkdir lib" disk.img
	debugfs -w -R "write toolchain/musl-sysroot/lib/libc.so lib/libc.so" disk.img
	debugfs -w -R "write toolchain/musl-sysroot/lib/libc.so lib/ld-musl-x86_64.so.1" disk.img
	debugfs -w -R "write /tmp/ascentos_readme.txt docs/readme.txt" disk.img
	debugfs -w -R "write userland/test_syscalls.elf bin/test_syscalls" disk.img
	debugfs -w -R "write userland/test_kilo_syscalls.elf bin/test_kilo_syscalls" disk.img
	debugfs -w -R "write userland/test_wait4_complex.elf bin/test_wait4_complex" disk.img
	debugfs -w -R "write userland/kilo.elf bin/kilo" disk.img
	debugfs -w -R "write userland/test_args.elf bin/test_args" disk.img
	debugfs -w -R "write userland/hello.elf bin/hello" disk.img
	debugfs -w -R "write userland/test_cow.elf bin/test_cow" disk.img
	debugfs -w -R "write userland/test_stat.elf bin/test_stat" disk.img
	debugfs -w -R "write userland/ls.elf bin/ls" disk.img
	debugfs -w -R "write userland/readelf.elf bin/readelf" disk.img
	debugfs -w -R "write userland/pong.elf bin/pong" disk.img
	debugfs -w -R "write userland/raycast.elf bin/raycast" disk.img
	debugfs -w -R "write userland/test_mmap_shared_private.elf bin/test_mmap_shared_private" disk.img
	debugfs -w -R "write userland/kria.elf bin/kria" disk.img
	debugfs -w -R "write userland/playwav.elf bin/playwav" disk.img
	debugfs -w -R "write userland/showbmp.elf bin/showbmp" disk.img
	debugfs -w -R "write userland/test_uname_pipe.elf bin/test_uname_pipe" disk.img
	debugfs -w -R "write userland/test_pipe_fork.elf bin/test_pipe_fork" disk.img
	debugfs -w -R "write userland/test_sys_access.elf bin/test_sys_access" disk.img
	debugfs -w -R "write userland/test_sys_cwd.elf bin/test_sys_cwd" disk.img
	debugfs -w -R "write userland/test_newfstatat.elf bin/test_newfstatat" disk.img
	debugfs -w -R "write userland/test_unlink_rename.elf bin/test_unlink_rename" disk.img
	debugfs -w -R "write userland/test_ext3.elf bin/test_ext3" disk.img
	debugfs -w -R "write userland/test_dynamic.elf bin/test_dynamic" disk.img
	debugfs -w -R "write userland/test_tcc_libc.c test_tcc_libc.c" disk.img
	debugfs -w -R "write userland/test_mm.c test_mm.c" disk.img
	debugfs -w -R "write userland/test_dup.elf bin/test_dup" disk.img
	debugfs -w -R "write userland/test_attrib.elf bin/test_attrib" disk.img
	debugfs -w -R "write userland/test_symlink.elf bin/test_symlink" disk.img
	debugfs -w -R "write userland/test_cred.elf bin/test_cred" disk.img
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
	debugfs -w -R "write userland/doom.elf bin/doom" disk.img
	debugfs -w -R "write userland/poll_test.elf bin/poll_test" disk.img
	debugfs -w -R "write userland/test_time.elf bin/test_time" disk.img
	debugfs -w -R "write userland/test_tsc_manual.elf bin/test_tsc_manual" disk.img
	debugfs -w -R "write doomu.wad doom1.wad" disk.img
	debugfs -w -R "write userland/wget.elf bin/wget" disk.img
	debugfs -w -R "write userland/lua.elf bin/lua" disk.img
	debugfs -w -R "write userland/test_unix_sock.elf bin/test_unix_sock" disk.img
	debugfs -w -R "write userland/test_unix_fdpass.elf bin/test_unix_fdpass" disk.img
	debugfs -w -R "write userland/test_fb.elf bin/test_fb" disk.img
	debugfs -w -R "write userland/test_events.elf bin/test_events" disk.img
	debugfs -w -R "write userland/test_shared_irq.elf bin/test_shared_irq" disk.img

	debugfs -w -R "write userland/test_socket_phase3.elf bin/test_socket_phase3" disk.img
	debugfs -w -R "write userland/test_socket_phase3_advanced.elf bin/test_socket_phase3_advanced" disk.img
	debugfs -w -R "write userland/test_socket_phase3_megastress.elf bin/test_socket_phase3_megastress" disk.img
	debugfs -w -R "write userland/test_socket_phase4.elf bin/test_socket_phase4" disk.img
	debugfs -w -R "write userland/test_socket_phase4_detailed.elf bin/test_socket_phase4_detailed" disk.img
	debugfs -w -R "write userland/test_socket_phase5.elf bin/test_socket_phase5" disk.img
	debugfs -w -R "write userland/test_socket_phase6.elf bin/test_socket_phase6" disk.img
	debugfs -w -R "write userland/test_socket_phase7.elf bin/test_socket_phase7" disk.img
	debugfs -w -R "write userland/test_socket_phase7_advanced.elf bin/test_socket_phase7_advanced" disk.img
	debugfs -w -R "write userland/test_socket_phase8.elf bin/test_socket_phase8" disk.img
	debugfs -w -R "write userland/test_socket_phase9.elf bin/test_socket_phase9" disk.img
	debugfs -w -R "write userland/test_socket_phase10.elf bin/test_socket_phase10" disk.img
	debugfs -w -R "write userland/test_socket_phase11.elf bin/test_socket_phase11" disk.img

	debugfs -w -R "write test.tar test.tar" disk.img
	rm -f /tmp/ascentos_hello.txt /tmp/ascentos_readme.txt
	@if [ -d toolchain/musl-sysroot/opt/tcc ]; then \
		echo "Installing TCC into disk image..."; \
		./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/opt/tcc opt/tcc; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/tcc/bin/tcc bin/tcc" disk.img; \
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
		debugfs -w -R "write toolchain/musl-sysroot/opt/bash/bin/bash bin/bash" disk.img; \
		debugfs -w -R "write toolchain/musl-sysroot/opt/bash/bin/bash bin/sh" disk.img; \
		echo "root:x:0:0:root:/root:/bin/bash" > /tmp/passwd; \
		echo "PS1='\033[0;32mRoot@AscentOS\033[0m:\w\\$$ '" > /tmp/bashrc; \
		echo "PATH=/opt/coreutils/bin:/bin:/opt/bash/bin:/opt/tcc/bin" >> /tmp/bashrc; \
		echo "HOME=/root" >> /tmp/bashrc; \
		echo "TERM=vt100" >> /tmp/bashrc; \
		echo "export TERM" >> /tmp/bashrc; \
		debugfs -w -R "mkdir etc" disk.img 2>/dev/null || true; \
		echo "nameserver 10.0.2.3" > /tmp/resolv.conf; \
		echo "127.0.0.1 localhost" > /tmp/hosts; \
		debugfs -w -R "write /tmp/resolv.conf etc/resolv.conf" disk.img; \
		debugfs -w -R "write /tmp/hosts etc/hosts" disk.img; \
		debugfs -w -R "mkdir etc/ssl" disk.img 2>/dev/null || true; \
		debugfs -w -R "mkdir etc/ssl/certs" disk.img 2>/dev/null || true; \
		debugfs -w -R "write /tmp/passwd etc/passwd" disk.img; \
		debugfs -w -R "mkdir root" disk.img 2>/dev/null || true; \
		debugfs -w -R "write /tmp/bashrc root/.bashrc" disk.img; \
		rm -f /tmp/passwd /tmp/bashrc /tmp/resolv.conf /tmp/hosts; \
	fi
	@if [ -f toolchain/musl-sysroot/bin/tar ]; then \
		echo "Installing tar into disk image..."; \
		debugfs -w -R "write toolchain/musl-sysroot/bin/tar bin/tar" disk.img; \
	fi
	@if [ -f userland/Xfbdev.elf ]; then \
		echo "Installing updated Xfbdev server into disk image..."; \
		debugfs -w -R "write userland/Xfbdev.elf bin/Xfbdev" disk.img; \
	elif [ -f toolchain/musl-sysroot/bin/Xfbdev ]; then \
		echo "Installing X11 server into disk image..."; \
		debugfs -w -R "write toolchain/musl-sysroot/bin/Xfbdev bin/Xfbdev" disk.img; \
	fi
	@if [ -f userland/xkbcomp.elf ]; then \
		echo "Installing xkbcomp into disk image..."; \
		debugfs -w -R "write userland/xkbcomp.elf bin/xkbcomp" disk.img; \
	fi
	@if [ -f toolchain/musl-sysroot/bin/Xfbdev ] || [ -f userland/Xfbdev.elf ]; then \
		echo "Installing XKB keyboard data..."; \
		debugfs -w -R "mkdir share" disk.img 2>/dev/null || true; \
		debugfs -w -R "mkdir share/X11" disk.img 2>/dev/null || true; \
		./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/share/X11/xkb share/X11/xkb; \
		./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/share/X11/locale share/X11/locale; \
		debugfs -w -R "write toolchain/musl-sysroot/share/X11/XErrorDB share/X11/XErrorDB" disk.img; \
		debugfs -w -R "write toolchain/musl-sysroot/share/X11/Xcms.txt share/X11/Xcms.txt" disk.img; \
		debugfs -w -R "mkdir share/X11/app-defaults" disk.img 2>/dev/null || true; \
		debugfs -w -R "write build/xterm/xterm-389/XTerm.ad share/X11/app-defaults/XTerm" disk.img; \
		debugfs -w -R "mkdir home" disk.img 2>/dev/null || true; \
		debugfs -w -R "mkdir home/offihito" disk.img 2>/dev/null || true; \
		debugfs -w -R "mkdir home/offihito/AscentOS" disk.img 2>/dev/null || true; \
		debugfs -w -R "mkdir home/offihito/AscentOS/toolchain" disk.img 2>/dev/null || true; \
		debugfs -w -R "symlink home/offihito/AscentOS/toolchain/musl-sysroot /" disk.img; \
		./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/share/fonts share/fonts; \
	fi
	@if [ -f userland/xeyes.elf ]; then \
		echo "Installing xeyes into disk image..."; \
		debugfs -w -R "write userland/xeyes.elf xeyes" disk.img; \
	fi
	@if [ -f userland/twm.elf ]; then \
		echo "Installing twm into disk image..."; \
		debugfs -w -R "write userland/twm.elf twm" disk.img; \
	fi
	@if [ -f userland/xterm.elf ]; then \
		echo "Installing xterm into disk image..."; \
		debugfs -w -R "write userland/xterm.elf bin/xterm" disk.img; \
		if [ -d toolchain/musl-sysroot/share/terminfo ]; then \
			echo "Installing terminfo data..."; \
			debugfs -w -R "mkdir share" disk.img 2>/dev/null || true; \
			./scripts/populate-ext2-dir.sh disk.img toolchain/musl-sysroot/share/terminfo share/terminfo; \
		fi \
	fi
	@if [ -f userland/xeyes.elf ] || [ -f userland/twm.elf ]; then \
		debugfs -w -R "write userland/test_x11_simple.elf bin/test_x11_simple" disk.img; \
		debugfs -w -R "mkdir root" disk.img 2>/dev/null || true; \
		debugfs -w -R "write initrd/.Xauthority root/.Xauthority" disk.img; \
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
clean: clean-musl clean-doom clean-coreutils clean-wolfssl clean-tar
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd

.PHONY: clean-coreutils
clean-coreutils:
	rm -rf build/coreutils-9.5
	rm -rf toolchain/musl-sysroot/opt/coreutils

.PHONY: clean-tar
clean-tar:
	rm -rf build/tar-1.35
	rm -f toolchain/musl-sysroot/bin/tar

.PHONY: clean-wolfssl
clean-wolfssl:
	rm -rf build/wolfssl-5.7.0
	rm -f $(WOLFSSL_LIB)
	rm -rf toolchain/musl-sysroot/include/wolfssl

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

userland/test_cow.elf: userland/test_cow.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_cow.c -o userland/test_cow.elf


userland/test_syscalls.elf: userland/test_syscalls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_syscalls.c -o userland/test_syscalls.elf


userland/test_kilo_syscalls.elf: userland/test_kilo_syscalls.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_kilo_syscalls.c -o userland/test_kilo_syscalls.elf


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

userland/test_socket_phase3.elf: userland/test_socket_phase3.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase3.c -o userland/test_socket_phase3.elf

userland/test_socket_phase3_advanced.elf: userland/test_socket_phase3_advanced.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase3_advanced.c -o userland/test_socket_phase3_advanced.elf

userland/test_socket_phase3_megastress.elf: userland/test_socket_phase3_megastress.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase3_megastress.c -o userland/test_socket_phase3_megastress.elf

userland/test_socket_phase4.elf: userland/test_socket_phase4.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase4.c -o userland/test_socket_phase4.elf

userland/test_socket_phase4_detailed.elf: userland/test_socket_phase4_detailed.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase4_detailed.c -o userland/test_socket_phase4_detailed.elf

userland/test_socket_phase5.elf: userland/test_socket_phase5.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase5.c -o userland/test_socket_phase5.elf

userland/test_socket_phase6.elf: userland/test_socket_phase6.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase6.c -o userland/test_socket_phase6.elf

userland/test_socket_phase7.elf: userland/test_socket_phase7.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase7.c -o userland/test_socket_phase7.elf

userland/test_socket_phase7_advanced.elf: userland/test_socket_phase7_advanced.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase7_advanced.c -o userland/test_socket_phase7_advanced.elf

userland/test_socket_phase8.elf: userland/test_socket_phase8.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase8.c -o userland/test_socket_phase8.elf

userland/test_socket_phase9.elf: userland/test_socket_phase9.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase9.c -o userland/test_socket_phase9.elf

userland/test_socket_phase10.elf: userland/test_socket_phase10.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase10.c -o userland/test_socket_phase10.elf

userland/test_socket_phase11.elf: userland/test_socket_phase11.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_socket_phase11.c -o userland/test_socket_phase11.elf

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

userland/test_time.elf: userland/test_time.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_time.c -o userland/test_time.elf

userland/test_tsc_manual.elf: userland/test_tsc_manual.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_tsc_manual.c -o userland/test_tsc_manual.elf

userland/test_unix_sock.elf: userland/test_unix_sock.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_unix_sock.c -o userland/test_unix_sock.elf

$(WOLFSSL_LIB): $(MUSL_LIBC)
	./scripts/build-wolfssl.sh

.PHONY: wolfssl
wolfssl: $(WOLFSSL_LIB)

userland/wget.elf: $(MUSL_LIBC) $(WOLFSSL_LIB)
	./scripts/build-wget.sh


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

userland/test_unix_fdpass.elf: userland/test_unix_fdpass.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_unix_fdpass.c -o userland/test_unix_fdpass.elf

userland/test_fb.elf: userland/test_fb.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_fb.c -o userland/test_fb.elf

userland/test_events.elf: userland/test_events.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_events.c -o userland/test_events.elf

userland/test_x11_simple.elf: userland/test_x11_simple.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_x11_simple.c -L$(MUSL_SYSROOT)/lib -lX11 -lxcb -lXau -lXdmcp -o userland/test_x11_simple.elf

userland/xkbcomp.elf: scripts/port-x11.sh
	./scripts/port-x11.sh

userland/test_shared_irq.elf: userland/test_shared_irq.c $(MUSL_LIBC)
	PATH="$(MUSL_TOOLCHAIN_BIN):$(PATH)" $(MUSL_CC) $(MUSL_USER_CFLAGS) \
		userland/test_shared_irq.c -o userland/test_shared_irq.elf

.PHONY: all qemu clean
