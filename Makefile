# AscentOS 64-bit Makefile — UNIFIED (tek kernel, gfx ile GUI)
# Ext2 filesystem backend (FAT32'den geçiş yapıldı)

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
	@echo "║  Filesystem: Ext2                                ║"
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

files64.o: fs/files64.c fs/files64.h kernel/ext2.h drivers/ata64.h
	$(CC) $(CFLAGS) -c fs/files64.c -o files64.o

ata64.o: drivers/ata64.c drivers/ata64.h
	$(CC) $(CFLAGS) -c drivers/ata64.c -o ata64.o

# Ext2 filesystem driver (FAT32'nin yerini aldı)
ext2.o: kernel/ext2.c kernel/ext2.h drivers/ata64.h fs/files64.h
	$(CC) $(CFLAGS) -c kernel/ext2.c -o ext2.o

elf64.o: kernel/elf64.c kernel/elf64.h
	$(CC) $(CFLAGS) -c kernel/elf64.c -o elf64.o

pmm.o: kernel/pmm.c kernel/pmm.h
	$(CC) $(CFLAGS) -c kernel/pmm.c -o pmm.o

heap.o: kernel/heap.c kernel/heap.h kernel/pmm.h
	$(CC) $(CFLAGS) -c kernel/heap.c -o heap.o

page_fault.o: kernel/page_fault_handler.c
	$(CC) $(CFLAGS) -c kernel/page_fault_handler.c -o page_fault.o

vmm64.o: kernel/vmm64.c kernel/vmm64.h kernel/pmm.h kernel/heap.h
	$(CC) $(CFLAGS) -c kernel/vmm64.c -o vmm64.o

timer.o: kernel/timer.c kernel/timer.h
	$(CC) $(CFLAGS) -c kernel/timer.c -o timer.o

pcspk.o: drivers/pcspk.c drivers/pcspk.h
	$(CC) $(CFLAGS) -c drivers/pcspk.c -o pcspk.o

sb16.o: drivers/sb16.c drivers/sb16.h
	$(CC) $(CFLAGS) -c drivers/sb16.c -o sb16.o

task.o: kernel/task.c kernel/task.h
	$(CC) $(CFLAGS) -c kernel/task.c -o task.o

scheduler.o: kernel/scheduler.c kernel/scheduler.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/scheduler.c -o scheduler.o

font8x16.o: kernel/font8x16.c kernel/font8x16.h
	$(CC) $(CFLAGS) -c kernel/font8x16.c -o font8x16.o

vesa64.o: drivers/vesa64.c drivers/vesa64.h kernel/font8x16.h
	$(CC) $(CFLAGS) -c drivers/vesa64.c -o vesa64.o

syscall.o: kernel/syscall.c kernel/syscall.h drivers/sb16.h kernel/signal64.h
	$(CC) $(CFLAGS) -c kernel/syscall.c -o syscall.o

signal64.o: kernel/signal64.c kernel/signal64.h kernel/syscall.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/signal64.c -o signal64.o

# ============================================================================
# KERNEL PANIC — VESA framebuffer üzerinde exception ekranı
# ============================================================================
panic64.o: kernel/panic64.c
	$(CC) $(CFLAGS) -c kernel/panic64.c -o panic64.o

rtl8139.o: drivers/rtl8139.c drivers/rtl8139.h
	$(CC) $(CFLAGS) -c drivers/rtl8139.c -o rtl8139.o

arp.o: network/arp.c network/arp.h drivers/rtl8139.h
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

mouse64.o: drivers/mouse64.c drivers/mouse64.h
	$(CC) $(CFLAGS) -c drivers/mouse64.c -o mouse64.o

idt64.o: kernel/idt64.c kernel/idt64.h kernel/task.h
	$(CC) $(CFLAGS) -c kernel/idt64.c -o idt64.o

keyboard.o: drivers/keyboard_unified.c kernel/idt64.h
	$(CC) $(CFLAGS) -c drivers/keyboard_unified.c -o keyboard.o

