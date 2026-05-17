#!/usr/bin/env bash
# scripts/build-gtktest.sh - Build a simple GTK3 application for AscentOS

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${ROOT_DIR}/build/alpine/rootfs"
CC="${ROOT_DIR}/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"

if [ ! -d "$SYSROOT" ]; then
    echo "Error: Alpine rootfs not found. Run scripts/setup-alpine.sh first."
    exit 1
fi

# We need to compile against the libraries in the Alpine rootfs.
# Since we are not using a formal "sysroot" (according to user request?), 
# we will just pass include and library paths manually.

INCLUDES=(
    "-I${SYSROOT}/usr/include/gtk-2.0"
    "-I${SYSROOT}/usr/lib/gtk-2.0/include"
    "-I${SYSROOT}/usr/include/glib-2.0"
    "-I${SYSROOT}/usr/lib/glib-2.0/include"
    "-I${SYSROOT}/usr/include/pango-1.0"
    "-I${SYSROOT}/usr/include/harfbuzz"
    "-I${SYSROOT}/usr/include/cairo"
    "-I${SYSROOT}/usr/include/gdk-pixbuf-2.0"
    "-I${SYSROOT}/usr/include/atk-1.0"
    "-I${SYSROOT}/usr/include/pixman-1"
    "-I${SYSROOT}/usr/include/freetype2"
    "-I${SYSROOT}/usr/include/libpng16"
)

LIBS=(
    "-L${SYSROOT}/usr/lib"
    "-L${SYSROOT}/lib"
    "-lgtk-x11-2.0" "-lgdk-x11-2.0" "-lpangocairo-1.0" "-lpango-1.0" "-latk-1.0"
    "-lcairo" "-lgdk_pixbuf-2.0" "-lgio-2.0" "-lgobject-2.0" "-lglib-2.0"
    "-ljpeg" "-lmount" "-lblkid" "-leconf" "-lintl" "-lXrandr" "-lXinerama"
    "-lgraphite2" "-lXcomposite" "-lXdamage"
)

echo "[*] Compiling userland/gtk_test.c ..."
if $CC -O2 \
    "${ROOT_DIR}/userland/gtk_test.c" \
    -o "${ROOT_DIR}/userland/gtk_test.elf" \
    "${INCLUDES[@]}" \
    "${LIBS[@]}" \
    -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
    -Wl,-rpath,/usr/lib \
    -Wl,-rpath-link,${SYSROOT}/usr/lib; then
    echo "[SUCCESS] Built userland/gtk_test.elf"
else
    echo "[FAILURE] Compilation failed"
    exit 1
fi
