#!/bin/sh
# AscentOS: Build Lua 5.4.7 for AscentOS
#
# Lua is minimal and self-contained - just needs a C compiler and libc.
# This script cross-compiles Lua using the musl toolchain.
#
# Usage:
#   ./scripts/build-lua.sh          # Build Lua
#   ./scripts/build-lua.sh clean    # Clean build directory

set -e

LUA_VERSION="5.4.7"
LUA_TARBALL="lua-${LUA_VERSION}.tar.gz"
LUA_URL="https://www.lua.org/ftp/${LUA_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/lua-${LUA_VERSION}"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
INSTALL_DIR="$PREFIX/opt/lua"
JOBS=$(nproc 2>/dev/null || echo 4)

find_compiler() {
    LOCAL_CC="$ROOT_DIR/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"
    
    if [ -x "$LOCAL_CC" ]; then
        CC="$LOCAL_CC"
    elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
        CC="x86_64-linux-musl-gcc"
    elif command -v musl-gcc >/dev/null 2>&1; then
        CC="musl-gcc"
    else
        echo "Error: No musl compiler found. Run scripts/musl-toolchain.sh first." >&2
        exit 1
    fi
    echo "Using CC=$CC"
}

do_clean() {
    echo "Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    rm -rf "$INSTALL_DIR"
    rm -f "$ROOT_DIR/userland/lua.elf"
    echo "Done."
}

build_lua() {
    find_compiler
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Download Lua source
    if [ ! -f "$LUA_TARBALL" ]; then
        echo "Downloading $LUA_URL ..."
        curl -L -o "$LUA_TARBALL" "$LUA_URL"
    fi
    
    # Extract
    if [ ! -d "lua-${LUA_VERSION}" ]; then
        echo "Extracting $LUA_TARBALL ..."
        tar xf "$LUA_TARBALL"
    fi
    
    cd "lua-${LUA_VERSION}"
    
    # Find toolchain tools
    case "$CC" in
        *-gcc)
            _prefix="${CC%gcc}"
            if command -v "${_prefix}ar" >/dev/null 2>&1; then AR="${_prefix}ar"; else AR="ar"; fi
            if command -v "${_prefix}ranlib" >/dev/null 2>&1; then RANLIB="${_prefix}ranlib"; else RANLIB="ranlib"; fi
            if command -v "${_prefix}strip" >/dev/null 2>&1; then STRIP="${_prefix}strip"; else STRIP="strip"; fi
            ;;
        *) AR="ar"; RANLIB="ranlib"; STRIP="strip" ;;
    esac
    
    echo "Building Lua for AscentOS ..."
    
    # Lua uses a simple Makefile. We build for "linux" platform with static linking.
    # MYCFLAGS/MYLDFLAGS allow custom compiler flags.
    # We disable readline to avoid dependency.
    make -j"$JOBS" \
        PLAT=linux \
        CC="$CC" \
        AR="$AR rc" \
        RANLIB="$RANLIB" \
        MYCFLAGS="-static -O2 -fno-stack-protector -I$PREFIX/include" \
        MYLDFLAGS="-static -L$PREFIX/lib" \
        MYLIBS="-lm" \
        LUA_A="liblua.a" \
        LUA_T="lua" \
        LUAC_T="luac" \
        linux
    
    # Install to PREFIX/opt/lua
    echo "Installing Lua to $INSTALL_DIR ..."
    mkdir -p "$INSTALL_DIR/bin"
    cp src/lua "$INSTALL_DIR/bin/"
    cp src/luac "$INSTALL_DIR/bin/"
    
    # Strip binaries
    if command -v "$STRIP" >/dev/null 2>&1; then
        echo "Stripping Lua binaries ..."
        "$STRIP" "$INSTALL_DIR/bin/lua"
        "$STRIP" "$INSTALL_DIR/bin/luac"
    fi
    
    # Copy to userland for easy access
    cp "$INSTALL_DIR/bin/lua" "$ROOT_DIR/userland/lua.elf"
    
    cd "$ROOT_DIR"
    
    echo ""
    echo "========================================"
    echo "Lua ${LUA_VERSION} built successfully!"
    echo "========================================"
    echo ""
    echo "Binary: $INSTALL_DIR/bin/lua"
    echo "Also:   $ROOT_DIR/userland/lua.elf"
    echo ""
    echo "To test on AscentOS: exec /mnt/lua.elf"
    echo ""
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_lua
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
