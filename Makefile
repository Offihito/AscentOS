# AscentOS 64-bit Makefile - Unified Boot & Unified Keyboard Version
# Updated with SYSCALL support (Phase 1)

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
	@echo "║  ✓ AscentOS 64-bit (Unified Boot + Keyboard)     ║"
	@echo "║  ✓ SYSCALL Support Enabled (Phase 1)             ║"
	@echo "║                                                   ║"
	@echo "║  Text Mode:   make run-text                      ║"
	@echo "║  GUI Mode:    make run-gui                       ║"
	@echo "║                                                   ║"
	@echo "║  🎯 Single keyboard driver for both modes        ║"
	@echo "║  🔧 Single unified bootloader for both modes     ║"
	@echo "║  🚀 Modern SYSCALL/SYSRET interface              ║"
	@echo "╚═══════════════════════════════════════════════════╝"

# ============================================================================
# SHARED COMPONENTS
# ============================================================================

# Shared files compiled once

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

# ============================================================================
# SYSCALL SUPPORT (PHASE 1) - Shared for both modes
# ============================================================================

syscall64.o: kernel/syscall64.asm
	$(AS) $(ASFLAGS) kernel/syscall64.asm -o syscall64.o

syscall.o: kernel/syscall.c kernel/syscall.h kernel/task.h kernel/scheduler.h
	$(CC) $(CFLAGS) -c kernel/syscall.c -o syscall.o

syscall_setup.o: kernel/syscall_setup.c
	$(CC) $(CFLAGS) -c kernel/syscall_setup.c -o syscall_setup.o

syscall_test.o: kernel/syscall_test.c kernel/syscall.h
	$(CC) $(CFLAGS) -c kernel/syscall_test.c -o syscall_test.o

usermode_transition.o: kernel/usermode_transition.asm
	$(AS) $(ASFLAGS) kernel/usermode_transition.asm -o usermode_transition.o
	
	
# ============================================================================
# TEXT MODE BUILD
# ============================================================================

boot64_text.o: boot/boot64_unified.asm
	$(AS) $(ASFLAGS) boot/boot64_unified.asm -o boot64_text.o

interrupts64_text.o: arch/x86_64/interrupts64.asm
	$(AS) $(ASFLAGS) -DTEXT_MODE_BUILD arch/x86_64/interrupts64.asm -o interrupts64_text.o

keyboard_text.o: kernel/keyboard_unified.c
	$(CC) $(CFLAGS) -c kernel/keyboard_unified.c -o keyboard_text.o

commands64_text.o: apps/commands64.c apps/commands64.h
	$(CC) $(CFLAGS) -c apps/commands64.c -o commands64_text.o

kernel64_text.o: kernel/kernel64.c
	$(CC) $(CFLAGS) -DTEXT_MODE -c kernel/kernel64.c -o kernel64_text.o

TEXT_OBJS = boot64_text.o interrupts64_text.o vga64.o keyboard_text.o \
            commands64_text.o files64.o disk64.o elf64.o memory_unified.o vmm64.o nano64.o \
            timer.o task.o scheduler.o kernel64_text.o page_fault.o \
            syscall64.o syscall.o syscall_setup.o syscall_test.o usermode_transition.o

kernel64_text.elf: $(TEXT_OBJS)
	$(LD) $(LDFLAGS) $(TEXT_OBJS) -o kernel64_text.elf

disk.img:
	@echo "📀 Creating 2GB disk image..."
	@if [ ! -f disk.img ]; then \
		qemu-img create -f raw disk.img 2G; \
		echo "✓ 2GB Disk image created!"; \
	else \
		echo "✓ Disk image already exists (preserving data)"; \
	fi

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
           syscall64.o syscall.o syscall_setup.o syscall_test.o \
		   usermode_transition.o
		   
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

