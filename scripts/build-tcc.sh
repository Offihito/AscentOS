#!/bin/sh
# AscentOS: Build Tiny C Compiler (TCC) for AscentOS
# 
# This script cross-compiles TCC using the musl toolchain, producing a
# statically-linked tcc binary that runs on AscentOS.
#
# Prerequisites:
#   - Run scripts/musl-toolchain.sh first to build the musl toolchain
#   - Or have x86_64-linux-musl-gcc available on PATH
#
# Usage:
#   ./scripts/build-tcc.sh          # Build and install TCC
#   ./scripts/build-tcc.sh clean    # Clean build directory
#
# Environment:
#   MUSL_SYSROOT - musl sysroot path (default: $ROOT/toolchain/musl-sysroot)
#   TCC_VERSION  - TCC version to build (default: 0.9.27)

set -e

TCC_VERSION=${TCC_VERSION:-0.9.27}
TCC_TARBALL="tcc-${TCC_VERSION}.tar.bz2"
TCC_URL="https://download.savannah.gnu.org/releases/tinycc/${TCC_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/tcc-${TCC_VERSION}"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
TCC_INSTALL="${PREFIX}/opt/tcc"
JOBS=$(nproc 2>/dev/null || echo 4)

# Find musl compiler
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

# Clean build directory
do_clean() {
    echo "Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    rm -rf "$TCC_INSTALL"
    echo "Done."
}

# Build TCC
build_tcc() {
    find_compiler
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Download TCC source
    if [ ! -f "$TCC_TARBALL" ]; then
        echo "Downloading $TCC_URL ..."
        curl -L -o "$TCC_TARBALL" "$TCC_URL"
    fi
    
    # Extract (or recover from partial/stale source tree)
    if [ ! -d "tcc-${TCC_VERSION}" ]; then
        echo "Extracting $TCC_TARBALL ..."
        tar xf "$TCC_TARBALL"
    elif [ ! -f "tcc-${TCC_VERSION}/configure" ]; then
        echo "Source tree exists but is incomplete; re-extracting..."
        rm -rf "tcc-${TCC_VERSION}"
        tar xf "$TCC_TARBALL"
    fi
    
    cd "tcc-${TCC_VERSION}"
    
    # Configure for AscentOS (x86_64-linux-musl target)
    # TCC will be a native compiler running on AscentOS
    echo "Configuring TCC for AscentOS ..."
    
    # Set up compiler tools
    case "$CC" in
        *-gcc)
            AR="${CC%gcc}ar"
            RANLIB="${CC%gcc}ranlib"
            STRIP="${CC%gcc}strip"
            ;;
        *)
            AR="ar"
            RANLIB="ranlib"
            STRIP="strip"
            ;;
    esac
    
    export CC AR RANLIB
    
    # Pre-create TCC include/lib directories and copy musl headers
    # This is needed BEFORE building so TCC can find headers for libtcc1.a
    echo "Setting up include/library paths before build..."
    mkdir -p "$TCC_INSTALL/lib/tcc/include"
    mkdir -p "$TCC_INSTALL/lib/tcc/lib"
    
    if [ -d "$PREFIX/include" ]; then
        cp -r "$PREFIX/include"/* "$TCC_INSTALL/lib/tcc/include/"
    fi
    
    # Copy musl libs and crt files
    for f in libc.a libm.a crt1.o crti.o crtn.o Scrt1.o rcrt1.o; do
        if [ -f "$PREFIX/lib/$f" ]; then
            cp "$PREFIX/lib/$f" "$TCC_INSTALL/lib/tcc/lib/"
        fi
    done

    # Rebuild crt1.o from our custom assembly source if available
    TCC_CRT1_SRC="$ROOT_DIR/toolchain/tcc-crt1.S"
    if [ -f "$TCC_CRT1_SRC" ]; then
        echo "Building TCC-compatible crt1.o from custom source..."
        "$CC" -c -nostdlib -fno-pic -fno-pie -o "$TCC_INSTALL/lib/tcc/lib/crt1.o" "$TCC_CRT1_SRC"
    fi

    
    # Configure options:
    # --config-musl: configure for musl libc (key for proper builds)
    # --prefix: installation directory (build-time location)
    # --cc: C compiler to use for building TCC
    # --enable-static: build static tcc (no libtcc.so)
    # --disable-rpath: no rpath in binaries
    # --tccdir: where TCC looks for its libs/includes AT RUNTIME on AscentOS
    # --crtprefix: where to find crt1.o, crti.o, crtn.o at runtime
    #
    # IMPORTANT: These paths are baked into the tcc binary at compile time.
    # They must match the runtime filesystem layout on AscentOS (/opt/tcc/...),
    # NOT the build-time location ($TCC_INSTALL).
    RUNTIME_TCCDIR="/opt/tcc/lib/tcc"
    RUNTIME_LIBDIR="/opt/tcc/lib/tcc/lib"

    ./configure \
        --config-musl \
        --prefix="$TCC_INSTALL" \
        --cc="$CC" \
        --enable-static \
        --disable-rpath \
        --tccdir="$RUNTIME_TCCDIR" \
        --crtprefix="$RUNTIME_LIBDIR" \
        --sysincludepaths="$RUNTIME_TCCDIR/include" \
        --libpaths="$RUNTIME_LIBDIR"
    
    # Build - configure already set the correct runtime paths
    echo "Building TCC (this may take a while) ..."
    make -j"$JOBS" \
        CFLAGS="-static -O2 -Wall -fno-stack-protector -I$PREFIX/include" \
        LDFLAGS="-static -L$PREFIX/lib"
    
    # Manual install to avoid writing to /opt/tcc as root
    echo "Installing TCC to $TCC_INSTALL ..."
    mkdir -p "$TCC_INSTALL/bin"
    mkdir -p "$TCC_INSTALL/lib/tcc"
    mkdir -p "$TCC_INSTALL/share/man/man1"
    
    cp tcc "$TCC_INSTALL/bin/"
    cp libtcc1.a "$TCC_INSTALL/lib/tcc/"
    cp libtcc.a "$TCC_INSTALL/lib/" 2>/dev/null || true
    cp tcc.1 "$TCC_INSTALL/share/man/man1/" 2>/dev/null || true
    
    # Strip the binary
    if [ -x "$STRIP" ]; then
        echo "Stripping tcc binary ..."
        "$STRIP" "$TCC_INSTALL/bin/tcc"
    fi
    
    # Headers and libs already copied before build
    echo "Verifying TCC installation..."
    
    cd "$ROOT_DIR"
    
    echo ""
    echo "========================================"
    echo "TCC ${TCC_VERSION} built successfully!"
    echo "========================================"
    echo ""
    echo "Binary: $TCC_INSTALL/bin/tcc"
    echo "Libs:   $TCC_INSTALL/lib/tcc"
    echo ""
    echo "To run on AscentOS, copy the entire tcc directory:"
    echo "  cp -r $TCC_INSTALL /path/to/ascentos/root/opt/tcc"
    echo ""
    echo "Or add to your initrd/filesystem image."
    echo ""
    echo "Usage on AscentOS:"
    echo "  tcc -o program program.c"
    echo "  tcc -run program.c      # interpret mode"
    echo ""
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_tcc
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac