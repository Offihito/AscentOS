#!/bin/sh
# AscentOS: Build st (simple terminal) and its dependencies (fontconfig, libXft)
#
# This script builds st for AscentOS, replacing xterm.
# Required: fontconfig, libXft, libX11 (already available from X11 port)

set -e

# ── Environment Setup ────────────────────────────────────────────────────────
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_BASE="$ROOT_DIR/build/st"
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
    
    if [ -d "${name}-${version}" ] && [ ! -f "${name}-${version}/configure.ac" ] && [ ! -f "${name}-${version}/configure" ] && [ ! -f "${name}-${version}/Makefile" ]; then
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

# ── Helper: Prepare Meson Cross File ─────────────────────────────────────────
prepare_meson_cross() {
    CROSS_FILE="$BUILD_BASE/x86_64-ascentos.meson"
    echo ">>> Generating Meson cross file: $CROSS_FILE"
    sed -e "s|@CC@|$(command -v $CC)|g" \
        -e "s|@CXX@|$(command -v $CXX)|g" \
        -e "s|@AR@|$(command -v $AR)|g" \
        -e "s|@STRIP@|$(command -v $STRIP)|g" \
        -e "s|@SYSROOT@|$SYSROOT|g" \
        "$ROOT_DIR/scripts/x86_64-ascentos.meson.template" > "$CROSS_FILE"
}

# ── 1. Build expat (XML parser, needed by fontconfig) ─────────────────────────
build_expat() {
    # Use older expat 2.2.10 which doesn't require C++11
    fetch_pkg "expat" "2.2.10" "https://github.com/libexpat/libexpat/releases/download/R_2_2_10/expat-2.2.10.tar.gz"
    echo ">>> Building expat ..."
    # Set CXX=no to tell autoconf there's no C++ compiler
    # Set am_cv_prog_cc_c_o=yes to skip redundant checks
    ./configure --host="$HOST_TRIPLET" \
        --prefix="$SYSROOT" \
        --enable-static \
        --disable-shared \
        --without-docbook \
        CFLAGS="-static -O2" \
        CXX="no" \
        am_cv_prog_cc_c_o=yes
    make -j"$JOBS"
    make install
}

# ── 2. Build fontconfig ──────────────────────────────────────────────────────
build_fontconfig() {
    fetch_pkg "fontconfig" "2.15.0" "https://freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.gz"
    echo ">>> Building fontconfig ..."
    
    # Set cache directory to something reasonable for AscentOS
    ./configure --host="$HOST_TRIPLET" \
        --prefix="$SYSROOT" \
        --enable-static \
        --disable-shared \
        --disable-docs \
        --with-default-fonts=/share/fonts \
        --with-cache-dir=/tmp/fontconfig \
        CFLAGS="-static -O2 -I$SYSROOT/include" \
        LDFLAGS="-static -L$SYSROOT/lib"
    
    make -j"$JOBS"
    make install
}

# ── 3. Build libXrender (needed by libXft) ───────────────────────────────────
build_libXrender() {
    fetch_pkg "libXrender" "0.9.11" "https://gitlab.freedesktop.org/xorg/lib/libxrender/-/archive/libXrender-0.9.11/libxrender-libXrender-0.9.11.tar.gz"
    echo ">>> Building libXrender ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make -j"$JOBS"
    make install
}

# ── 4. Build libXft ──────────────────────────────────────────────────────────
build_libXft() {
    fetch_pkg "libXft" "2.3.8" "https://gitlab.freedesktop.org/xorg/lib/libxft/-/archive/libXft-2.3.8/libxft-libXft-2.3.8.tar.gz"
    echo ">>> Building libXft ..."
    ./configure --host="$HOST_TRIPLET" \
        --prefix="$SYSROOT" \
        --enable-static \
        --disable-shared \
        CFLAGS="-static -O2 -I$SYSROOT/include -I$SYSROOT/include/freetype2" \
        LDFLAGS="-static -L$SYSROOT/lib"
    make -j"$JOBS"
    make install
}

# ── 5. Build st (simple terminal) ────────────────────────────────────────────
build_st() {
    # Clone from git since st doesn't have regular releases
    mkdir -p "$BUILD_BASE"
    cd "$BUILD_BASE"
    
    if [ ! -d "st" ]; then
        echo ">>> Cloning st from suckless.org ..."
        git clone https://git.suckless.org/st
    fi
    
    cd st
    
    echo ">>> Building st ..."
    
    # Create config.mk override for cross-compilation
    # Note: st uses STCFLAGS for compilation, not CFLAGS
    cat > config.mk << EOF
# AscentOS cross-compilation config for st
VERSION = 0.9.2
PREFIX = /usr
MANPREFIX = \$(PREFIX)/share/man

CC = x86_64-linux-musl-gcc
AR = x86_64-linux-musl-ar
RANLIB = x86_64-linux-musl-ranlib

# st uses STCFLAGS for compilation
STCFLAGS = -I$SYSROOT/include -I$SYSROOT/include/freetype2 -I$SYSROOT/include/fontconfig -I$SYSROOT/include/X11 -DVERSION=\"0.9.2\" -D_XOPEN_SOURCE=600
STLDFLAGS = -static -L$SYSROOT/lib -lXft -lXrender -lX11 -lxcb -lXau -lXdmcp -lfontconfig -lfreetype -lexpat -lpixman-1

# X11 and font libraries
X11INC = $SYSROOT/include
X11LIB = $SYSROOT/lib

FREETYPEINC = $SYSROOT/include/freetype2
FREETYPELIB = $SYSROOT/lib

FONTCONFIGINC = $SYSROOT/include/fontconfig
FONTCONFIGLIB = $SYSROOT/lib
EOF

    # Clean and build
    make clean 2>/dev/null || true
    make -j"$JOBS"
    
    # Copy to userland as st.elf (replacing xterm.elf conceptually)
    cp -f st "$ROOT_DIR/userland/st.elf"
    echo ">>> st installed to $ROOT_DIR/userland/st.elf"
    
    # Also remove the old xterm.elf since we're replacing it
    if [ -f "$ROOT_DIR/userland/xterm.elf" ]; then
        echo ">>> Removing old xterm.elf (replaced by st)"
        rm -f "$ROOT_DIR/userland/xterm.elf"
    fi
}

# ── Main ─────────────────────────────────────────────────────────────────────

echo "========================================"
echo "      AscentOS st Builder               "
echo "      (Replacing xterm with st)         "
echo "========================================"

mkdir -p "$BUILD_BASE"
prepare_meson_cross

# Build dependencies in order
build_expat
build_fontconfig
build_libXrender
build_libXft

# Build st
build_st

echo "========================================"
echo "st built successfully!"
echo "Find it at: $ROOT_DIR/userland/st.elf"
echo "xterm has been replaced with st"
echo "========================================"
