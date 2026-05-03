#!/bin/sh
# AscentOS: Build midiplay library for AscentOS
#
# This script cross-compiles the midiplay library (pure C MIDI player with
# Nuked OPL3 emulation) using the musl toolchain.
#
# Prerequisites:
#   - Run scripts/musl-toolchain.sh first to build the musl toolchain
#   - Or have x86_64-linux-musl-gcc available on PATH
#
# Usage:
#   ./scripts/build-midiplay.sh          # Build midiplay library
#   ./scripts/build-midiplay.sh clean    # Clean build directory

set -e

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MIDIPLAY_SRC="${ROOT_DIR}/userland/midiplay-src"
BUILD_DIR="${ROOT_DIR}/build/midiplay"
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

# Clean build directory
do_clean() {
    echo "Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    rm -f "${MIDIPLAY_SRC}/opl.o" "${MIDIPLAY_SRC}/midiplay.o"
    echo "Done."
}

# Build midiplay as a static library
build_midiplay() {
    find_compiler

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

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

    echo "Using CC=$CC AR=$AR RANLIB=$RANLIB"

    CFLAGS="-static -O2 -Wall -fno-stack-protector -I$PREFIX/include"
    LDFLAGS="-static -L$PREFIX/lib"

    echo "Compiling OPL3 emulator..."
    $CC $CFLAGS -c "${MIDIPLAY_SRC}/opl.c" -o opl.o

    echo "Compiling MIDI player..."
    $CC $CFLAGS -c "${MIDIPLAY_SRC}/midiplay.c" -o midiplay.o

    echo "Creating static library..."
    $AR rcs libmidiplay.a opl.o midiplay.o
    $RANLIB libmidiplay.a

    # Also copy to midiplay-src for convenience
    cp libmidiplay.a "${MIDIPLAY_SRC}/"

    echo ""
    echo "========================================"
    echo "midiplay library built successfully!"
    echo "========================================"
    echo ""
    echo "Library: $BUILD_DIR/libmidiplay.a"
    echo "Also copied to: ${MIDIPLAY_SRC}/libmidiplay.a"
    echo ""
    echo "To use in Doom, update Makefile.ascentos to link with libmidiplay.a"
    echo ""
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_midiplay
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