commands64.o: commands/commands64.c commands/commands64.h \
              drivers/sb16.h drivers/pcspk.h
	$(CC) $(CFLAGS) -c commands/commands64.c -o commands64.o

ipv4.o: network/ipv4.c network/ipv4.h network/arp.h drivers/rtl8139.h
	$(CC) $(CFLAGS) -c network/ipv4.c -o ipv4.o

icmp.o: network/icmp.c network/icmp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/icmp.c -o icmp.o

udp.o: network/udp.c network/udp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/udp.c -o udp.o

dhcp.o: network/dhcp.c network/dhcp.h network/udp.h network/ipv4.h network/arp.h
	$(CC) $(CFLAGS) -c network/dhcp.c -o dhcp.o

tcp.o: network/tcp.c network/tcp.h network/ipv4.h network/arp.h drivers/rtl8139.h
	$(CC) $(CFLAGS) -c network/tcp.c -o tcp.o

http.o: network/http.c network/http.h network/tcp.h network/arp.h network/ipv4.h
	$(CC) $(CFLAGS) -c network/http.c -o http.o

syscalltest64.o: commands/syscalltest64.c commands/commands64.h kernel/syscall.h kernel/signal64.h kernel/task.h
	$(CC) $(CFLAGS) -c commands/syscalltest64.c -o syscalltest64.o

kernel64.o: kernel/kernel64.c drivers/mouse64.h \
            drivers/ata64.h kernel/ext2.h fs/files64.h kernel/cpu64.h \
            drivers/sb16.h
	$(CC) $(CFLAGS) -c kernel/kernel64.c -o kernel64.o

cpu64.o: kernel/cpu64.c kernel/cpu64.h
	$(CC) $(CFLAGS) -c kernel/cpu64.c -o cpu64.o

spinlock64.o: kernel/spinlock64.c kernel/spinlock64.h kernel/cpu64.h
	$(CC) $(CFLAGS) -c kernel/spinlock64.c -o spinlock64.o

KERNEL_OBJS = boot64.o interrupts64.o idt64.o \
              font8x16.o vesa64.o mouse64.o \
              keyboard.o kernel64.o cpu64.o spinlock64.o \
              commands64.o syscalltest64.o files64.o ata64.o ext2.o elf64.o \
              pmm.o heap.o vmm64.o timer.o pcspk.o sb16.o task.o scheduler.o \
              page_fault.o syscall.o signal64.o \
              panic64.o rtl8139.o arp.o ipv4.o icmp.o udp.o dhcp.o tcp.o http.o

kernel64.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o kernel64.elf

# ============================================================================
# DISK IMAGE — Ext2 formatında (FAT32'nin yerini aldı)
#
#  Araç gereksinimi: e2tools veya mke2fs (e2fsprogs paketi)
#    Ubuntu/Debian: sudo apt install e2fsprogs e2tools
#
#  disk.img oluşturma adımları:
#    1. 64 MB ham disk imajı oluştur
#    2. ext2 ile formatla (1024-byte block, "AscentOS" etiketi)
#    3. Temel dizin yapısını kur
#
#  NOT: İlk oluşturmada root yetkisi gerekmez (loop mount yerine
#       e2tools kullanılır). Eğer e2tools yoksa mount -o loop ile
#       alternatif hedef (disk-mount) kullanılabilir.
# ============================================================================
# ── disk.img oluşturma ───────────────────────────────────────────────────────
# Gereksinim: mkfs.ext2  →  sudo pacman -S e2fsprogs
# (CachyOS imza sorunu varsa önce: sudo pacman-key --refresh-keys)
#
# Dizin yapısı debugfs ile oluşturulur (root gerekmez, e2tools gerekmez).
# debugfs e2fsprogs ile birlikte gelir.
# Geçici mount noktası
MNT_TMP := /tmp/ascentos_mnt

disk.img:
	@echo "📀 Ext2 disk imajı oluşturuluyor (2048MB)..."
	@if ! command -v mkfs.ext2 >/dev/null 2>&1; then \
	    echo ""; \
	    echo "HATA: mkfs.ext2 bulunamadi."; \
	    echo "  CachyOS imza sorunu cozumu:"; \
	    echo "    sudo pacman-key --populate archlinux cachyos"; \
	    echo "    sudo pacman -Sy && sudo pacman -S e2fsprogs"; \
	    echo ""; \
	    exit 1; \
	fi
	@# Eski imaj varsa sil (FAT32 veya bozuk olabilir)
	@rm -f disk.img
	dd if=/dev/zero of=disk.img bs=1M count=2048 status=none
	@# ^metadata_csum: eski kernel (4.x) ext2 uyumluluğu için
	mkfs.ext2 -b 1024 -L "AscentOS" -m 0 -O ^metadata_csum,^has_journal disk.img
	@echo "📁 Temel dizin yapısı oluşturuluyor..."
	@$(MAKE) --no-print-directory _disk_mkdirs
	@echo "✓ disk.img hazir (Ext2, 2048MB, block=1024)"
	@echo "  Dogrulama: file disk.img"
	@file disk.img

# İç hedef: debugfs yoksa loop mount dene
_disk_mkdirs:
	@if command -v debugfs >/dev/null 2>&1; then \
	    debugfs -w disk.img -R "mkdir bin"  2>/dev/null; \
	    debugfs -w disk.img -R "mkdir usr"  2>/dev/null; \
	    debugfs -w disk.img -R "mkdir etc"  2>/dev/null; \
	    debugfs -w disk.img -R "mkdir tmp"  2>/dev/null; \
	    debugfs -w disk.img -R "mkdir home" 2>/dev/null; \
	else \
	    echo "⚠ debugfs yok, dizinler ext2_mount sırasında oluşturulacak"; \
	fi

# Eski/bozuk disk.img'i sil ve yeniden oluştur
disk-rebuild:
	@echo "🔄 disk.img yeniden oluşturuluyor..."
	@rm -f disk.img
	@$(MAKE) --no-print-directory disk.img

# Alternatif: loop mount ile dizin oluştur (root yetkisi gerekir)
disk-mount-mkdirs: disk.img
	@echo "🔧 Loop mount ile dizinler oluşturuluyor (sudo gerekli)..."
	mkdir -p /tmp/ascentos_mnt
	sudo mount -o loop disk.img /tmp/ascentos_mnt
	sudo mkdir -p /tmp/ascentos_mnt/{bin,usr,etc,tmp,home}
	sudo umount /tmp/ascentos_mnt
	rmdir /tmp/ascentos_mnt
	@echo "✓ Dizinler oluşturuldu"

AscentOS.iso: kernel64.elf grub64.cfg disk.img
	@echo "📦 Building AscentOS Unified ISO..."
	mkdir -p isodir/boot/grub
	cp kernel64.elf isodir/boot/kernel64.elf
	cp grub64.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o AscentOS.iso isodir 2>&1 | grep -v "xorriso"
	@echo "✓ AscentOS.iso hazir!"


# ============================================================================
# MUSL — build (bir kez çalıştır, kütüphane cache'lenir)
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

USERLAND_LDFLAGS := \
	-T userland/libc/user.ld \
	-static                  \
	-nostdlib                \
	--gc-sections            \
	-u ___errno_location     \
	-u __errno_location

USERLAND_CRT0   := userland/libc/crt0.o
SYSCALLS_OBJ    := userland/out/syscalls.o
USERLAND_APPS   := hello calculator shell snake wav_player
USERLAND_ELFS   := $(addprefix userland/out/, $(addsuffix .elf, $(USERLAND_APPS)))

.PRECIOUS: userland/out/%.o userland/out/%.elf userland/libc/crt0.o $(SYSCALLS_OBJ)