run-text: AscentOS-Text.iso disk.img
	@echo "╔════════════════════════════════════════════╗"
	@echo "║   AscentOS Text Mode (Unified)            ║"
	@echo "║   + SYSCALL Support (Phase 1)             ║"
	@echo "╚════════════════════════════════════════════╝"
	@echo ""
	@echo "Test commands:"
	@echo "  • testsyscall  - Test syscall infrastructure"
	@echo "  • syscallstats - Show syscall statistics"
	@echo ""
	qemu-system-x86_64 \
	  -cdrom AscentOS-Text.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 1024M \
	  -cpu qemu64 \
	  -boot d \
	  -serial stdio \
	  -display gtk 

run-gui: AscentOS-GUI.iso disk.img
	@echo "╔════════════════════════════════════════════╗"
	@echo "║   AscentOS GUI Mode (Unified)             ║"
	@echo "║   + SYSCALL Support (Phase 1)             ║"
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
# INFO TARGET
# ============================================================================

info:
	@echo "╔════════════════════════════════════════════════════════════╗"
	@echo "║   AscentOS Build Information - Unified + SYSCALL Edition  ║"
	@echo "╠════════════════════════════════════════════════════════════╣"
	@echo "║                                                            ║"
	@echo "║  🚀 SYSCALL Support (Phase 1):                             ║"
	@echo "║    • Modern SYSCALL/SYSRET instructions                    ║"
	@echo "║    • MSR-based configuration                               ║"
	@echo "║    • Linux-compatible syscall numbers                      ║"
	@echo "║    • Statistics and debugging                              ║"
	@echo "║                                                            ║"
	@echo "║  🎹 Unified Keyboard Driver:                               ║"
	@echo "║    • Single source file for both modes                     ║"
	@echo "║    • Conditional compilation (GUI_MODE flag)               ║"
	@echo "║    • Full nano editor support in text mode                 ║"
	@echo "║    • Terminal support in GUI mode                          ║"
	@echo "║                                                            ║"
	@echo "║  🔧 Bootloader:                                            ║"
	@echo "║    • Single unified bootloader for both modes              ║"
	@echo "║    • 4GB memory mapping (identity mapped)                  ║"
	@echo "║    • VESA framebuffer support                              ║"
	@echo "║    • Serial debug output                                   ║"
	@echo "║                                                            ║"
	@echo "║  📦 Build Targets:                                         ║"
	@echo "║    make              - Build both modes                    ║"
	@echo "║    make run-text     - Run Text mode                       ║"
	@echo "║    make run-gui      - Run GUI mode                        ║"
	@echo "║    make clean        - Clean all build files               ║"
	@echo "║                                                            ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

# ============================================================================
# DEBUG TARGETS
# ============================================================================

debug-text: AscentOS-Text.iso disk.img
	@echo "🐛 Starting debug mode (Text - Unified + SYSCALL)..."
	qemu-system-x86_64 \
	  -cdrom AscentOS-Text.iso \
	  -drive file=disk.img,format=raw,if=ide,cache=writeback \
	  -m 512M -cpu qemu64 -boot d -serial stdio \
	  -s -S

debug-gui: AscentOS-GUI.iso
	@echo "🐛 Starting debug mode (GUI - Unified + SYSCALL)..."
	qemu-system-x86_64 \
	  -cdrom AscentOS-GUI.iso \
	  -m 512M -cpu qemu64 -boot d -serial stdio -vga std \
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
	@echo "║     AscentOS Makefile Help - Unified + SYSCALL Edition    ║"
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
	@echo "║  🎹 Unified Keyboard: Single driver for both modes         ║"
	@echo "║  🔧 Unified Boot: Single bootloader for both modes         ║"
	@echo "║  🚀 SYSCALL: Modern syscall interface (Phase 1)            ║"
	@echo "║  🎯 GUI: Start Menu + Taskbar + Terminal                   ║"
	@echo "║                                                            ║"
	@echo "║  New test commands in shell:                               ║"
	@echo "║    testsyscall   - Test syscall infrastructure             ║"
	@echo "║    syscallstats  - Show syscall statistics                 ║"
	@echo "║                                                            ║"
	@echo "╚════════════════════════════════════════════════════════════╝"

.PHONY: all run run-text run-gui debug-text debug-gui clean help info