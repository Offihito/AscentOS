# AscentOS 64-bit Makefile — UNIFIED (tek kernel, gfx ile GUI)

CC = gcc
AS = nasm
LD = ld

USERLAND_CC := $(shell which x86_64-elf-gcc 2>/dev/null || echo gcc)
USERLAND_LD := $(shell which x86_64-elf-ld  2>/dev/null || echo ld)
LIBGCC := $(shell $(USERLAND_CC) -m64 --print-libgcc-file-name 2>/dev/null)
GCC_INCLUDE := $(shell $(USERLAND_CC) -m64 -print-file-name=include 2>/dev/null)

# Kernel derlerken TEXT_MODE veya GUI_MODE tanımlanmaz.
# keyboard_unified.c runtime'da kernel_mode değişkenini okur.
CFLAGS = -m64 -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel \
         -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fno-pic \
         -Wall -Wextra -O2

ASFLAGS  = -f elf64
# Boot'u GUI_MODE flag'iyle derliyoruz: Multiboot2 framebuffer tag'i
# yüksek çözünürlük (1920x1080) ister ve isr_mouse export edilir.
BOOT_ASFLAGS = -f elf64 -DGUI_MODE

LDFLAGS  = -n -T kernel/linker64.ld -nostdlib

# ── Ana hedef ────────────────────────────────────────────────────────────────
all: AscentOS.iso userland install-userland
	@echo "╔═══════════════════════════════════════════════════╗"
	@echo "║  AscentOS Unified Kernel hazir                   ║"
	@echo "║  Baslangic: TEXT terminali                       ║"
	@echo "║  'gfx' komutu: GUI moduna gec                    ║"
	@echo "║  make run   — calistir                           ║"
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

vesa64.o: kernel/vesa64.c kernel/vesa64.h
	$(CC) $(CFLAGS) -c kernel/vesa64.c -o vesa64.o

syscall.o: kernel/syscall.c kernel/syscall.h kernel/signal64.h
	$(CC) $(CFLAGS) -c kernel/syscall.c -o syscall.o

syscall_test.o: kernel/syscall_test.c kernel/syscall.h
	$(CC) $(CFLAGS) -c kernel/syscall_test.c -o syscall_test.o

signal64.o: kernel/signal64.c kernel/signal64.h kernel/syscall.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/signal64.c -o signal64.o

# ============================================================================
# UNIFIED KERNEL OBJECTS
# ============================================================================

boot64.o: boot/boot64_unified.asm
	$(AS) $(BOOT_ASFLAGS) boot/boot64_unified.asm -o boot64.o

# interrupts64.asm — GUI_MODE olmadan derlersek isr_mouse export edilmez.
# BOOT_ASFLAGS (GUI_MODE tanımlı) ile derliyoruz → mouse IRQ dahil.
interrupts64.o: arch/x86_64/interrupts64.asm
	$(AS) $(BOOT_ASFLAGS) arch/x86_64/interrupts64.asm -o interrupts64.o

gui64.o: kernel/gui64.c kernel/gui64.h
	$(CC) $(CFLAGS) -c kernel/gui64.c -o gui64.o

compositor64.o: kernel/compositor64.c kernel/compositor64.h kernel/gui64.h
	$(CC) $(CFLAGS) -c kernel/compositor64.c -o compositor64.o

mouse64.o: kernel/mouse64.c kernel/mouse64.h
	$(CC) $(CFLAGS) -c kernel/mouse64.c -o mouse64.o

keyboard.o: kernel/keyboard_unified.c
	$(CC) $(CFLAGS) -c kernel/keyboard_unified.c -o keyboard.o

taskbar.o: kernel/taskbar64.c kernel/taskbar64.h
	$(CC) $(CFLAGS) -c kernel/taskbar64.c -o taskbar.o

wm64.o: kernel/wm64.c kernel/wm64.h kernel/compositor64.h kernel/taskbar64.h
	$(CC) $(CFLAGS) -c kernel/wm64.c -o wm64.o

commands64.o: apps/commands64.c apps/commands64.h
	$(CC) $(CFLAGS) -c apps/commands64.c -o commands64.o

syscalltest64.o: apps/syscalltest64.c apps/commands64.h kernel/syscall.h kernel/signal64.h kernel/task.h
	$(CC) $(CFLAGS) -c apps/syscalltest64.c -o syscalltest64.o

kernel64.o: kernel/kernel64.c kernel/gui64.h kernel/mouse64.h kernel/wm64.h
	$(CC) $(CFLAGS) -c kernel/kernel64.c -o kernel64.o

