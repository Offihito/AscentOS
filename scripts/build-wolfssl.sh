#!/bin/sh
# AscentOS: Build WolfSSL v5.7.0 for AscentOS
#
# This script cross-compiles WolfSSL using the musl toolchain, producing a
# static library that can be used by other userland programs.

set -e

WOLFSSL_VERSION="5.7.0"
WOLFSSL_TARBALL="v${WOLFSSL_VERSION}-stable.tar.gz"
WOLFSSL_URL="https://github.com/wolfSSL/wolfssl/archive/refs/tags/${WOLFSSL_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/wolfssl-${WOLFSSL_VERSION}"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
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

# Build WolfSSL
build_wolfssl() {
    find_compiler
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Download WolfSSL source
    if [ ! -f "$WOLFSSL_TARBALL" ]; then
        echo "Downloading $WOLFSSL_URL ..."
        curl -L -o "$WOLFSSL_TARBALL" "$WOLFSSL_URL"
    fi
    
    # Extract
    if [ ! -d "wolfssl-${WOLFSSL_VERSION}-stable" ]; then
        echo "Extracting $WOLFSSL_TARBALL ..."
        tar xf "$WOLFSSL_TARBALL"
    fi
    
    cd "wolfssl-${WOLFSSL_VERSION}-stable"

    # Need to generate configure script as it's a GitHub tarball
    if [ ! -f "./configure" ]; then
        echo "Generating configure script ..."
        ./autogen.sh
    fi
    
    # Set up compiler tools
    case "$CC" in
        *-gcc)
            _prefix="${CC%gcc}"
            if command -v "${_prefix}ar" >/dev/null 2>&1; then
                AR="${_prefix}ar"
            else
                AR="ar"
            fi
            if command -v "${_prefix}ranlib" >/dev/null 2>&1; then
                RANLIB="${_prefix}ranlib"
            else
                RANLIB="ranlib"
            fi
            ;;
        *)
            AR="ar"
            RANLIB="ranlib"
            ;;
    esac
    
    export CC AR RANLIB
    
    echo "Configuring WolfSSL for AscentOS ..."
    
    ./configure \
        --host=x86_64-linux-musl \
        --prefix="$PREFIX" \
        --enable-static \
        --disable-shared \
        --enable-tls13 \
        --enable-all \
        --enable-opensslextra \
        CFLAGS="-static -O2 -I$PREFIX/include" \
        LDFLAGS="-static -L$PREFIX/lib"
    
    echo "Building WolfSSL ..."
    make -j"$JOBS"
    
    echo "Installing WolfSSL to $PREFIX ..."
    make install
    
    cd "$ROOT_DIR"
    
    echo ""
    echo "=========================================="
    echo "WolfSSL ${WOLFSSL_VERSION} built successfully!"
    echo "=========================================="
}

case "${1:-build}" in
    clean)
        echo "Cleaning $BUILD_DIR ..."
        rm -rf "$BUILD_DIR"
        ;;
    build|"")
        build_wolfssl
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
