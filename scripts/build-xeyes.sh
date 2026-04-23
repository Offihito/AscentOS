#!/bin/sh
# AscentOS: Build Xt stack and xeyes
#
# This script builds the X Toolkit libraries and xeyes for AscentOS.
# Required for xeyes: libXt, libXmu, libXi, libXrender, libXfixes, libSM, libICE

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

# ── 1. Build libICE (Inter-Client Exchange) ───────────────────────────────────
build_libICE() {
    fetch_pkg "libICE" "1.1.1" "https://gitlab.freedesktop.org/xorg/lib/libice/-/archive/libICE-1.1.1/libice-libICE-1.1.1.tar.gz"
    echo ">>> Building libICE ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared \
        --disable-xf86bigfont
    make install
}

# ── 2. Build libSM (Session Management) ───────────────────────────────────────
build_libSM() {
    fetch_pkg "libSM" "1.2.4" "https://gitlab.freedesktop.org/xorg/lib/libsm/-/archive/libSM-1.2.4/libsm-libSM-1.2.4.tar.gz"
    echo ">>> Building libSM ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 3. Build libXfixes (X Fixes Extension) ────────────────────────────────────
build_libXfixes() {
    fetch_pkg "libXfixes" "6.0.1" "https://gitlab.freedesktop.org/xorg/lib/libxfixes/-/archive/libXfixes-6.0.1/libxfixes-libXfixes-6.0.1.tar.gz"
    echo ">>> Building libXfixes ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 4. Build libXrender (X Render Extension) ──────────────────────────────────
build_libXrender() {
    fetch_pkg "libXrender" "0.9.11" "https://gitlab.freedesktop.org/xorg/lib/libxrender/-/archive/libXrender-0.9.11/libxrender-libXrender-0.9.11.tar.gz"
    echo ">>> Building libXrender ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 5. Build libXt (X Toolkit Intrinsics) ─────────────────────────────────────
build_libXt() {
    fetch_pkg "libXt" "1.3.0" "https://gitlab.freedesktop.org/xorg/lib/libxt/-/archive/libXt-1.3.0/libxt-libXt-1.3.0.tar.gz"
    echo ">>> Patching libXt for C99 compatibility (true keyword) ..."
    # Fix 'true' being used as variable name (conflicts with C99 bool keyword)
    sed -i 's/static Boolean true = True/static Boolean true_val = True/g' src/Shell.c
    sed -i 's/(\&true)/(\&true_val)/g' src/Shell.c
    echo ">>> Building libXt ..."
    # libXt needs some special handling for cross-compilation
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared \
        CFLAGS="-static -O2 -I$SYSROOT/include" \
        LDFLAGS="-static -L$SYSROOT/lib"
    make install
}

# ── 6. Build libXmu (X Miscellaneous Utilities) ───────────────────────────────
build_libXmu() {
    fetch_pkg "libXmu" "1.1.4" "https://gitlab.freedesktop.org/xorg/lib/libxmu/-/archive/libXmu-1.1.4/libxmu-libXmu-1.1.4.tar.gz"
    echo ">>> Building libXmu ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 7. Build libXi (X Input Extension) ────────────────────────────────────────
build_libXi() {
    fetch_pkg "libXi" "1.8.1" "https://gitlab.freedesktop.org/xorg/lib/libxi/-/archive/libXi-1.8.1/libxi-libXi-1.8.1.tar.gz"
    echo ">>> Building libXi ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared \
        CFLAGS="-static -O2 -I$SYSROOT/include" \
        LDFLAGS="-static -L$SYSROOT/lib"
    make install
}

# ── 8. Build xeyes ────────────────────────────────────────────────────────────
build_xeyes() {
    fetch_pkg "xeyes" "1.2.0" "https://gitlab.freedesktop.org/xorg/app/xeyes/-/archive/xeyes-1.2.0/xeyes-xeyes-1.2.0.tar.gz"
    echo ">>> Building xeyes ..."
    
    # Disable XRender and Present for simpler build (Kdrive doesn't fully support them)
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" \
        --with-xrender=no \
        --with-present=no \
        CFLAGS="-static -O2 -I$SYSROOT/include" \
        LDFLAGS="-static -L$SYSROOT/lib" \
        LIBS="-lXmu -lXt -lXext -lX11 -lXfixes -lXrender -lXi -lSM -lICE -lxcb -lXau -lXdmcp -lmd"
    make -j"$JOBS"
    
    # Copy to userland
    cp -f xeyes "$ROOT_DIR/userland/xeyes.elf"
    echo ">>> xeyes installed to $ROOT_DIR/userland/xeyes.elf"
}

# ── Main ─────────────────────────────────────────────────────────────────────

echo "========================================"
echo "    AscentOS Xt Stack & xeyes Builder  "
echo "========================================"

mkdir -p "$BUILD_BASE"
prepare_meson_cross

# Build Xt stack in order
build_libICE
build_libSM
build_libXfixes
build_libXrender
build_libXt
build_libXmu
build_libXi

# Build xeyes
build_xeyes

echo "========================================"
echo "xeyes built successfully!"
echo "Find it at: $ROOT_DIR/userland/xeyes.elf"
echo "========================================"
