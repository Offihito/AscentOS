# AscentOS – 64-bit Hobby Operating System

**One-sentence summary**  
AscentOS is a minimal, from-scratch 64-bit x86_64 hobby OS with a higher-half kernel, musl libc-based userland, EXT3 filesystem, full TCP/UDP/IP network stack, and a handful of ported userland applications (Doom, TCC, Lua, Kilo,).

**Current status (March 2026)**  
- Higher-half kernel  
- Fast syscalls via sysret + MSR  
- Round-robin preemptive multitasking  
- SSE-enabled spinlocks  
- Full IDT / GDT / TSS / TLS support  
- PCI enumeration & basic driver support  
- **Graphics abstraction layer** (NEW): UEFI GOP + VESA framebuffer support  
- Drivers: ATA (disk), RTL8139 (network), Sound Blaster 16, PC speaker, **Graphics (GOP/VESA)**  
- Filesystem: EXT3 (read/write + journaling)  
- Network: full TCP/IP/UDP stack + minimal HTTP server  
- Userland: statically linked with musl libc  
- Ported applications: doomgeneric, tcc, lua, kilo (editor),  
- WAV audio decoder  
- Kernel panic screen (classic blue style)  

**Critical rules – NEVER do these (highest priority)**  
- **Never** use glibc, GNU extensions, _GNU_SOURCE, fortify_source, or any glibc-specific symbols  
- **Never** assume dynamic linking exists — everything must be -static for now  
- **Never** include kernel headers directly from userland code  
- **Never** use modern POSIX/syscalls that are not yet implemented (e.g. no memfd_create, eventfd, timerfd, etc.)  
- **Never** leave debug printks or console.log-style prints in final code — use proper logging when it exists  
- **Never** write inline assembly that breaks the sysret/MSR fast syscall convention  

**Project folder structure & ownership**

arch/x86_64/       → Architecture-specific code: boot, entry points, IDT/GDT/TSS, syscall stubs  
boot/              → Multiboot2 header, early boot, higher-half transition  
commands/          → ring 0 shell builtins & small utilities (ls, cat, echo, …)  
drivers/           → Device drivers (PCI, ATA, RTL8139, SB16, PC speaker, …)  
fs/                → Filesystem implementations (currently only EXT3)  
kernel/            → Core kernel: scheduler, memory management, syscall dispatcher, panic, locking  
network/           → Network stack: TCP/UDP/IP, socket layer, simple HTTP  
userland/          → Ported user applications + musl libc build integration  

**Userland porting rules (very important – follow exactly)**

1. Create shell script → `<app-name>-build.sh`  
2. Write a build script → `<app-name>-build.sh`  
   - Use musl-based cross toolchain (`x86_64-ascent-musl-gcc` or similar)  
   - Always pass `-static` (dynamic linking not ready)  
   - Add musl-friendly flags: `-fno-stack-protect -U_FORTIFY_SOURCE -D_POSIX_C_SOURCE=200809L` etc.  
   - Look at existing clean examples: `kilo-build.sh` (simplest), `lua-build.sh`, `tcc-build.sh`, `doom-build.sh`  
3. Output binary → copy to initrd / ramdisk location or embed in ISO  
4. If new syscalls are required:  
   - Add them first in kernel (syscall table + implementation)  
   - Update musl's `kernel/syscall.c and syscall.h` or equivalent  
   - Rebuild musl & userland  

**Build & run commands (reminder)**

make               → build
make run           → qemu-system-x86_64 
make clean         → clean build artifacts  
make clean-all     → clean build artifacts + musl

**Toolchain & musl facts**

- Musl is built via `musl-build.sh` → produces x86_64-ascent-musl-* prefix  
- All userland is compiled against this musl (very strict POSIX)  
- **Never** define _GNU_SOURCE or use glibc-only features  
- Minimal syscalls implemented → check `kernel/syscall.c` or `userland/libc/syscalls.c` before assuming anything exists  

**Code style & conventions**

- Language: C11 (no C++ yet)  
- Warnings: -Wall -Wextra -Werror -pedantic  
- Naming:  
  - Types & structs  → UpperCamelCase     (e.g. TaskStruct)  
  - Functions        → snake_case         (e.g. sched_yield)  
  - Variables        → snake_case  
  - Constants        → UPPER_CASE  
- Kernel includes: `#include <kernel/ext3.h>`, `#include <kernel/scheduler.h>`, etc.  
- Prefer forward declarations over unnecessary includes  

**Current development priorities**

1. Test all remaining syscalls
2. Port GNU Core Utils 
3. Implementing SMP

**When in doubt**  
- Check existing port scripts in `userland/` — they are the source of truth for musl-friendly builds  
- Ask before adding new syscalls: many ports fail because of missing-but-assumed syscalls  