#!/bin/sh
# AscentOS: Build GNU Coreutils 9.5 for AscentOS
#
# This script cross-compiles Coreutils using the musl toolchain, producing
# statically-linked binaries that run on AscentOS.
#
# Usage:
#   ./scripts/build-coreutils.sh          # Build and install Coreutils
#   ./scripts/build-coreutils.sh clean    # Clean build directory

set -e

COREUTILS_VERSION=9.5
COREUTILS_TARBALL="coreutils-${COREUTILS_VERSION}.tar.xz"
COREUTILS_URL="https://ftp.gnu.org/gnu/coreutils/${COREUTILS_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/coreutils-${COREUTILS_VERSION}"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
COREUTILS_INSTALL="${PREFIX}/opt/coreutils"
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
    rm -rf "$COREUTILS_INSTALL"
    echo "Done."
}

build_coreutils() {
    find_compiler

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Download Coreutils source
    if [ ! -f "$COREUTILS_TARBALL" ]; then
        echo "Downloading $COREUTILS_URL ..."
        curl -L -o "$COREUTILS_TARBALL" "$COREUTILS_URL"
    fi

    # Extract
    if [ ! -d "coreutils-${COREUTILS_VERSION}" ]; then
        echo "Extracting $COREUTILS_TARBALL ..."
        tar xf "$COREUTILS_TARBALL"
    fi

    cd "coreutils-${COREUTILS_VERSION}"

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

    echo "Configuring Coreutils for AscentOS ..."

    # Cross-compile cache variables to avoid configure test failures
    export gl_cv_func_getcwd_path_max=yes
    export gl_cv_func_getcwd_abort_bug=no
    export gl_cv_func_getcwd_null=yes
    export gl_cv_func_working_mktime=yes
    export gl_cv_func_working_utimes=yes
    export ac_cv_func_mmap_fixed_mapped=yes
    export fu_cv_sys_mounted_getmntent1=yes
    export gl_cv_double_slash_root=no
    export gl_cv_func_nanosleep_momentary_interruption=no
    export ac_cv_header_selinux_selinux_h=no
    export ac_cv_lib_selinux_setfilecon=no
    
    # Disable features not supported by AscentOS kernel
    ./configure \
        --host=x86_64-linux-musl \
        --prefix=/opt/coreutils \
        --disable-nls \
        --disable-acl \
        --disable-xattr \
        --disable-libcap \
        --disable-rpath \
        --enable-single-binary=symlinks \
        --enable-no-install-program=stdbuf,timeout,chroot \
        CFLAGS="-static -O2 -fno-stack-protector -I$PREFIX/include" \
        LDFLAGS="-static -L$PREFIX/lib"

    echo "Building Coreutils (this may take a while) ..."
    make -j"$JOBS"

    # Install to temporary location
    echo "Installing Coreutils to $COREUTILS_INSTALL ..."
    make DESTDIR="$BUILD_DIR/dest" install

    # Fix up the installation for AscentOS
    mkdir -p "$COREUTILS_INSTALL"
    cp -r "$BUILD_DIR/dest/opt/coreutils"/* "$COREUTILS_INSTALL/"

    # Strip the binaries
    if command -v "$STRIP" >/dev/null 2>&1; then
        echo "Stripping coreutils binaries ..."
        find "$COREUTILS_INSTALL/bin" -type f -executable -exec "$STRIP" {} + || true
    fi

    cd "$ROOT_DIR"

    echo ""
    echo "========================================"
    echo "Coreutils ${COREUTILS_VERSION} built successfully!"
    echo "========================================"
    echo ""
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_coreutils
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
