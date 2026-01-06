# AscentOS 64-bit Makefile - With Network + ARP + Start Menu

CC = gcc
AS = nasm
LD = ld

# 64-bit flags
CFLAGS = -m64 -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel \
         -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fno-pic \
         -Wall -Wextra -O2

ASFLAGS = -f elf64
LDFLAGS = -n -T kernel/linker64.ld -nostdlib

# Main target
all: AscentOS-Text.iso AscentOS-GUI.iso
	@echo "╔═══════════════════════════════════════════════════╗"
	@echo "║  ✓ AscentOS 64-bit with Start Menu!              ║"
	@echo "║                                                   ║"
	@echo "║  Text Mode:   make run-text                      ║"
	@echo "║  GUI Mode:    make run-gui                       ║"
	@echo "║                                                   ║"
	@echo "║  🌐 Network: RTL8139 + ARP + ICMP                ║"
	@echo "║  🎯 GUI: Start Menu + Taskbar + Terminal         ║"
	@echo "╚═══════════════════════════════════════════════════╝"

# ============================================================================
# NETWORK MODULE
# ============================================================================

network64.o: kernel/network64.c kernel/network64.h
	$(CC) $(CFLAGS) -c kernel/network64.c -o network64.o

icmp64.o: kernel/icmp64.c kernel/icmp64.h kernel/network64.h
	$(CC) $(CFLAGS) -c kernel/icmp64.c -o icmp64.o

arp64.o: kernel/arp64.c kernel/arp64.h kernel/network64.h
	$(CC) $(CFLAGS) -c kernel/arp64.c -o arp64.o

# ============================================================================
# TEXT MODE BUILD
# ============================================================================

boot64.o: kernel/boot64.asm
	$(AS) $(ASFLAGS) kernel/boot64.asm -o boot64.o

interrupts64_text.o: kernel/interrupts64.asm
	$(AS) $(ASFLAGS) -DTEXT_MODE_BUILD kernel/interrupts64.asm -o interrupts64_text.o

vga64.o: kernel/vga64.c
	$(CC) $(CFLAGS) -c kernel/vga64.c -o vga64.o

keyboard64.o: kernel/keyboard64.c
	$(CC) $(CFLAGS) -c kernel/keyboard64.c -o keyboard64.o

accounts64.o: kernel/accounts64.c kernel/accounts64.h
	$(CC) $(CFLAGS) -c kernel/accounts64.c -o accounts64.o

commands64_text.o: kernel/commands64.c kernel/commands64.h kernel/script64.h kernel/accounts64.h kernel/network64.h kernel/icmp64.h kernel/arp64.h
	$(CC) $(CFLAGS) -c kernel/commands64.c -o commands64_text.o

script64.o: kernel/script64.c kernel/script64.h
	$(CC) $(CFLAGS) -c kernel/script64.c -o script64.o

udp64.o: kernel/udp64.c kernel/udp64.h kernel/network64.h
	$(CC) $(CFLAGS) -c kernel/udp64.c -o udp64.o

files64.o: kernel/files64.c
	$(CC) $(CFLAGS) -c kernel/files64.c -o files64.o

disk64.o: kernel/disk64.c
	$(CC) $(CFLAGS) -c kernel/disk64.c -o disk64.o

memory64.o: kernel/memory64.c
	$(CC) $(CFLAGS) -c kernel/memory64.c -o memory64.o

kernel64_text.o: kernel/kernel64.c
	$(CC) $(CFLAGS) -DTEXT_MODE -c kernel/kernel64.c -o kernel64_text.o

nano64.o: kernel/nano64.c kernel/nano64.h
	$(CC) $(CFLAGS) -c kernel/nano64.c -o nano64.o

TEXT_OBJS = boot64.o interrupts64_text.o vga64.o keyboard64.o \
            commands64_text.o script64.o files64.o disk64.o memory64.o nano64.o \
            accounts64.o network64.o icmp64.o arp64.o udp64.o \
            task64.o task_switch.o kernel64_text.o

kernel64_text.elf: $(TEXT_OBJS)
	$(LD) $(LDFLAGS) $(TEXT_OBJS) -o kernel64_text.elf

disk.img:
	@echo "📀 Creating disk image..."
	qemu-img create -f raw disk.img 100M
	@echo "✓ Disk image ready!"

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

boot64_gui.o: kernel/boot64_gui.asm
	$(AS) $(ASFLAGS) kernel/boot64_gui.asm -o boot64_gui.o

interrupts64_gui.o: kernel/interrupts64.asm
	$(AS) $(ASFLAGS) kernel/interrupts64.asm -o interrupts64_gui.o

interrupts_setup.o: kernel/interrupts_setup.c
	$(CC) $(CFLAGS) -c kernel/interrupts_setup.c -o interrupts_setup.o

