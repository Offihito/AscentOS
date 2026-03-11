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
	@echo "║  'dhcp'     → Otomatik IP al (10.0.2.15)         ║"
	@echo "║  'tcptest 10.0.2.2 80' → TCP test               ║"
	@echo "║  'tcplisten 8080'      → TCP sunucu              ║"
	@echo "║  'gfx'      → GUI moduna gec                     ║"
	@echo "║  make run   → calistir                           ║"
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

pmm.o: kernel/pmm.c kernel/pmm.h
	$(CC) $(CFLAGS) -c kernel/pmm.c -o pmm.o

heap.o: kernel/heap.c kernel/heap.h kernel/pmm.h
	$(CC) $(CFLAGS) -c kernel/heap.c -o heap.o

# Backward-compat umbrella (artık kullanılmıyor; pmm.o + heap.o'ya bölündü)
# memory_unified.o: kernel/memory_unified.c kernel/memory_unified.h
#	$(CC) $(CFLAGS) -c kernel/memory_unified.c -o memory_unified.o

page_fault.o: kernel/page_fault_handler.c
	$(CC) $(CFLAGS) -c kernel/page_fault_handler.c -o page_fault.o

vmm64.o: kernel/vmm64.c kernel/vmm64.h kernel/pmm.h kernel/heap.h
	$(CC) $(CFLAGS) -c kernel/vmm64.c -o vmm64.o

timer.o: kernel/timer.c kernel/timer.h
	$(CC) $(CFLAGS) -c kernel/timer.c -o timer.o

pcspk.o: kernel/pcspk.c kernel/pcspk.h
	$(CC) $(CFLAGS) -c kernel/pcspk.c -o pcspk.o

task.o: kernel/task.c kernel/task.h
	$(CC) $(CFLAGS) -c kernel/task.c -o task.o

scheduler.o: kernel/scheduler.c kernel/scheduler.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/scheduler.c -o scheduler.o

nano64.o: apps/nano64.c apps/nano64.h
	$(CC) $(CFLAGS) -c apps/nano64.c -o nano64.o

font8x16.o: kernel/font8x16.c kernel/font8x16.h
	$(CC) $(CFLAGS) -c kernel/font8x16.c -o font8x16.o

vesa64.o: kernel/vesa64.c kernel/vesa64.h kernel/font8x16.h
	$(CC) $(CFLAGS) -c kernel/vesa64.c -o vesa64.o

syscall.o: kernel/syscall.c kernel/syscall.h kernel/signal64.h
	$(CC) $(CFLAGS) -c kernel/syscall.c -o syscall.o

signal64.o: kernel/signal64.c kernel/signal64.h kernel/syscall.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/signal64.c -o signal64.o

# ============================================================================
# KERNEL PANIC — VESA framebuffer üzerinde exception ekranı
# ============================================================================
panic64.o: kernel/panic64.c
	$(CC) $(CFLAGS) -c kernel/panic64.c -o panic64.o

rtl8139.o: kernel/rtl8139.c kernel/rtl8139.h
	$(CC) $(CFLAGS) -c kernel/rtl8139.c -o rtl8139.o

arp.o: network/arp.c network/arp.h kernel/rtl8139.h
	$(CC) $(CFLAGS) -c network/arp.c -o arp.o

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

idt64.o: kernel/idt64.c kernel/idt64.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/idt64.c -o idt64.o

keyboard.o: kernel/keyboard_unified.c kernel/idt64.h
	$(CC) $(CFLAGS) -c kernel/keyboard_unified.c -o keyboard.o

taskbar.o: kernel/taskbar64.c kernel/taskbar64.h
	$(CC) $(CFLAGS) -c kernel/taskbar64.c -o taskbar.o

wm64.o: kernel/wm64.c kernel/wm64.h kernel/compositor64.h kernel/taskbar64.h
	$(CC) $(CFLAGS) -c kernel/wm64.c -o wm64.o

commands64.o: apps/commands64.c apps/commands64.h
	$(CC) $(CFLAGS) -c apps/commands64.c -o commands64.o

ipv4.o: network/ipv4.c network/ipv4.h network/arp.h kernel/rtl8139.h
	$(CC) $(CFLAGS) -c network/ipv4.c -o ipv4.o

icmp.o: network/icmp.c network/icmp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/icmp.c -o icmp.o

