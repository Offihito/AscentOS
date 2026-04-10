# AscentOS

**AscentOS** is a custom 64-bit (x86_64) hobby operating system kernel written in C and Assembly, focused on Testing AI's Coding Power It uses the [Limine](https://limine-bootloader.org/) boot protocol for an easy and compliant boot process.

## Features

- **Core Architecture & Multiprocessing**
   Symmetric Multiprocessing (SMP): Support for multiple CPU cores, initialized via ACPI MADT parsing.
   Interrupt Handling: Advanced Programmable Interrupt Controller (APIC) and IOAPIC support with local APIC timers.
   Inter-Processor Interrupts (IPIs): Communication between cores, including thread blocking and wake-up mechanisms.
   Memory Management: Physical Memory Manager (PMM), Virtual Memory Manager (VMM), and a dynamic kernel heap allocator.
   Concurrency: Thread-safe locking mechanisms (spinlocks, etc.) and per-CPU scheduling strategies.

- **Filesystem & Storage**
   AHCI Driver: Custom driver to communicate with SATA devices.
   Virtual File System (VFS): Abstraction layer for filesystem operations.
   Ext2 Filesystem: Fully functional driver with features including reading, writing, inode management, `rename`, `chmod`, `chown`, file creation (`touch`), and directory creation (`mkdir`).
  
- **User Interface & Interaction**
   Kernel Shell: Built-in interactive command-line interface for executing commands right within the kernel.
   Graphics: Framebuffer setup via Limine with a basic windowing/console drawing system and custom fonts, complete with a customized boot background (`boo.png`).
   Debugging: COM1 serial port support for early boot and kernel-level logging.

## Prerequisites

To build and run AscentOS, you will need the following dependencies installed on your Linux system:

- `make`
- A C compiler (e.g., `gcc` or `clang` targeting x86_64-elf or your native compiler if compatible)
- `qemu-system-x86_64` (for emulation)
- `xorriso` (for creating the bootable ISO)
- `curl`, `tar`, `gunzip` (for downloading dependencies like Limine and OVMF)
- `e2fsprogs` (specifically `mkfs.ext2` and `debugfs` for generating disk images)
- `rustc` and `cargo` (for building the kria-lang port)

## Building and Running

The project includes a robust `Makefile` for automated builds, disk image creation, and QEMU execution.

1. **Build the bootable ISO:**
   ```bash
   make
   ```
   *This clones Limine, builds the kernel, and creates `ascentos-x86_64.iso`.*

2. **Run in QEMU (UEFI Mode):**
   ```bash
   make run
   ```
   *This downloads the EDK2 OVMF firmware, creates a sample 64MB Ext2 disk image (`disk.img`), and launches QEMU with 4 cores in UEFI mode.*

3. **Run in QEMU (Legacy BIOS Mode):**
   ```bash
   make run-bios
   ```

## Ported Software

- **Kria Programming Language** - A custom programming language with bytecode VM execution, ported from [kria-lang](https://github.com/Piotriox/kria-lang). Built as a static binary using Rust and musl libc for Linux x86_64 binary compatibility.
  - Build: `make kria`
  - Included on disk image: `kria.elf` (interpreter) and `test.krx` (sample program)
  - Usage on AscentOS: `./kria.elf program.krx`

## Directory Structure

- `kernel/` - The core operating system source code.
  - `src/` - C source files and subsystems (acpi, apic, fs, mm, sched, shell, smp, etc.).
  - `linker-scripts/` - Custom linker script for the x86_64 kernel.
- `userland/` - User-space programs compiled with musl libc.
  - `kria-lang/` - Kria programming language source (Rust).
- `limine.conf` - Configuration file for the Limine bootloader.
- `GNUmakefile` - The main build script orchestrating the Limine download, kernel build, disk image generation, and QEMU execution.

## License

This project is open-source and intended for educational purposes and operating system development exploration.
