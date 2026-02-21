# AscentOS 64-bit Makefile
# Unified Boot + Unified Keyboard + SYSCALL (Phase 1) + newlib Userland

CC = gcc
AS = nasm
LD = ld

# ── Userland toolchain ────────────────────────────────────────────────────────
# x86_64-elf-gcc varsa kullan, yoksa sistem gcc'sine düş
USERLAND_CC := $(shell which x86_64-elf-gcc 2>/dev/null || echo gcc)
USERLAND_LD := $(shell which x86_64-elf-ld  2>/dev/null || echo ld)

# libgcc.a tam path — hangi compiler kullanılıyorsa onun runtime'ı
LIBGCC := $(shell $(USERLAND_CC) -m64 --print-libgcc-file-name 2>/dev/null)

# Cross-compiler'ın dahili header dizini (stddef.h, stdarg.h, stdint.h buradadır)
# "gcc -print-file-name=include" → .../lib/gcc/x86_64-elf/<ver>/include
GCC_INCLUDE := $(shell $(USERLAND_CC) -m64 -print-file-name=include 2>/dev/null)

# ── Kernel flags ──────────────────────────────────────────────────────────────
CFLAGS = -m64 -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel \
         -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fno-pic \
         -Wall -Wextra -O2

ASFLAGS = -f elf64
LDFLAGS = -n -T kernel/linker64.ld -nostdlib

# ── Ana hedef ─────────────────────────────────────────────────────────────────
all: AscentOS-Text.iso AscentOS-GUI.iso userland install-userland
	@echo "╔═══════════════════════════════════════════════════╗"
	@echo "║  ✓ AscentOS 64-bit (Unified Boot + Keyboard)     ║"
	@echo "║  ✓ SYSCALL Support Enabled (Phase 1)             ║"
	@echo "║  ✓ Userland (newlib) derlendi                    ║"
	@echo "║  ✓ ELF'ler disk.img'e yazildi (LBA 2048)         ║"
	@echo "║                                                   ║"
	@echo "║  Text Mode:   make run-text                      ║"
	@echo "║  GUI Mode:    make run-gui                       ║"
	@echo "║  Userland:    make userland                      ║"
	@echo "║  newlib:      make newlib                        ║"
	@echo "║                                                   ║"
	@echo "║  🎯 Single keyboard driver for both modes        ║"
	@echo "║  🔧 Single unified bootloader for both modes     ║"
	@echo "║  🚀 Modern SYSCALL/SYSRET interface              ║"
	@echo "║  📦 Kernel'de: elfload HELLO.ELF                 ║"
	@echo "╚═══════════════════════════════════════════════════╝"

# ============================================================================
# SHARED KERNEL COMPONENTS
# ============================================================================

files64.o: fs/files64.c
	$(CC) $(CFLAGS) -c fs/files64.c -o files64.o

disk64.o: kernel/disk64.c
	$(CC) $(CFLAGS) -c kernel/disk64.c -o disk64.o

elf64.o: kernel/elf64.c kernel/elf64.h
	$(CC) $(CFLAGS) -c kernel/elf64.c -o elf64.o

memory_unified.o: kernel/memory_unified.c kernel/memory_unified.h
	$(CC) $(CFLAGS) -c kernel/memory_unified.c -o memory_unified.o

page_fault.o: kernel/page_fault_handler.c
	$(CC) $(CFLAGS) -c kernel/page_fault_handler.c -o page_fault.o

vmm64.o: kernel/vmm64.c kernel/vmm64.h kernel/memory_unified.h
	$(CC) $(CFLAGS) -c kernel/vmm64.c -o vmm64.o

timer.o: kernel/timer.c kernel/timer.h
	$(CC) $(CFLAGS) -c kernel/timer.c -o timer.o

task.o: kernel/task.c kernel/task.h
	$(CC) $(CFLAGS) -c kernel/task.c -o task.o

scheduler.o: kernel/scheduler.c kernel/scheduler.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/scheduler.c -o scheduler.o

nano64.o: apps/nano64.c apps/nano64.h
	$(CC) $(CFLAGS) -c apps/nano64.c -o nano64.o