udp.o: network/udp.c network/udp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/udp.c -o udp.o

dhcp.o: network/dhcp.c network/dhcp.h network/udp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/dhcp.c -o dhcp.o

tcp.o: network/tcp.c network/tcp.h network/ipv4.h network/arp.h kernel/rtl8139.h
	$(CC) $(CFLAGS) -c network/tcp.c -o tcp.o

http.o: network/http.c network/http.h network/tcp.h network/arp.h network/ipv4.h
	$(CC) $(CFLAGS) -c network/http.c -o http.o

syscalltest64.o: apps/syscalltest64.c apps/commands64.h kernel/syscall.h kernel/signal64.h kernel/task.h
	$(CC) $(CFLAGS) -c apps/syscalltest64.c -o syscalltest64.o

kernel64.o: kernel/kernel64.c kernel/gui64.h kernel/mouse64.h kernel/wm64.h
	$(CC) $(CFLAGS) -c kernel/kernel64.c -o kernel64.o

KERNEL_OBJS = boot64.o interrupts64.o idt64.o \
              font8x16.o vesa64.o gui64.o compositor64.o wm64.o mouse64.o \
              keyboard.o kernel64.o taskbar.o \
              commands64.o syscalltest64.o files64.o disk64.o elf64.o nano64.o \
              pmm.o heap.o vmm64.o timer.o pcspk.o task.o scheduler.o \
              page_fault.o syscall.o signal64.o \
              panic64.o rtl8139.o arp.o ipv4.o icmp.o udp.o dhcp.o tcp.o http.o

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
# MUSL — build (bir kez çalıştır, kütüphane cache'lenir)
#
#  Gereksinim: x86_64-elf-gcc PATH'te olmalı
#  Çıktı:     userland/libc/musl/lib/libc.a
#             userland/libc/musl/include/
#
#  Yalnızca libc.a yoksa ya da "make musl" komutuyla çalışır.
# ============================================================================

MUSL_STAMP := userland/libc/musl/lib/libc.a

$(MUSL_STAMP):
	@echo "🔨 musl libc derleniyor (ilk seferde uzun sürer)..."
	@chmod +x musl-build.sh && ./musl-build.sh
	@echo "✓ musl hazir: userland/libc/musl/"

musl: $(MUSL_STAMP)

# ============================================================================
# USERLAND BUILD — musl libc destekli
# ============================================================================

MUSL_INC := userland/libc/musl/include
MUSL_LIB := userland/libc/musl/lib

# ── Userland compiler flags ───────────────────────────────────────────────────
#   -ffreestanding  : host stdlib yok
#   -nostdlib       : otomatik -lc ekleme
#   -nostdinc       : host /usr/include kullanma
#   -isystem        : musl header'ları (sistem header'ı gibi davran, warning bastır)
#   -ffunction/data-sections : kullanılmayan kodu linker temizlesin
#   -mno-red-zone   : kernel ile aynı ABI tutarlılığı için
USERLAND_CFLAGS := \
	-m64                    \
	-ffreestanding          \
	-nostdlib               \
	-nostdinc               \
	-isystem $(GCC_INCLUDE)   \
	-isystem $(MUSL_INC)    \
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
	--gc-sections            \
	-u ___errno_location     \
	-u __errno_location

USERLAND_CRT0   := userland/libc/crt0.o
SYSCALLS_OBJ    := userland/out/syscalls.o
USERLAND_APPS   := hello calculator shell snake
USERLAND_ELFS   := $(addprefix userland/out/, $(addsuffix .elf, $(USERLAND_APPS)))

.PRECIOUS: userland/out/%.o userland/out/%.elf userland/libc/crt0.o $(SYSCALLS_OBJ)