userland: $(MUSL_STAMP) userland/out $(USERLAND_CRT0) $(SYSCALLS_OBJ) $(USERLAND_ELFS)
	@echo "✓ Userland (musl) derlendi → userland/out/"
	@ls -lh userland/out/*.elf

userland/out:
	@mkdir -p userland/out

userland/libc/crt0.o: userland/libc/crt0.asm
	$(AS) $(USERLAND_ASFLAGS) -o $@ $<

$(SYSCALLS_OBJ): userland/libc/syscalls.c | userland/out
	$(USERLAND_CC) -m64 -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INCLUDE) -fno-stack-protector \
	      -ffunction-sections -fdata-sections -O2 -Wall \
	      -Wno-builtin-declaration-mismatch \
	      -fno-builtin-malloc -fno-builtin-free \
	      -c -o $@ $<

userland/out/%.o: userland/apps/%.c | $(MUSL_STAMP)
	$(USERLAND_CC) $(USERLAND_CFLAGS) -c -o $@ $<

userland/out/%.elf: userland/out/%.o $(USERLAND_CRT0) $(SYSCALLS_OBJ) | $(MUSL_STAMP)
	$(USERLAND_LD) $(USERLAND_LDFLAGS) -m elf_x86_64 \
	    --allow-multiple-definition \
	    $(USERLAND_CRT0)           \
	    $<                         \
	    $(SYSCALLS_OBJ)            \
	    -L$(MUSL_LIB) -lc $(LIBGCC) \
	    -o $@
	@echo "  ✓ $@ hazir"

hello: userland/out/hello.elf
	@echo "  ✓ hello.elf hazir"

calculator: userland/out/calculator.elf
	@echo "  ✓ calculator.elf hazir"

snake: userland/out/snake.elf
	@echo "  ✓ snake.elf hazir"

shell: userland/out/shell.elf
	@echo "  ✓ shell.elf hazir"

# ── install-userland ──────────────────────────────────────────────────────────
# ELF'leri Ext2 disk.img'e yaz.
# Öncelik sırası:
#   1. debugfs  (e2fsprogs ile gelir, ROOT GEREKMEz) ← varsayılan
#   2. e2cp     (e2tools, AUR)
#   3. loop mount (sudo gerekir)
install-userland: userland disk.img
	@echo "📦 ELF'ler disk.img'e yaziliyor (Ext2 /bin/)..."
	@if command -v debugfs >/dev/null 2>&1; then \
	    echo "  → debugfs kullaniliyor (root gerekmez)"; \
	    debugfs -w disk.img -R "write userland/out/hello.elf      bin/hello.elf"      2>/dev/null; \
	    debugfs -w disk.img -R "write userland/out/calculator.elf bin/calc.elf"        2>/dev/null; \
	    debugfs -w disk.img -R "write userland/out/snake.elf      bin/snake.elf"       2>/dev/null; \
	    debugfs -w disk.img -R "write userland/out/shell.elf      bin/shell.elf"       2>/dev/null; \
	    debugfs -w disk.img -R "write userland/out/wav_player.elf bin/wav_player.elf"  2>/dev/null; \
	    echo "✓ ELF'ler /bin/'e yazildi (debugfs)"; \
	elif command -v e2cp >/dev/null 2>&1; then \
	    echo "  → e2cp kullaniliyor"; \
	    e2cp userland/out/hello.elf      disk.img:/bin/hello.elf; \
	    e2cp userland/out/calculator.elf disk.img:/bin/calc.elf;  \
	    e2cp userland/out/snake.elf      disk.img:/bin/snake.elf; \
	    e2cp userland/out/shell.elf      disk.img:/bin/shell.elf; \
	    e2cp userland/out/wav_player.elf disk.img:/bin/wav_player.elf; \
	    echo "✓ ELF'ler /bin/'e yazildi (e2cp)"; \
	else \
	    echo "  → loop mount deneniyor (sudo gerekebilir)"; \
	    $(MAKE) --no-print-directory install-userland-mount; \
	fi

# Alternatif: loop mount ile kopyala (sudo gerekir)
install-userland-mount: userland disk.img
	@echo "🔧 Loop mount ile ELF'ler kopyalanıyor..."
	mkdir -p /tmp/ascentos_mnt
	sudo mount -o loop disk.img /tmp/ascentos_mnt
	sudo mkdir -p /tmp/ascentos_mnt/bin
	sudo cp userland/out/hello.elf      /tmp/ascentos_mnt/bin/hello.elf
	sudo cp userland/out/calculator.elf /tmp/ascentos_mnt/bin/calc.elf
	sudo cp userland/out/snake.elf      /tmp/ascentos_mnt/bin/snake.elf
	sudo cp userland/out/shell.elf      /tmp/ascentos_mnt/bin/shell.elf
	sudo cp userland/out/wav_player.elf /tmp/ascentos_mnt/bin/wav_player.elf
	sudo umount /tmp/ascentos_mnt
	rmdir /tmp/ascentos_mnt
	@echo "✓ ELF'ler kopyalandi"

# ── disk içeriğini listele ────────────────────────────────────────────────────
disk-ls:
	@if command -v debugfs >/dev/null 2>&1; then \
	    debugfs disk.img -R "ls bin" 2>/dev/null; \
	elif command -v e2ls >/dev/null 2>&1; then \
	    e2ls disk.img:/bin; \
	else \
	    echo "e2fsprogs kurulu degil (debugfs/e2ls yok)"; \
	fi

# ============================================================================
# RUN / DEBUG
# ============================================================================

run: AscentOS.iso disk.img install-userland
	@echo "▶  AscentOS Unified başlatılıyor..."
	@echo "   Filesystem: Ext2 (disk.img, 64MB)"
	@echo "   TEXT modu açılır. 'gfx' yazınca GUI moduna geçer."
	@echo ""
	@echo "   ── UDP Port Yönlendirme ──────────────────────────────"
	@echo "   host→guest  : nc -u 127.0.0.1 5000   (udplisten 5000 sonrası)"
	@echo "   guest→host  : nc -u -l -p 9999        (udpsend 10.0.2.2 9999 msg)"
	@echo "   ── TCP Port Yönlendirme ──────────────────────────────"
	@echo "   tcplisten   : hostfwd=tcp::8080-:8080  -> nc 127.0.0.1 8080"
	@echo "   tcptest     : nc -l -p 8080 (host'ta) -> tcptest 10.0.2.15 8080"
	@echo "   ── ICMP (ping) için ──────────────────────────────────"
	@echo "   QEMU SLiRP ICMP icin Linux izni gerekir:"
	@echo "   sudo sysctl -w net.ipv4.ping_group_range='0 2147483647'"
	@echo "   ── Ses (SB16) ───────────────────────────────────────"
	@echo "   sb16 tone | sb16 ding | sb16 vol 200 | beep"
	@echo "   wav_player /bin/sound.wav  (WAV oynatici)"
	@echo "   ─────────────────────────────────────────────────────"
	@sudo sysctl -w net.ipv4.ping_group_range="0 2147483647" 2>/dev/null || \
	    echo "   [UYARI] ping_group_range ayarlanamadi — 'ping 1.1.1.1' calismazsa yukardaki komutu elle calistir"
	qemu-system-x86_64 \
	  -cdrom AscentOS.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 1024M -cpu qemu64 -boot d \
	  -serial stdio -vga std \
	  -usb -device usb-tablet \
	  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
	  -device sb16,audiodev=snd0 \
	  -netdev user,id=net0,restrict=off,ipv6=off,hostname=AscentOS,hostfwd=udp::5000-:5000,hostfwd=udp::5001-:5001,hostfwd=udp::5002-:5002,hostfwd=tcp::8080-:8080,hostfwd=tcp::8081-:8081 \
	  -device rtl8139,netdev=net0 \
	  -display gtk,zoom-to-fit=off

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
	  -device sb16,audiodev=snd0 \
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
	@echo "🧹 Build dosyaları temizleniyor..."
	rm -rf *.o *.elf
	rm -rf isodir isodir_text isodir_gui
	rm -rf AscentOS.iso AscentOS-Text.iso AscentOS-GUI.iso
	rm -rf disk.img
	rm -rf userland/out userland/libc/crt0.o
	@echo "✓ Temizlendi!"

musl-clean:
	@echo "🧹 musl cache temizleniyor..."
	rm -rf userland/libc/musl
	rm -rf build-musl
	rm -f  musl-*.tar.gz
	@echo "✓ musl temizlendi. Sonraki 'make userland' yeniden derler."

clean-all: clean musl-clean
	@echo "✓ Tam temizlik tamamlandi."

# ============================================================================
# INFO TARGET
# ============================================================================

info:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Build Information - Ext2 + musl + SYSCALL    ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  💾 Filesystem: Ext2                                      ║"
	@echo "║    • Block size: 1024 byte                                 ║"
	@echo "║    • Disk image: 64 MB (disk.img)                         ║"
	@echo "║    • Direkt + indirect + çift indirect blok desteği       ║"
	@echo "║    • Araç: mkfs.ext2 (e2fsprogs) + e2tools (e2cp/e2ls)   ║"
	@echo "║                                                            ║"
	@echo "║  📦 Userland (musl):                                     ║"
	@echo "║    • malloc, free, realloc                                 ║"
	@echo "║    • printf, fprintf, sprintf, snprintf                    ║"
	@echo "║    • strlen, memcpy, memset, strcmp, strcpy                ║"
	@echo "║    • ELF'ler: /bin/hello.elf, /bin/calc.elf, /bin/shell.elf║"
	@echo "║                                                            ║"
	@echo "║  🚀 SYSCALL Support:                                       ║"
	@echo "║    • fork, execve, waitpid, pipe, dup2                    ║"
	@echo "║    • sigaction, sigprocmask, kill                         ║"
	@echo "║    • open, read, write, close, lseek, stat               ║"
	@echo "║    • getcwd, chdir, opendir, readdir                      ║"
	@echo "║                                                            ║"
	@echo "║  📋 Build Targets:                                         ║"
	@echo "║    make              Kernel + userland derle              ║"
	@echo "║    make musl         musl libc'i derle (ilk kurulum)      ║"
	@echo "║    make disk.img     Ext2 disk imajı oluştur              ║"
	@echo "║    make userland     Userland ELF'leri derle              ║"
	@echo "║    make run          QEMU'da çalıştır                     ║"
	@echo "║    make disk-ls      Ext2 /bin/ içeriğini listele         ║"
	@echo "║    make clean        Build temizle (musl korunur)         ║"
	@echo "║    make clean-all    Her şeyi sıfırla                     ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# HELP
# ============================================================================

help:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Makefile — Ext2 + musl + SYSCALL + Bash      ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  İlk kurulum:                                              ║"
	@echo "║    sudo apt install e2fsprogs e2tools  (araçlar)          ║"
	@echo "║    make musl         musl libc'i cross-compile et         ║"
	@echo "║    make              Her şeyi derle                       ║"
	@echo "║                                                            ║"
	@echo "║  Geliştirme:                                               ║"
	@echo "║    make disk.img     Ext2 disk imajı oluştur (64MB)       ║"
	@echo "║    make userland     Sadece userland ELF'leri derle       ║"
	@echo "║    make hello        Sadece hello.elf derle               ║"
	@echo "║    make calculator   Sadece calculator.elf derle          ║"
	@echo "║    make shell        Sadece shell.elf derle               ║"
	@echo "║    make run          QEMU'da çalıştır                     ║"
	@echo "║    make disk-ls      /bin/ içeriğini listele              ║"
	@echo "║                                                            ║"
	@echo "║  Temizlik:                                                 ║"
	@echo "║    make clean        Build dosyaları (musl korunur)       ║"
	@echo "║    make musl-clean   musl cache sil                       ║"
	@echo "║    make clean-all    Tam sıfırlama                        ║"
	@echo "║                                                            ║"
	@echo "║  Kernel shell komutları:                                   ║"
	@echo "║    elfload /bin/hello.elf                                  ║"
	@echo "║    elfload /bin/calc.elf                                   ║"
	@echo "║    elfload /bin/shell.elf                                  ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# PHONY
# ============================================================================

.PHONY: all run debug net-test gdb musl userland install-userland wav_player \
        install-userland-mount disk-ls disk-mount-mkdirs \
        disk-rebuild _disk_mkdirs \
        hello calculator shell snake \
        clean musl-clean clean-all info help