task64.o: kernel/task64.c kernel/task64.h
	$(CC) $(CFLAGS) -c kernel/task64.c -o task64.o

task_switch.o: kernel/task_switch.asm
	$(AS) $(ASFLAGS) kernel/task_switch.asm -o task_switch.o

gui64.o: kernel/gui64.c kernel/gui64.h
	$(CC) $(CFLAGS) -c kernel/gui64.c -o gui64.o

mouse64.o: kernel/mouse64.c kernel/mouse64.h
	$(CC) $(CFLAGS) -c kernel/mouse64.c -o mouse64.o

keyboard_gui.o: kernel/keyboard_gui.c
	$(CC) $(CFLAGS) -c kernel/keyboard_gui.c -o keyboard_gui.o

taskbar.o: kernel/taskbar64.c kernel/taskbar64.h
	$(CC) $(CFLAGS) -c kernel/taskbar64.c -o taskbar.o

startmenu.o: kernel/startmenu64.c kernel/startmenu64.h
	$(CC) $(CFLAGS) -c kernel/startmenu64.c -o startmenu.o

terminal64.o: kernel/terminal64.c kernel/terminal64.h kernel/gui64.h kernel/commands_gui.h
	$(CC) $(CFLAGS) -c kernel/terminal64.c -o terminal64.o

commands_gui.o: kernel/commands_gui.c kernel/commands_gui.h kernel/terminal64.h
	$(CC) $(CFLAGS) -DGUI_MODE -c kernel/commands_gui.c -o commands_gui.o

memory_gui.o: kernel/memory_gui.c kernel/memory_gui.h
	$(CC) $(CFLAGS) -c kernel/memory_gui.c -o memory_gui.o

wallpaper64.o: kernel/wallpaper64.c kernel/wallpaper64.h
	$(CC) $(CFLAGS) -c kernel/wallpaper64.c -o wallpaper64.o

commands64_gui.o: kernel/commands64.c kernel/commands64.h kernel/script64.h kernel/accounts64.h kernel/network64.h kernel/icmp64.h kernel/arp64.h
	$(CC) $(CFLAGS) -DGUI_MODE -c kernel/commands64.c -o commands64_gui.o

kernel64_gui.o: kernel/kernel64.c kernel/gui64.h kernel/mouse64.h kernel/terminal64.h kernel/wallpaper64.h kernel/startmenu64.h
	$(CC) $(CFLAGS) -DGUI_MODE -c kernel/kernel64.c -o kernel64_gui.o

GUI_OBJS = boot64_gui.o interrupts64_gui.o interrupts_setup.o gui64.o \
           mouse64.o keyboard_gui.o kernel64_gui.o taskbar.o startmenu.o terminal64.o \
           commands_gui.o memory_gui.o wallpaper64.o \
           commands64_gui.o script64.o files64.o disk64.o memory64.o nano64.o vga64.o \
           accounts64.o network64.o icmp64.o arp64.o udp64.o \
           task64.o task_switch.o
		   
kernel64_gui.elf: $(GUI_OBJS)
	$(LD) $(LDFLAGS) $(GUI_OBJS) -o kernel64_gui.elf

AscentOS-GUI.iso: kernel64_gui.elf grub64.cfg
	@echo "📦 Building GUI Mode ISO with Start Menu..."
	mkdir -p isodir_gui/boot/grub
	cp kernel64_gui.elf isodir_gui/boot/kernel64.elf
	cp grub64.cfg isodir_gui/boot/grub/grub.cfg
	grub-mkrescue -o AscentOS-GUI.iso isodir_gui 2>&1 | grep -v "xorriso"
	@echo "✓ GUI Mode ISO ready with Start Menu!"

# ============================================================================
# RUN TARGETS
# ============================================================================

run-text: AscentOS-Text.iso disk.img
	@echo "╔════════════════════════════════════════════╗"
	@echo "║   AscentOS Text Mode + Network             ║"
	@echo "╚════════════════════════════════════════════╝"
	@echo ""
	@echo "🌐 Network Features:"
	@echo "  • MAC address detection"
	@echo "  • IP configuration"
	@echo "  • ARP protocol (address resolution)"
	@echo "  • ICMP/Ping support"
	@echo ""
	@echo "📡 Network Commands:"
	@echo "  ifconfig       - Show/configure network"
	@echo "  ping <ip>      - Ping a host"
	@echo "  arp            - ARP cache management"
	@echo "  netstat        - Network statistics"
	@echo ""
	qemu-system-x86_64 \
	  -cdrom AscentOS-Text.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M \
	  -cpu qemu64 \
	  -boot d \
	  -serial stdio \
	  -display gtk \
	  -netdev user,id=net0 \
	  -device rtl8139,netdev=net0 \
	  -no-reboot