# ── userland ana hedef ───────────────────────────────────────────────────────
userland: $(MUSL_STAMP) userland/out $(USERLAND_CRT0) $(SYSCALLS_OBJ) $(USERLAND_ELFS)
	@echo "✓ Userland (musl) derlendi → userland/out/"
	@ls -lh userland/out/*.elf

userland/out:
	@mkdir -p userland/out

# ── crt0 ─────────────────────────────────────────────────────────────────────
userland/libc/crt0.o: userland/libc/crt0.asm
	$(AS) $(USERLAND_ASFLAGS) -o $@ $<



# ── syscalls.o — bir kez derle, her ELF'e link et ────────────────────────────
#   syscalls.c kendi tiplerini tanımlar, musl header'ına ihtiyaç duymaz.
$(SYSCALLS_OBJ): userland/libc/syscalls.c | userland/out
	$(USERLAND_CC) -m64 -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INCLUDE) -fno-stack-protector \
	      -ffunction-sections -fdata-sections -O2 -Wall \
	      -Wno-builtin-declaration-mismatch \
	      -fno-builtin-malloc -fno-builtin-free \
	      -c -o $@ $<

# ── Uygulama .o ──────────────────────────────────────────────────────────────
userland/out/%.o: userland/apps/%.c | $(MUSL_STAMP)
	$(USERLAND_CC) $(USERLAND_CFLAGS) -c -o $@ $<

# ── ELF linkleme ─────────────────────────────────────────────────────────────
#   Link sırası kritik:
#     crt0.o   → _start tanımı
#     app.o    → main + uygulama kodu
#     syscalls.o → _write, _sbrk, _exit ... (musl'ün çağırdığı stub'lar)
#     -lc      → musl libc.a (malloc, printf, string...)
#     -lgcc    → compiler runtime (__udivdi3, soft-float...)
userland/out/%.elf: userland/out/%.o $(USERLAND_CRT0) $(SYSCALLS_OBJ) | $(MUSL_STAMP)
	$(USERLAND_LD) $(USERLAND_LDFLAGS) -m elf_x86_64 \
	    --allow-multiple-definition \
	    $(USERLAND_CRT0)           \
	    $<                         \
	    $(SYSCALLS_OBJ)            \
	    -L$(MUSL_LIB) -lc $(LIBGCC) \
	    -o $@
	@echo "  ✓ $@ hazir"

# ── Tekil uygulama hedefleri (kolaylık) ──────────────────────────────────────
hello: userland/out/hello.elf
	@echo "  ✓ hello.elf hazir"

calculator: userland/out/calculator.elf
	@echo "  ✓ calculator.elf hazir"

snake: userland/out/snake.elf
	@echo "  ✓ snake.elf hazir"

shell: userland/out/shell.elf
	@echo "  ✓ shell.elf hazir"

# ── install-userland ──────────────────────────────────────────────────────────
install-userland: userland
	@echo "📦 ELF'ler disk.img'e yaziliyor (offset=2048 sektör)..."
	@if [ ! -f disk.img ]; then echo "HATA: disk.img yok"; exit 1; fi
	mcopy -i disk.img@@1048576 -o userland/out/hello.elf      ::HELLO.ELF
	mcopy -i disk.img@@1048576 -o userland/out/calculator.elf ::CALC.ELF
	mcopy -i disk.img@@1048576 -o userland/out/snake.elf      ::SNAKE.ELF
	mcopy -i disk.img@@1048576 -o userland/out/shell.elf      ::SHELL.ELF
	@echo "✓ Yazildi:"
	@mdir -i disk.img@@1048576 :: 2>/dev/null | grep -i elf || true

# ============================================================================
# RUN / DEBUG
# ============================================================================

run: AscentOS.iso disk.img install-userland
	@echo "▶  AscentOS Unified başlatılıyor..."
	@echo "   TEXT modu açılır. 'gfx' yazınca GUI moduna geçer."
	@echo ""
	@echo "   ── UDP Port Yönlendirme ──────────────────────────────"
	@echo "   host→guest  : nc -u 127.0.0.1 5000   (udplisten 5000 sonrası)"
	@echo "   guest→host  : nc -u -l -p 9999        (udpsend 10.0.2.2 9999 msg)"
	@echo "   ── TCP Port Yönlendirme ──────────────────────────────"
	@echo "   tcplisten   : hostfwd=tcp::8080-:8080  -> nc 127.0.0.1 8080"
	@echo "   tcptest     : nc -l -p 8080 (host'ta) -> tcptest 10.0.2.15 8080"
	@echo "   ─────────────────────────────────────────────────────"
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 1024M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
	  -netdev user,id=net0,restrict=off,hostfwd=udp::5000-:5000,hostfwd=udp::5001-:5001,hostfwd=udp::5002-:5002,hostfwd=tcp::8080-:8080,hostfwd=tcp::8081-:8081 \
	  -device rtl8139,netdev=net0 \
	  -display gtk,zoom-to-fit=off

# ARP paketi doğrulama: pcap dump ile gelen/giden frame'leri yakala
# Çalıştırdıktan sonra: tcpdump -r /tmp/ascent_net.pcap arp
net-test: AscentOS.iso disk.img install-userland
	@echo "▶  Ağ testi — ARP paketleri /tmp/ascent_net.pcap'e yakalanıyor..."
	@echo "   Sonuç için: tcpdump -r /tmp/ascent_net.pcap arp"
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 1024M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
	  -netdev user,id=net0,restrict=off,hostfwd=udp::5000-:5000,hostfwd=udp::5001-:5001,hostfwd=tcp::8080-:8080 \
	  -device rtl8139,netdev=net0 \
	  -object filter-dump,id=dump0,netdev=net0,file=/tmp/ascent_net.pcap \
	  -display gtk,zoom-to-fit=off

debug: AscentOS.iso disk.img
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
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

# Kernel + userland object ve ELF'leri temizle (musl cache'ini korur)
clean:
	@echo "🧹 Build dosyaları temizleniyor..."
	rm -rf *.o *.elf
	rm -rf isodir isodir_text isodir_gui
	rm -rf AscentOS.iso AscentOS-Text.iso AscentOS-GUI.iso
	rm -rf disk.img
	rm -rf userland/out userland/libc/crt0.o
	@echo "✓ Temizlendi!"

# musl cache'ini de temizle (yeniden derlemek için)
musl-clean:
	@echo "🧹 musl cache temizleniyor..."
	rm -rf userland/libc/musl
	rm -rf build-musl
	rm -f  musl-*.tar.gz
	@echo "✓ musl temizlendi. Sonraki 'make userland' yeniden derler."

# Her şeyi sıfırla
clean-all: clean musl-clean
	@echo "✓ Tam temizlik tamamlandi."

# ============================================================================
# INFO TARGET
# ============================================================================

info:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Build Information - musl + SYSCALL Edition   ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  📦 Userland (musl):                                     ║"
	@echo "║    • malloc, free, realloc                                 ║"
	@echo "║    • printf, fprintf, sprintf, snprintf                    ║"
	@echo "║    • strlen, memcpy, memset, strcmp, strcpy                ║"
	@echo "║    • atoi, strtol, strtoul, exit                          ║"
	@echo "║    • syscall stub'lar: userland/libc/syscalls.c           ║"
	@echo "║    • ELF'ler: HELLO.ELF, CALC.ELF, SHELL.ELF             ║"
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
	@echo "║    make musl        musl libc'i derle (ilk kurulum)         ║"
	@echo "║    make userland     Userland ELF'leri derle              ║"
	@echo "║    make run          QEMU'da çalıştır                     ║"
	@echo "║    make clean        Build temizle (musl korunur)       ║"
	@echo "║    make clean-all    Her şeyi sıfırla                     ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# HELP
# ============================================================================

help:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Makefile — musl + SYSCALL + Bash Edition     ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  İlk kurulum:                                              ║"
	@echo "║    make musl        musl libc'i cross-compile et            ║"
	@echo "║    make              Her şeyi derle                       ║"
	@echo "║                                                            ║"
	@echo "║                                                            ║"
	@echo "║  Geliştirme:                                               ║"
	@echo "║    make userland     Sadece userland ELF'leri derle       ║"
	@echo "║    make hello        Sadece hello.elf derle               ║"
	@echo "║    make calculator   Sadece calculator.elf derle          ║"
	@echo "║    make shell        Sadece shell.elf (ash) derle         ║"
	@echo "║    make run          QEMU'da çalıştır                     ║"               
	@echo "║                                                            ║"
	@echo "║  Temizlik:                                                 ║"
	@echo "║    make clean        Build dosyaları (musl korunur)     ║"
	@echo "║    make musl-clean    musl cache sil                     ║"
	@echo "║    make clean-all    Tam sıfırlama                        ║"
	@echo "║                                                            ║"
	@echo "║  Kernel shell komutları:                                   ║"
	@echo "║    elfload HELLO.ELF / CALC.ELF / SHELL.ELF              ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# PHONY
# ============================================================================

.PHONY: all run debug net-test gdb musl userland install-userland \
        hello calculator shell \
        clean musl-clean clean-all info help