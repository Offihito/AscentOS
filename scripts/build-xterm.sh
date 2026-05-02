#!/bin/sh
# AscentOS: Build xterm and its dependencies (libXaw, libXpm, ncurses)
#
# This script builds xterm for AscentOS.
# Required: libXaw, libXpm, ncurses (for terminfo/libtinfo)

set -e

# ── Environment Setup ────────────────────────────────────────────────────────
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_BASE="$ROOT_DIR/build/xterm"
SYSROOT=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
TOOLCHAIN_BIN="$ROOT_DIR/toolchain/x86_64-linux-musl/bin"
JOBS=$(nproc 2>/dev/null || echo 4)

export PATH="$TOOLCHAIN_BIN:$PATH"
export CC="x86_64-linux-musl-gcc"
export CXX="x86_64-linux-musl-g++"
export AR=$(command -v x86_64-linux-musl-ar || echo "ar")
export STRIP=$(command -v x86_64-linux-musl-strip || echo "strip")
export RANLIB=$(command -v x86_64-linux-musl-ranlib || echo "ranlib")
export PKG_CONFIG="pkg-config"
export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export HOST_TRIPLET="x86_64-linux-musl"
export ACLOCAL_PATH="$SYSROOT/share/aclocal"

mkdir -p "$ACLOCAL_PATH"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "Error: $CC not found. Run scripts/musl-toolchain.sh first."
    exit 1
fi

# ── Helper: Download and Extract ─────────────────────────────────────────────
fetch_pkg() {
    name=$1
    version=$2
    url=$3
    tarball="${name}-${version}.tar.gz"
    
    mkdir -p "$BUILD_BASE"
    cd "$BUILD_BASE"
    
    if [ ! -f "$tarball" ] || ! tar tzf "$tarball" >/dev/null 2>&1; then
        echo ">>> Downloading $name $version ..."
        rm -f "$tarball"
        curl -L --connect-timeout 60 --max-time 600 --retry 5 --retry-delay 5 -o "$tarball" "$url"
    fi
    
    if [ -d "${name}-${version}" ] && [ ! -f "${name}-${version}/configure.ac" ] && [ ! -f "${name}-${version}/configure" ]; then
        echo ">>> Removing broken directory ${name}-${version} ..."
        rm -rf "${name}-${version}"
    fi

    if [ ! -d "${name}-${version}" ]; then
        echo ">>> Extracting $tarball ..."
        mkdir -p "${name}-${version}"
        tar xf "$tarball" -C "${name}-${version}" --strip-components=1
    fi
    
    cd "${name}-${version}"
    
    if [ ! -f "configure" ] && [ -f "configure.ac" ]; then
        echo ">>> Generating configure script for $name ..."
        autoreconf -vfi
    fi
}

# ── 1. Build libXpm (X Pixmap) ──────────────────────────────────────────────
build_libXpm() {
    fetch_pkg "libXpm" "3.5.17" "https://gitlab.freedesktop.org/xorg/lib/libxpm/-/archive/libXpm-3.5.17/libxpm-libXpm-3.5.17.tar.gz"
    echo ">>> Building libXpm ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make -j"$JOBS"
    make install
}

# ── 2. Build libXaw (Athena Widget Set) ──────────────────────────────────────
build_libXaw() {
    fetch_pkg "libXaw" "1.0.16" "https://gitlab.freedesktop.org/xorg/lib/libxaw/-/archive/libXaw-1.0.16/libxaw-libXaw-1.0.16.tar.gz"
    echo ">>> Building libXaw ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make -j"$JOBS"
    make install
}

# ── 3. Build ncurses (for terminfo) ──────────────────────────────────────────
build_ncurses() {
    fetch_pkg "ncurses" "6.4" "https://ftp.gnu.org/pub/gnu/ncurses/ncurses-6.4.tar.gz"
    echo ">>> Building ncurses ..."
    ./configure --host="$HOST_TRIPLET" \
        --prefix="$SYSROOT" \
        --enable-static \
        --disable-shared \
        --without-cxx \
        --without-ada \
        --without-progs \
        --without-tests \
        --disable-db-install \
        --without-cxx-binding \
        --with-terminfo-dirs="/share/terminfo" \
        --with-default-terminfo-dir="/share/terminfo"
    make -j"$JOBS"
    make install
}

# ── 4. Build xterm ────────────────────────────────────────────────────────────
build_xterm() {
    fetch_pkg "xterm" "389" "https://invisible-mirror.net/archives/xterm/xterm-389.tgz"
    echo ">>> Building xterm ..."
    
    # Configure xterm with settings appropriate for AscentOS
    ./configure --host="$HOST_TRIPLET" \
        --prefix="$SYSROOT" \
        --disable-setuid \
        --disable-setgid \
        --with-x \
        --x-includes="$SYSROOT/include" \
        --x-libraries="$SYSROOT/lib" \
        CFLAGS="-static -O2 -I$SYSROOT/include -I$SYSROOT/include/ncurses" \
        LDFLAGS="-static -L$SYSROOT/lib" \
        LIBS="-lXaw7 -lXmu -lXt -lSM -lICE -lXpm -lXext -lX11 -lxcb -lXau -lXdmcp -lmd -lncurses"

    make -j"$JOBS"
    
    # Copy to userland
    cp -f xterm "$ROOT_DIR/userland/xterm.elf"
    echo ">>> xterm installed to $ROOT_DIR/userland/xterm.elf"
}

# ── Main ─────────────────────────────────────────────────────────────────────

echo "========================================"
echo "      AscentOS xterm Builder            "
echo "========================================"

mkdir -p "$BUILD_BASE"

# Build dependencies
build_libXpm
build_libXaw
build_ncurses

# Build xterm
build_xterm

echo "========================================"
echo "xterm built successfully!"
echo "Find it at: $ROOT_DIR/userland/xterm.elf"
echo "========================================"