run-gui: AscentOS-GUI.iso disk.img
	@echo "╔════════════════════════════════════════════╗"
	@echo "║   AscentOS GUI Mode + Start Menu           ║"
	@echo "╚════════════════════════════════════════════╝"
	@echo ""
	@echo "🎯 GUI Features:"
	@echo "  • Windows 7 style Start Menu"
	@echo "  • Taskbar with clock"
	@echo "  • Terminal window"
	@echo "  • Desktop icons"
	@echo ""
	@echo "🌐 Network Support:"
	@echo "  • Complete network stack"
	@echo "  • ARP + ICMP protocols"
	@echo ""
	@echo "📡 Try these in terminal:"
	@echo "  ifconfig, ping, arp, netstat"
	@echo ""
	qemu-system-x86_64 \
	  -cdrom AscentOS-GUI.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M \
	  -cpu qemu64 \
	  -boot d \
	  -serial stdio \
	  -vga std \
	  -netdev user,id=net0 \
	  -device rtl8139,netdev=net0 \
	  -no-reboot

run: run-text

# ============================================================================
# INFO TARGET
# ============================================================================

info:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║        AscentOS Build Information - Start Menu Edition    ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  📦 Build Targets:                                         ║"
	@echo "║    make              - Build both modes                    ║"
	@echo "║    make run-text     - Run Text mode                       ║"
	@echo "║    make run-gui      - Run GUI mode                        ║"
	@echo "║    make clean        - Clean all build files               ║"
	@echo "║                                                            ║"
	@echo "║  🎯 GUI Features:                                          ║"
	@echo "║    • Windows 7 style Start Menu                            ║"
	@echo "║    • Taskbar with clock and system tray                    ║"
	@echo "║    • Terminal with network commands                        ║"
	@echo "║    • Desktop icons and window management                   ║"
	@echo "║    • Smooth animations and gradients                       ║"
	@echo "║                                                            ║"
	@echo "║  🌐 Network System:                                        ║"
	@echo "║    • Network card detection (RTL8139, E1000)               ║"
	@echo "║    • MAC address support                                   ║"
	@echo "║    • IP configuration (IPv4)                               ║"
	@echo "║    • ARP Protocol (Address Resolution)                     ║"
	@echo "║    • ICMP Protocol (Ping)                                  ║"
	@echo "║    • UDP Protocol                                          ║"
	@echo "║                                                            ║"
	@echo "║  📡 Network Commands:                                      ║"
	@echo "║    ifconfig          - Network configuration               ║"
	@echo "║    ping <ip>         - Ping a host                         ║"
	@echo "║    arp               - Show ARP cache                      ║"
	@echo "║    netstat           - Show network stats                  ║"
	@echo "║                                                            ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# DEBUG TARGETS
# ============================================================================

debug-text: AscentOS-Text.iso disk.img
	@echo "🐛 Starting debug mode (Text)..."
	qemu-system-x86_64 \
	  -cdrom AscentOS-Text.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M -cpu qemu64 -boot d -serial stdio \
	  -netdev user,id=net0 -device rtl8139,netdev=net0 \
	  -s -S

debug-gui: AscentOS-GUI.iso
	@echo "🐛 Starting debug mode (GUI)..."
	qemu-system-x86_64 \
	  -cdrom AscentOS-GUI.iso \
	  -m 512M -cpu qemu64 -boot d -serial stdio -vga std \
	  -netdev user,id=net0 -device rtl8139,netdev=net0 \
	  -s -S

# ============================================================================
# CLEAN
# ============================================================================

clean:
	@echo "🧹 Cleaning build files..."
	rm -rf *.o *.elf
	rm -rf isodir_text isodir_gui
	rm -rf AscentOS-Text.iso AscentOS-GUI.iso
	rm -rf disk.img
	@echo "✓ Clean complete!"

# ============================================================================
# HELP
# ============================================================================

help:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║          AscentOS Makefile Help - Start Menu Edition      ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  Available targets:                                        ║"
	@echo "║                                                            ║"
	@echo "║  make              Build everything                        ║"
	@echo "║  make run-text     Build & run in text mode                ║"
	@echo "║  make run-gui      Build & run in GUI mode                 ║"
	@echo "║  make run          Same as run-text (default)              ║"
	@echo "║  make clean        Remove all build files                  ║"
	@echo "║  make info         Show detailed information               ║"
	@echo "║  make help         Show this help message                  ║"
	@echo "║                                                            ║"
	@echo "║  Debug targets:                                            ║"
	@echo "║  make debug-text   Start text mode with GDB support        ║"
	@echo "║  make debug-gui    Start GUI mode with GDB support         ║"
	@echo "║                                                            ║"
	@echo "║  🎯 GUI: Start Menu + Taskbar + Terminal                   ║"
	@echo "║  🌐 Network: RTL8139 + ARP + ICMP + UDP                    ║"
	@echo "║                                                            ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

.PHONY: all run run-text run-gui debug-text debug-gui clean help info