# 🚀 AscentOS

> A modern 64-bit hobby operating system — with its own filesystem and basic GUI support

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
![C](https://img.shields.io/badge/language-C-00599C.svg)
![x86_64](https://img.shields.io/badge/arch-x86__64-important)

## ✨ Current Features (as of March 2026)

- **64-bit x86_64 architecture** — designed for modern hardware
- **FAT32 filesystem** — read/write support fully working
- **Simple text-based shell**
- **Basic graphics mode** (switch by typing `gfx` in the shell)
- **Modular memory management** (stabilized in recent updates)
- **Musl libc** integration
- **First userland application:** [kilo](https://github.com/antirez/kilo) text editor successfully ported
- **GRUB2 multiboot booting**
- **Full Network Stack**

The OS currently **successfully boots on its own**, can read/write files on FAT32, and switches to a basic graphical mode.

## 🛠️ Building & Running from Source

### Prerequisites

- `make`, `gcc` / `clang`, `nasm`, `grub-mkrescue`, `qemu-system-x86_64`
- `xorriso` (for creating the bootable ISO)
- Musl cross-toolchain (builds automatically, or install manually if preferred)

### Quick Start

```bash
# Clone the repository
git clone https://github.com/Offihito/AscentOS.git
cd AscentOS

# Build and run in QEMU (text mode)
make run