vga64.o: kernel/vga64.c
	$(CC) $(CFLAGS) -c kernel/vga64.c -o vga64.o

vesa64.o: kernel/vesa64.c kernel/vesa64.h
	$(CC) $(CFLAGS) -c kernel/vesa64.c -o vesa64.o

syscall.o: kernel/syscall.c kernel/syscall.h
	$(CC) $(CFLAGS) -c kernel/syscall.c -o syscall.o

# ============================================================================
# NEWLIB — build (bir kez çalıştır, kütüphane cache'lenir)
#
#  Gereksinim: x86_64-elf-gcc PATH'te olmalı
#  Çıktı:     userland/libc/newlib/lib/libc.a
#             userland/libc/newlib/include/
#
#  Yalnızca libc.a yoksa ya da "make newlib" komutuyla çalışır.
# ============================================================================

NEWLIB_STAMP := userland/libc/newlib/lib/libc.a

$(NEWLIB_STAMP):
	@echo "🔨 newlib derleniyor (ilk seferde uzun sürer)..."
	@chmod +x newlib-build.sh && ./newlib-build.sh
	@echo "✓ newlib hazir: userland/libc/newlib/"

newlib: $(NEWLIB_STAMP)

# ============================================================================
# USERLAND BUILD — newlib destekli
# ============================================================================

NEWLIB_INC := userland/libc/newlib/include
NEWLIB_LIB := userland/libc/newlib/lib

# ── Userland compiler flags ───────────────────────────────────────────────────
#   -ffreestanding  : host stdlib yok
#   -nostdlib       : otomatik -lc ekleme
#   -nostdinc       : host /usr/include kullanma
#   -isystem        : newlib header'ları (sistem header'ı gibi davran, warning bastır)
#   -ffunction/data-sections : kullanılmayan kodu linker temizlesin
#   -mno-red-zone   : kernel ile aynı ABI tutarlılığı için
USERLAND_CFLAGS := \
	-m64                    \
	-ffreestanding          \
	-nostdlib               \
	-nostdinc               \
	-isystem $(GCC_INCLUDE)   \
	-isystem $(NEWLIB_INC)    \
	-ffunction-sections     \
	-fdata-sections         \
	-fno-stack-protector    \
	-mno-red-zone           \
	-O2 -Wall

USERLAND_ASFLAGS := -f elf64

# ── Userland linker flags ─────────────────────────────────────────────────────
#   --gc-sections : kullanılmayan section'ları at → küçük ELF
#   Link sırası:  crt0 → app → syscalls → -lc → $(LIBGCC)
#   $(LIBGCC) = gcc --print-libgcc-file-name çıktısı, tam path olarak geçilir
USERLAND_LDFLAGS := \
	-T userland/libc/user.ld \
	-static                  \
	-nostdlib                \
	--gc-sections

USERLAND_CRT0   := userland/libc/crt0.o
SYSCALLS_OBJ    := userland/out/syscalls.o
USERLAND_APPS   := hello fork_test stdio_test math_test calculator
USERLAND_ELFS   := $(addprefix userland/out/, $(addsuffix .elf, $(USERLAND_APPS)))

.PRECIOUS: userland/out/%.o userland/out/%.elf userland/libc/crt0.o $(SYSCALLS_OBJ)

