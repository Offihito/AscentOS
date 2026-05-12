#!/bin/sh
# AscentOS: Build JWM (Joe's Window Manager)
#
# JWM is a lightweight window manager for X11.
# This script builds JWM and its dependencies for AscentOS.

set -e

# ── Environment Setup ────────────────────────────────────────────────────────
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_BASE="$ROOT_DIR/build/jwm-build"
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
mkdir -p "$BUILD_BASE"

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
    
    cd "$BUILD_BASE"
    
    if [ ! -f "$tarball" ] || ! tar tzf "$tarball" >/dev/null 2>&1; then
        echo ">>> Downloading $name $version ..."
        rm -f "$tarball"
        curl -L --connect-timeout 60 --max-time 600 --retry 5 --retry-delay 5 -o "$tarball" "$url"
    fi
    
    if [ -d "${name}-${version}" ] && [ ! -f "${name}-${version}/configure.ac" ] && [ ! -f "${name}-${version}/configure" ] && [ ! -f "${name}-${version}/meson.build" ] && [ ! -f "${name}-${version}/Makefile" ]; then
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

# ── 0. Build libpng ──────────────────────────────────────────────────────────
build_libpng() {
    if [ -f "$SYSROOT/lib/pkgconfig/libpng16.pc" ]; then
        echo ">>> libpng already built."
        return
    fi
    fetch_pkg "libpng" "1.6.43" "https://downloads.sourceforge.net/project/libpng/libpng16/1.6.43/libpng-1.6.43.tar.gz"
    echo ">>> Building libpng ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared \
        CPPFLAGS="-I$SYSROOT/include" LDFLAGS="-L$SYSROOT/lib"
    make -j"$JOBS"
    make install
}

# ── 1. Build libXpm ──────────────────────────────────────────────────────────
build_libXpm() {
    if [ -f "$SYSROOT/lib/pkgconfig/xpm.pc" ]; then
        echo ">>> libXpm already built."
        return
    fi
    fetch_pkg "libXpm" "3.5.17" "https://gitlab.freedesktop.org/xorg/lib/libxpm/-/archive/libXpm-3.5.17/libxpm-libXpm-3.5.17.tar.gz"
    echo ">>> Building libXpm ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make -j"$JOBS"
    make install
}

# ── 2. Build libXinerama ─────────────────────────────────────────────────────
build_libXinerama() {
    if [ -f "$SYSROOT/lib/pkgconfig/xinerama.pc" ]; then
        echo ">>> libXinerama already built."
        return
    fi
    fetch_pkg "libXinerama" "1.1.5" "https://gitlab.freedesktop.org/xorg/lib/libxinerama/-/archive/libXinerama-1.1.5/libxinerama-libXinerama-1.1.5.tar.gz"
    echo ">>> Building libXinerama ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make -j"$JOBS"
    make install
}

# ── 3. Build expat ───────────────────────────────────────────────────────────
build_expat() {
    if [ -f "$SYSROOT/lib/pkgconfig/expat.pc" ]; then
        echo ">>> expat already built."
        return
    fi
    fetch_pkg "expat" "2.6.2" "https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-2.6.2.tar.gz"
    echo ">>> Building expat ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared --without-docbook
    make -j"$JOBS"
    make install
}

# ── 4. Build fontconfig ──────────────────────────────────────────────────────
build_fontconfig() {
    if [ -f "$SYSROOT/lib/pkgconfig/fontconfig.pc" ]; then
        echo ">>> fontconfig already built."
        return
    fi
    fetch_pkg "fontconfig" "2.15.0" "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.gz"
    echo ">>> Building fontconfig ..."
    # fontconfig needs uuid, we might need a dummy or disable it
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared \
        --disable-docs --with-expat-includes="$SYSROOT/include" --with-expat-lib="$SYSROOT/lib" \
        UUID_CFLAGS=" " UUID_LIBS=" "
    make -j"$JOBS"
    make install
}

# ── 5. Build libXft ──────────────────────────────────────────────────────────
build_libXft() {
    if [ -f "$SYSROOT/lib/pkgconfig/xft.pc" ]; then
        echo ">>> libXft already built."
        return
    fi
    fetch_pkg "libXft" "2.3.8" "https://gitlab.freedesktop.org/xorg/lib/libxft/-/archive/libXft-2.3.8/libxft-libXft-2.3.8.tar.gz"
    echo ">>> Building libXft ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make -j"$JOBS"
    make install
}

# ── 6. Build JWM ─────────────────────────────────────────────────────────────
build_jwm() {
    fetch_pkg "jwm" "2.4.3" "https://github.com/joewing/jwm/releases/download/v2.4.3/jwm-2.4.3.tar.xz"
    # Note: .tar.xz handler in fetch_pkg needs update or use .tar.gz if available
    # Actually, I'll just use the tar xf which handles xz automatically usually.
    # But JWM also has a .tar.gz
    # URL: https://github.com/joewing/jwm/archive/refs/tags/v2.4.3.tar.gz
    
    # Let's use the tar.gz from github tags
    fetch_pkg "jwm" "2.4.3" "https://github.com/joewing/jwm/archive/refs/tags/v2.4.3.tar.gz"
    
    # Define the full set of libraries needed for static linking in correct order
    # High-level libraries must come before their dependencies
    STATIC_LIBS="-lXmu -lXft -lfontconfig -lfreetype -lexpat -lXpm -lXrender -lXinerama -lXext -lX11 -lpng16 -lz -lxcb -lXau -lXdmcp -lmd"

    echo ">>> Building JWM ..."
    
    # JWM configuration. We force static.
    # During configure, keep LDFLAGS clean (-L paths only) so tests dont fail.
    # Put all libraries in LIBS for configure tests.
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" \
        --enable-debug \
        --enable-xrender \
        --enable-xpm \
        --enable-xft \
        --enable-xinerama \
        --enable-xrandr \
        --enable-png \
        --disable-nls \
        CFLAGS="-static -O2 -I$SYSROOT/include -I$SYSROOT/include/freetype2" \
        LDFLAGS="-static -L$SYSROOT/lib" \
        LIBS="$STATIC_LIBS" \
        CPPFLAGS="-I$SYSROOT/include" \
        PNG_CFLAGS="-I$SYSROOT/include" \
        PNG_LIBS="-L$SYSROOT/lib -lpng16"

    # Explicitly pass LDFLAGS to make because JWM's Makefile uses LDFLAGS for everything
    make -j"$JOBS" LDFLAGS="-static -L$SYSROOT/lib $STATIC_LIBS -g"
    
    # Copy to userland
    cp -f src/jwm "$ROOT_DIR/userland/jwm.elf"
    echo ">>> JWM installed to $ROOT_DIR/userland/jwm.elf"
}

# ── Main ─────────────────────────────────────────────────────────────────────

echo "========================================"
echo "    AscentOS JWM Builder               "
echo "========================================"

build_libpng
build_libXpm
build_libXinerama
build_expat
build_fontconfig
build_libXft
build_jwm

echo "========================================"
echo "JWM built successfully!"
echo "Find it at: $ROOT_DIR/userland/jwm.elf"
echo "========================================"
