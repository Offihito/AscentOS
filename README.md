# AscentOS

AscentOS is a hobby operating system kernel for the x86_64 architecture, written in C and Assembly. It has grown from a minimal bootable kernel into a system with an X11 graphical environment, a basic networking stack, and audio support.

## Current Status

The system is currently capable of:
*   **Graphics:** Running an X11 server (Xfbdev) with the `twm` window manager, `xterm`, and `xeyes`.
*   **Networking:** Full TCP/UDP support. Tools like `wget` work with SSL (via wolfSSL).
*   **Audio:** Sound Blaster 16 and AC97 drivers are functional; capable of playing WAV files.
*   **Software Ported:** A variety of Unix tools including Bash, coreutils, TCC (Tiny C Compiler), Lua, and Kria-lang.
*   **Kernel Features:** SMP (multiprocessing), advanced memory management (PMM/VMM/Heap), and CoW fork support.

## Current Goals

*   **Porting GCC:** Enabling a native GCC toolchain to support full self-hosting.
*   **Full USB Support:** Expanding beyond UHCI to support OHCI, EHCI, and xHCI controllers, along with a wider range of USB devices.

## Prerequisites

To build the project on Linux, you'll need:

*   `make`, `gcc`/`clang`, `nasm`
*   `git`, `curl`
*   `xorriso` (for ISO creation)
*   `qemu-system-x86_64` (for emulation)
*   `e2fsprogs` (`mkfs.ext3` and `debugfs` are required for disk image generation)
*   `python3`
*   `rustc` & `cargo` (only if you want to build `kria-lang`)

## Building and Running

1.  **Build everything:**
    ```bash
    make
    ```
    This prepares the musl-based cross-toolchain, builds the kernel, and generates the bootable ISO.

2.  **Run in QEMU:**
    ```bash
    make run
    ```

## Building Ported Software

To build the full suite of ported tools (Bash, X11, etc.), run these scripts in order:

```bash
./scripts/build-bash.sh      && \
./scripts/build-coreutils.sh && \
./scripts/build-lua.sh       && \
./scripts/build-tar.sh       && \
./scripts/build-tcc.sh       && \
./scripts/port-x11.sh        && \
./scripts/build-xeyes.sh     && \
./scripts/build-twm.sh       && \
./scripts/build-xterm.sh
```

## Directory Structure

*   `kernel/`: Core kernel source code.
*   `userland/`: User-space applications and ports.
*   `scripts/`: Build system and toolchain automation.
*   `initrd/`: Initial files and configurations for the system boot.
*   `toolchain/`: (Generated) Musl toolchain and sysroot.
*   `limine/`: Bootloader source and binaries.

---
Developed as an open-source project for educational purposes and OS development exploration.