# ── userland ana hedef ───────────────────────────────────────────────────────
userland: $(NEWLIB_STAMP) userland/out $(USERLAND_CRT0) $(SYSCALLS_OBJ) $(USERLAND_ELFS)
	@echo "✓ Userland (newlib) derlendi → userland/out/"
	@ls -lh userland/out/*.elf

userland/out:
	@mkdir -p userland/out

# ── crt0 ─────────────────────────────────────────────────────────────────────
userland/libc/crt0.o: userland/libc/crt0.asm
	$(AS) $(USERLAND_ASFLAGS) -o $@ $<

# ── syscalls.o — bir kez derle, her ELF'e link et ────────────────────────────
#   syscalls.c kendi tiplerini tanımlar, newlib header'ına ihtiyaç duymaz.
$(SYSCALLS_OBJ): userland/libc/syscalls.c | userland/out
	$(USERLAND_CC) -m64 -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INCLUDE) -fno-stack-protector \
	      -ffunction-sections -fdata-sections -O2 -Wall \
	      -c -o $@ $<

# ── Uygulama .o ──────────────────────────────────────────────────────────────
userland/out/%.o: userland/apps/%.c | $(NEWLIB_STAMP)
	$(USERLAND_CC) $(USERLAND_CFLAGS) -c -o $@ $<

# ── ELF linkleme ─────────────────────────────────────────────────────────────
#   Link sırası kritik:
#     crt0.o   → _start tanımı
#     app.o    → main + uygulama kodu
#     syscalls.o → _write, _sbrk, _exit ... (newlib'in çağırdığı stub'lar)
#     -lc      → newlib libc.a (malloc, printf, string...)
#     -lgcc    → compiler runtime (__udivdi3, soft-float...)
userland/out/%.elf: userland/out/%.o $(USERLAND_CRT0) $(SYSCALLS_OBJ) | $(NEWLIB_STAMP)
	$(USERLAND_LD) $(USERLAND_LDFLAGS) -m elf_x86_64 \
	    $(USERLAND_CRT0)           \
	    $<                         \
	    $(SYSCALLS_OBJ)            \
	    -L$(NEWLIB_LIB) -lc $(LIBGCC) \
	    -o $@
	@echo "  ✓ $@ hazir"

# ── install-userland ──────────────────────────────────────────────────────────
install-userland: userland
	@echo "📦 ELF'ler disk.img'e yaziliyor (offset=2048 sektör)..."
	@if [ ! -f disk.img ]; then echo "HATA: disk.img yok"; exit 1; fi
	mcopy -i disk.img@@1048576 -o userland/out/hello.elf      ::HELLO.ELF
	mcopy -i disk.img@@1048576 -o userland/out/fork_test.elf  ::FORKTEST.ELF
	mcopy -i disk.img@@1048576 -o userland/out/stdio_test.elf ::STDIO.ELF
	mcopy -i disk.img@@1048576 -o userland/out/math_test.elf  ::MATHTEST.ELF
	mcopy -i disk.img@@1048576 -o userland/out/calculator.elf ::CALC.ELF
	@echo "✓ Yazildi:"
	@mdir -i disk.img@@1048576 :: 2>/dev/null | grep -i elf || true

# ============================================================================
# TEXT MODE BUILD
# ============================================================================

boot64_text.o: boot/boot64_unified.asm
	$(AS) $(ASFLAGS) -DTEXT_MODE boot/boot64_unified.asm -o boot64_text.o

interrupts64_text.o: arch/x86_64/interrupts64.asm
	$(AS) $(ASFLAGS) -DTEXT_MODE_BUILD arch/x86_64/interrupts64.asm -o interrupts64_text.o

keyboard_text.o: kernel/keyboard_unified.c
	$(CC) $(CFLAGS) -c kernel/keyboard_unified.c -o keyboard_text.o

commands64_text.o: apps/commands64.c apps/commands64.h
	$(CC) $(CFLAGS) -c apps/commands64.c -o commands64_text.o

kernel64_text.o: kernel/kernel64.c kernel/vesa64.h
	$(CC) $(CFLAGS) -DTEXT_MODE -c kernel/kernel64.c -o kernel64_text.o

TEXT_OBJS = boot64_text.o interrupts64_text.o vesa64.o keyboard_text.o \
            commands64_text.o files64.o disk64.o elf64.o memory_unified.o vmm64.o nano64.o \
            timer.o task.o scheduler.o kernel64_text.o page_fault.o \
            syscall.o

kernel64_text.elf: $(TEXT_OBJS)
	$(LD) $(LDFLAGS) $(TEXT_OBJS) -o kernel64_text.elf

disk.img:
	@echo "📀 Creating 2GB disk image..."
	qemu-img create -f raw disk.img 2G
	@echo "📝 FAT32 format at LBA 2048 (512*2048=1048576 byte offset)..."
	mformat -i disk.img@@1048576 -F -v "ASCENT" -T 4177920 ::
	@echo "✓ Disk image ready (FAT32 @ LBA 2048)"

AscentOS-Text.iso: kernel64_text.elf grub64.cfg disk.img
	@echo "📦 Building Text Mode ISO..."
	mkdir -p isodir_text/boot/grub
	cp kernel64_text.elf isodir_text/boot/kernel64.elf
	cp grub64.cfg isodir_text/boot/grub/grub.cfg
	grub-mkrescue -o AscentOS-Text.iso isodir_text 2>&1 | grep -v "xorriso"
	@echo "✓ Text Mode ISO ready!"

# ============================================================================
# GUI MODE BUILD
# ============================================================================

boot64_gui.o: boot/boot64_unified.asm
	$(AS) $(ASFLAGS) -DGUI_MODE boot/boot64_unified.asm -o boot64_gui.o

interrupts64_gui.o: arch/x86_64/interrupts64.asm
	$(AS) $(ASFLAGS) arch/x86_64/interrupts64.asm -o interrupts64_gui.o

interrupts_setup.o: arch/x86_64/interrupts_setup.c
	$(CC) $(CFLAGS) -c arch/x86_64/interrupts_setup.c -o interrupts_setup.o

gui64.o: kernel/gui64.c kernel/gui64.h
	$(CC) $(CFLAGS) -c kernel/gui64.c -o gui64.o

compositor64.o: kernel/compositor64.c kernel/compositor64.h kernel/gui64.h
	$(CC) $(CFLAGS) -c kernel/compositor64.c -o compositor64.o

mouse64.o: kernel/mouse64.c kernel/mouse64.h
	$(CC) $(CFLAGS) -c kernel/mouse64.c -o mouse64.o

keyboard_gui.o: kernel/keyboard_unified.c
	$(CC) $(CFLAGS) -DGUI_MODE -c kernel/keyboard_unified.c -o keyboard_gui.o

taskbar.o: kernel/taskbar64.c kernel/taskbar64.h
	$(CC) $(CFLAGS) -c kernel/taskbar64.c -o taskbar.o

wm64.o: kernel/wm64.c kernel/wm64.h kernel/compositor64.h kernel/taskbar64.h
	$(CC) $(CFLAGS) -c kernel/wm64.c -o wm64.o

commands_gui.o: kernel/commands_gui.c kernel/commands_gui.h
	$(CC) $(CFLAGS) -DGUI_MODE -c kernel/commands_gui.c -o commands_gui.o

commands64_gui.o: apps/commands64.c apps/commands64.h
	$(CC) $(CFLAGS) -DGUI_MODE -c apps/commands64.c -o commands64_gui.o

kernel64_gui.o: kernel/kernel64.c kernel/gui64.h kernel/mouse64.h kernel/wm64.h
	$(CC) $(CFLAGS) -DGUI_MODE -c kernel/kernel64.c -o kernel64_gui.o

GUI_OBJS = boot64_gui.o interrupts64_gui.o interrupts_setup.o gui64.o compositor64.o \
           wm64.o mouse64.o keyboard_gui.o kernel64_gui.o taskbar.o \
           commands_gui.o memory_unified.o vmm64.o \
           commands64_gui.o files64.o disk64.o elf64.o nano64.o vga64.o \
           timer.o task.o scheduler.o page_fault.o \
           syscall.o

kernel64_gui.elf: $(GUI_OBJS)
	$(LD) $(LDFLAGS) $(GUI_OBJS) -o kernel64_gui.elf

AscentOS-GUI.iso: kernel64_gui.elf grub64.cfg
	@echo "📦 Building GUI Mode ISO..."
	mkdir -p isodir_gui/boot/grub
	cp kernel64_gui.elf isodir_gui/boot/kernel64.elf
	cp grub64.cfg isodir_gui/boot/grub/grub.cfg
	grub-mkrescue -o AscentOS-GUI.iso isodir_gui 2>&1 | grep -v "xorriso"
	@echo "✓ GUI Mode ISO ready!"

# ============================================================================
# RUN TARGETS
# ============================================================================

run-text: AscentOS-Text.iso disk.img install-userland
	@echo "╔════════════════════════════════════════════╗"
	@echo "║   AscentOS Text Mode (Unified)            ║"
	@echo "║   + SYSCALL Support (Phase 1)             ║"
	@echo "║   📦 ELF'ler disk'te hazir                ║"
	@echo "╚════════════════════════════════════════════╝"
	@echo ""
	@echo "Kernel komutlari:"
	@echo "  elfload HELLO.ELF"
	@echo "  elfload FORKTEST.ELF"
	@echo "  elfload STDIO.ELF"
	@echo "  elfload MATHTEST.ELF"
	@echo "  elfload CALC.ELF"
	@echo ""
	qemu-system-x86_64 \
	  -cdrom AscentOS-Text.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 1024M \
	  -cpu qemu64 \
	  -boot d \
	  -serial stdio \
	  -vga std \
	  -display gtk,zoom-to-fit=off

run-gui: AscentOS-GUI.iso disk.img install-userland
	@echo "╔════════════════════════════════════════════╗"
	@echo "║   AscentOS GUI Mode (Unified)             ║"
	@echo "║   + SYSCALL Support (Phase 1)             ║"
	@echo "║   📦 ELF'ler disk'te hazir                ║"
	@echo "╚════════════════════════════════════════════╝"
	@echo ""
	@echo "🎹 Using unified keyboard driver"
	@echo "🔧 Using unified bootloader (4GB mapped)"
	@echo "🚀 SYSCALL/SYSRET enabled"
	@echo ""
	@echo "🎯 GUI Features:"
	@echo "  • Windows 7 style Start Menu"
	@echo "  • Taskbar with clock"
	@echo "  • Terminal window"
	@echo "  • Desktop icons"
	@echo ""
	qemu-system-x86_64 \
	  -cdrom AscentOS-GUI.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M \
	  -cpu qemu64 \
	  -boot d \
	  -serial stdio \
	  -vga std

run: run-text

# ============================================================================
# DEBUG TARGETS
# ============================================================================

debug-text: AscentOS-Text.iso disk.img
	@echo "🐛 Starting debug mode (Text - Unified + SYSCALL)..."
	qemu-system-x86_64 \
	  -cdrom AscentOS-Text.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M -cpu qemu64 -boot d -serial stdio \
	  -vga std -display gtk,zoom-to-fit=off \
	  -s -S

debug-gui: AscentOS-GUI.iso
	@echo "🐛 Starting debug mode (GUI - Unified + SYSCALL)..."
	qemu-system-x86_64 \
	  -cdrom AscentOS-GUI.iso \
	  -m 512M -cpu qemu64 -boot d -serial stdio -vga std \
	  -s -S

# GDB ile bağlan (debug-* çalışırken ayrı terminalde)
gdb-text:
	gdb -ex "target remote :1234" \
	    -ex "symbol-file kernel64_text.elf" \
	    -ex "set architecture i386:x86-64"

gdb-gui:
	gdb -ex "target remote :1234" \
	    -ex "symbol-file kernel64_gui.elf" \
	    -ex "set architecture i386:x86-64"

# ============================================================================
# CLEAN
# ============================================================================

# Kernel + userland object ve ELF'leri temizle (newlib cache'ini korur)
clean:
	@echo "🧹 Build dosyaları temizleniyor..."
	rm -rf *.o *.elf
	rm -rf isodir_text isodir_gui
	rm -rf AscentOS-Text.iso AscentOS-GUI.iso
	rm -rf disk.img
	rm -rf userland/out userland/libc/crt0.o
	@echo "✓ Temizlendi! (newlib cache korundu → make newlib-clean ile silinir)"

# newlib cache'ini de temizle (yeniden derlemek için)
newlib-clean:
	@echo "🧹 newlib cache temizleniyor..."
	rm -rf userland/libc/newlib
	rm -rf build-newlib
	rm -f  newlib-*.tar.gz
	@echo "✓ newlib temizlendi. Sonraki 'make userland' yeniden derler."

# Her şeyi sıfırla
clean-all: clean newlib-clean
	@echo "✓ Tam temizlik tamamlandi."

# ============================================================================
# INFO TARGET
# ============================================================================

info:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Build Information - newlib + SYSCALL Edition   ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  📦 Userland (newlib):                                     ║"
	@echo "║    • malloc, free, realloc                                 ║"
	@echo "║    • printf, fprintf, sprintf, snprintf                    ║"
	@echo "║    • strlen, memcpy, memset, strcmp, strcpy                ║"
	@echo "║    • atoi, strtol, strtoul, exit                          ║"
	@echo "║    • syscall stub'lar: userland/libc/syscalls.c           ║"
	@echo "║                                                            ║"
	@echo "║  🚀 SYSCALL Support (Phase 1):                             ║"
	@echo "║    • Modern SYSCALL/SYSRET instructions                    ║"
	@echo "║    • MSR-based configuration                               ║"
	@echo "║    • Statistics and debugging                              ║"
	@echo "║                                                            ║"
	@echo "║  🎹 Unified Keyboard Driver:                               ║"
	@echo "║    • Single source for both modes (GUI_MODE flag)          ║"
	@echo "║    • Full nano editor + terminal support                   ║"
	@echo "║                                                            ║"
	@echo "║  🔧 Bootloader:                                            ║"
	@echo "║    • Single unified bootloader (TEXT + GUI)                ║"
	@echo "║    • 4GB identity mapped, VESA framebuffer                 ║"
	@echo "║                                                            ║"
	@echo "║  📋 Build Targets:                                         ║"
	@echo "║    make              Build everything                      ║"
	@echo "║    make newlib       newlib'i derle (ilk kurulum)         ║"
	@echo "║    make userland     Userland ELF'leri derle              ║"
	@echo "║    make run-text     Text mode çalıştır                   ║"
	@echo "║    make run-gui      GUI mode çalıştır                    ║"
	@echo "║    make debug-text   GDB ile text mode                    ║"
	@echo "║    make debug-gui    GDB ile GUI mode                     ║"
	@echo "║    make clean        Build temizle (newlib korunur)       ║"
	@echo "║    make newlib-clean newlib cache'ini sil                 ║"
	@echo "║    make clean-all    Her şeyi sıfırla                     ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# HELP
# ============================================================================

help:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Makefile — newlib + SYSCALL Edition            ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  İlk kurulum:                                              ║"
	@echo "║    make newlib       newlib'i cross-compile et            ║"
	@echo "║    make              Her şeyi derle                       ║"
	@echo "║                                                            ║"
	@echo "║  Geliştirme:                                               ║"
	@echo "║    make userland     Sadece userland ELF'leri derle       ║"
	@echo "║    make run-text     Build + Text mode QEMU               ║"
	@echo "║    make run-gui      Build + GUI mode QEMU                ║"
	@echo "║    make run          run-text ile aynı (default)          ║"
	@echo "║                                                            ║"
	@echo "║  Debug:                                                    ║"
	@echo "║    make debug-text   QEMU -s -S (Text mode)               ║"
	@echo "║    make debug-gui    QEMU -s -S (GUI mode)                ║"
	@echo "║    make gdb-text     GDB bağlan (text kernel)             ║"
	@echo "║    make gdb-gui      GDB bağlan (GUI kernel)              ║"
	@echo "║                                                            ║"
	@echo "║  Temizlik:                                                 ║"
	@echo "║    make clean        Build dosyaları (newlib korunur)     ║"
	@echo "║    make newlib-clean newlib cache sil + yeniden derle     ║"
	@echo "║    make clean-all    Tam sıfırlama                        ║"
	@echo "║                                                            ║"
	@echo "║  Bilgi:                                                    ║"
	@echo "║    make info         Detaylı build bilgisi                ║"
	@echo "║    make help         Bu menü                              ║"
	@echo "║                                                            ║"
	@echo "║  Kernel shell komutları:                                   ║"
	@echo "║    elfload HELLO.ELF / FORKTEST.ELF / STDIO.ELF           ║"
	@echo "║    elfload MATHTEST.ELF / CALC.ELF                        ║"
	@echo "║    testsyscall / syscallstats                              ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# PHONY
# ============================================================================

.PHONY: all run run-text run-gui \
        debug-text debug-gui gdb-text gdb-gui \
        newlib userland install-userland \
        clean newlib-clean clean-all \
        info help