KERNEL_OBJS = boot64.o interrupts64.o \
              vesa64.o gui64.o compositor64.o wm64.o mouse64.o \
              keyboard.o kernel64.o taskbar.o \
              commands64.o syscalltest64.o files64.o disk64.o elf64.o nano64.o \
              memory_unified.o vmm64.o timer.o task.o scheduler.o \
              page_fault.o syscall.o syscall_test.o signal64.o

kernel64.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o kernel64.elf

disk.img:
	@echo "📀 Creating 2GB disk image..."
	qemu-img create -f raw disk.img 2G
	mformat -i disk.img@@1048576 -F -v "ASCENT" -T 4177920 ::
	@echo "✓ Disk image ready"

AscentOS.iso: kernel64.elf grub64.cfg disk.img
	@echo "📦 Building AscentOS Unified ISO..."
	mkdir -p isodir/boot/grub
	cp kernel64.elf isodir/boot/kernel64.elf
	cp grub64.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o AscentOS.iso isodir 2>&1 | grep -v "xorriso"
	@echo "✓ AscentOS.iso hazir!"



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
USERLAND_APPS   := hello calculator
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
	mcopy -i disk.img@@1048576 -o userland/out/calculator.elf ::CALC.ELF
	@echo "✓ Yazildi:"
	@mdir -i disk.img@@1048576 :: 2>/dev/null | grep -i elf || true

# ============================================================================
# RUN / DEBUG
# ============================================================================

run: AscentOS.iso disk.img install-userland
	@echo "▶  AscentOS Unified başlatılıyor..."
	@echo "   TEXT modu açılır. 'gfx' yazınca GUI moduna geçer."
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 1024M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -display gtk,zoom-to-fit=off

debug: AscentOS.iso disk.img
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -s -S

gdb:
	gdb -ex "target remote :1234" \
	    -ex "symbol-file kernel64.elf" \
	    -ex "set architecture i386:x86-64"


# ============================================================================
# CLEAN
# ============================================================================

# Kernel + userland object ve ELF'leri temizle (newlib cache'ini korur)
clean:
	@echo "🧹 Build dosyaları temizleniyor..."
	rm -rf *.o *.elf
	rm -rf isodir isodir_text isodir_gui
	rm -rf AscentOS.iso AscentOS-Text.iso AscentOS-GUI.iso
	rm -rf disk.img
	rm -rf userland/out userland/libc/crt0.o
	@echo "✓ Temizlendi!"

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
	@echo "║                                                            ║"
	@echo "║  🚀 SYSCALL Support:                                       ║"
	@echo "║    • fork, execve, waitpid, pipe, dup2                    ║"
	@echo "║    • sigaction, sigprocmask, kill                         ║"
	@echo "║    • open, read, write, close, lseek, stat               ║"
	@echo "║    • getcwd, chdir, opendir, readdir                      ║"
	@echo "║    • setpgid, setsid, tcsetpgrp (job control)             ║"
	@echo "║                                                            ║"
	@echo "║  📋 Build Targets:                                         ║"
	@echo "║    make              Kernel + userland derle              ║"
	@echo "║    make newlib       newlib'i derle (ilk kurulum)         ║"
	@echo "║    make userland     Userland ELF'leri derle              ║"
	@echo "║    make run          QEMU'da çalıştır                     ║"
	@echo "║    make clean        Build temizle (newlib korunur)       ║"
	@echo "║    make clean-all    Her şeyi sıfırla                     ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# HELP
# ============================================================================

help:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Makefile — newlib + SYSCALL + Bash Edition     ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  İlk kurulum:                                              ║"
	@echo "║    make newlib       newlib'i cross-compile et            ║"
	@echo "║    make              Her şeyi derle                       ║"
	@echo "║                                                            ║" 
	@echo "║                                                            ║"
	@echo "║  Geliştirme:                                               ║"
	@echo "║    make userland     Sadece userland ELF'leri derle       ║"
	@echo "║    make run          QEMU'da çalıştır                     ║"               
	@echo "║                                                            ║"
	@echo "║  Temizlik:                                                 ║"
	@echo "║    make clean        Build dosyaları (newlib korunur)     ║"
	@echo "║    make newlib-clean newlib cache sil                     ║"
	@echo "║    make clean-all    Tam sıfırlama                        ║"
	@echo "║                                                            ║"
	@echo "║  Kernel shell komutları:                                   ║"
	@echo "║    elfload HELLO.ELF / CALC.ELF                           ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# PHONY
# ============================================================================

.PHONY: all run debug gdb newlib userland install-userland \
        clean newlib-clean clean-all info help