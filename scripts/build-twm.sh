#!/bin/sh
# AscentOS: Build twm (Tab Window Manager)
#
# This script builds twm for AscentOS.
# TWM is a simple, lightweight window manager for X11.

set -e

# ── Environment Setup ────────────────────────────────────────────────────────
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_BASE="$ROOT_DIR/build/x11"
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
    
    if [ -d "${name}-${version}" ] && [ ! -f "${name}-${version}/configure.ac" ] && [ ! -f "${name}-${version}/meson.build" ]; then
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

# ── 1. Build libXrandr (X Resize and Rotate Extension) ───────────────────────
build_libXrandr() {
    fetch_pkg "libXrandr" "1.5.4" "https://gitlab.freedesktop.org/xorg/lib/libxrandr/-/archive/libXrandr-1.5.4/libxrandr-libXrandr-1.5.4.tar.gz"
    echo ">>> Building libXrandr ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 2. Build twm ─────────────────────────────────────────────────────────────
build_twm() {
    fetch_pkg "twm" "1.0.12" "https://gitlab.freedesktop.org/xorg/app/twm/-/archive/twm-1.0.12/twm-twm-1.0.12.tar.gz"
    echo ">>> Building twm ..."
    
    # twm needs libX11, libXt, libXmu, libXrandr, libXrender
    # Most of these are already built by build-xeyes.sh
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" \
        CFLAGS="-static -O2 -I$SYSROOT/include" \
        LDFLAGS="-static -L$SYSROOT/lib" \
        LIBS="-lXmu -lXt -lXext -lX11 -lXrandr -lXrender -lXfixes -lXi -lSM -lICE -lxcb -lXau -lXdmcp -lmd"
    make -j"$JOBS"
    
    # Copy to userland (twm binary is in src/ subdirectory)
    cp -f src/twm "$ROOT_DIR/userland/twm.elf"
    echo ">>> twm installed to $ROOT_DIR/userland/twm.elf"
}

# ── Main ─────────────────────────────────────────────────────────────────────

echo "========================================"
echo "    AscentOS twm Builder               "
echo "========================================"

mkdir -p "$BUILD_BASE"
prepare_meson_cross

# Build libXrandr (needed by twm)
build_libXrandr

# Build twm
build_twm

echo "========================================"
echo "twm built successfully!"
echo "Find it at: $ROOT_DIR/userland/twm.elf"
echo "========================================"
