#!/bin/sh
# AscentOS: Build GNU Tar for AscentOS
#
# This script cross-compiles GNU tar using the musl toolchain, producing
# a statically-linked binary that runs on AscentOS.
#
# Usage:
#   ./scripts/build-tar.sh          # Build and install tar
#   ./scripts/build-tar.sh clean    # Clean build directory

set -e

TAR_VERSION=1.35
TAR_TARBALL="tar-${TAR_VERSION}.tar.xz"
TAR_URL="https://ftp.gnu.org/gnu/tar/${TAR_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/tar-${TAR_VERSION}"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
TAR_INSTALL="${PREFIX}/bin"
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
    rm -f "$TAR_INSTALL/tar"
    echo "Done."
}

build_tar() {
    find_compiler

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Download tar source
    if [ ! -f "$TAR_TARBALL" ]; then
        echo "Downloading $TAR_URL ..."
        curl -L -o "$TAR_TARBALL" "$TAR_URL"
    fi

    # Extract
    if [ ! -d "tar-${TAR_VERSION}" ]; then
        echo "Extracting $TAR_TARBALL ..."
        tar xf "$TAR_TARBALL"
    fi

    cd "tar-${TAR_VERSION}"

    # Set up compiler tools
    TOOLCHAIN_BIN=$(dirname "$CC")
    export PATH="$TOOLCHAIN_BIN:$PATH"

    case "$CC" in
        *-gcc)
            _prefix=$(basename "$CC")
            _prefix="${_prefix%gcc}"
            
            if command -v "${TOOLCHAIN_BIN}/${_prefix}ar" >/dev/null 2>&1 || command -v "${_prefix}ar" >/dev/null 2>&1; then
                AR="${_prefix}ar"
            else
                AR="ar"
            fi
            
            if command -v "${TOOLCHAIN_BIN}/${_prefix}ranlib" >/dev/null 2>&1 || command -v "${_prefix}ranlib" >/dev/null 2>&1; then
                RANLIB="${_prefix}ranlib"
            else
                RANLIB="ranlib"
            fi
            
            if command -v "${TOOLCHAIN_BIN}/${_prefix}strip" >/dev/null 2>&1 || command -v "${_prefix}strip" >/dev/null 2>&1; then
                STRIP="${_prefix}strip"
            else
                STRIP="strip"
            fi
            ;;
        *)
            AR="ar"
            RANLIB="ranlib"
            STRIP="strip"
            ;;
    esac

    export CC AR RANLIB STRIP

    echo "Configuring GNU tar for AscentOS ..."

    # Cross-compile cache variables
    export gl_cv_func_getcwd_path_max=yes
    export gl_cv_func_getcwd_abort_bug=no
    export gl_cv_func_getcwd_null=yes
    export gl_cv_func_working_mktime=yes
    export gl_cv_func_working_utimes=yes
    export ac_cv_func_mmap_fixed_mapped=yes
    export gl_cv_double_slash_root=no
    export ac_cv_header_selinux_selinux_h=no
    export ac_cv_lib_selinux_setfilecon=no
    export ac_cv_func_setfilecon=no
    
    # Disable features not supported by AscentOS
    ./configure \
        --host=x86_64-linux-musl \
        --prefix=/ \
        --disable-nls \
        --disable-acl \
        --disable-xattr \
        --without-selinux \
        --without-posix-acls \
        --without-xattrs \
        CFLAGS="-static -O2 -fno-stack-protector -I$PREFIX/include" \
        LDFLAGS="-static -L$PREFIX/lib"

    echo "Building GNU tar ..."
    make -j"$JOBS"

    # Install binary
    echo "Installing tar to $TAR_INSTALL ..."
    mkdir -p "$TAR_INSTALL"
    cp src/tar "$TAR_INSTALL/tar"

    # Strip the binary
    if command -v "$STRIP" >/dev/null 2>&1; then
        echo "Stripping tar binary ..."
        "$STRIP" "$TAR_INSTALL/tar"
    fi

    cd "$ROOT_DIR"

    echo ""
    echo "========================================"
    echo "GNU tar ${TAR_VERSION} built successfully!"
    echo "========================================"
    echo ""
    ls -lh "$TAR_INSTALL/tar"
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_tar
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
