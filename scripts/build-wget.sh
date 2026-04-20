#!/bin/sh
# AscentOS: Build GNU Wget 1.24.5 for AscentOS
#
# This script cross-compiles Wget using the musl toolchain, producing a
# statically-linked binary.

set -e

WGET_VERSION="1.24.5"
WGET_TARBALL="wget-${WGET_VERSION}.tar.gz"
WGET_URL="https://ftp.gnu.org/gnu/wget/${WGET_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/wget-${WGET_VERSION}"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
INSTALL_DIR="$PREFIX/opt/wget"
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

build_wget() {
    find_compiler
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    if [ ! -f "$WGET_TARBALL" ]; then
        echo "Downloading $WGET_URL ..."
        curl -L -o "$WGET_TARBALL" "$WGET_URL"
    fi
    
    if [ ! -d "wget-${WGET_VERSION}" ]; then
        echo "Extracting $WGET_TARBALL ..."
        tar xf "$WGET_TARBALL"
    else
        echo "Source exists. Cleaning for a fresh configuration..."
        cd "wget-${WGET_VERSION}"
        make distclean || true
        cd ..
    fi
    
    cd "wget-${WGET_VERSION}"

    echo "Patching Wget for WolfSSL compatibility..."
    # 1. Comment out the HPKP function call as it's missing i2d_X509_PUBKEY
    # We'll just make pkp_pin_peer_pubkey always return true.
    sed -i 's/result = wg_pin_peer_pubkey (pinnedpubkey, buff1, len1);/result = true; (void)buff1; (void)len1;/' src/openssl.c
    # Also stub out the X509_get_X509_PUBKEY and i2d_X509_PUBKEY calls which fail
    sed -i 's/i2d_X509_PUBKEY (X509_get_X509_PUBKEY (cert),/(-1); \/\/ i2d_X509_PUBKEY (NULL,/g' src/openssl.c
    
    # 2. Add compatibility defines to cover older OpenSSL naming wget expects
    # and fix the XMALLOC conflict. We MUST include config.h first.
    echo "#include <config.h>" > src/wolfssl_fix.h
    echo "#include <wolfssl/options.h>" >> src/wolfssl_fix.h
    echo "#include <wolfssl/wolfcrypt/settings.h>" >> src/wolfssl_fix.h
    echo "#include <wolfssl/ssl.h>" >> src/wolfssl_fix.h
    echo "#include <wolfssl/openssl/ssl.h>" >> src/wolfssl_fix.h
    echo "#include <wolfssl/openssl/err.h>" >> src/wolfssl_fix.h
    
    echo "#define BIO_number_written(b) BIO_ctrl_pending(b)" >> src/wolfssl_fix.h
    
    # Explicitly map types in case they are hidden
    echo "typedef struct WOLFSSL_CTX SSL_CTX;" >> src/wolfssl_fix.h
    echo "typedef struct WOLFSSL_METHOD SSL_METHOD;" >> src/wolfssl_fix.h
    echo "typedef struct WOLFSSL_SESSION SSL_SESSION;" >> src/wolfssl_fix.h
    echo "typedef struct WOLFSSL_X509 X509;" >> src/wolfssl_fix.h
    echo "typedef struct WOLFSSL_X509_NAME X509_NAME;" >> src/wolfssl_fix.h
    
    # Map methods
    echo "#define SSLv23_client_method wolfSSLv23_client_method" >> src/wolfssl_fix.h
    echo "#define SSLv3_client_method wolfSSLv23_client_method" >> src/wolfssl_fix.h
    echo "#define TLSv1_client_method wolfSSLv23_client_method" >> src/wolfssl_fix.h
    echo "#define TLSv1_1_client_method wolfSSLv23_client_method" >> src/wolfssl_fix.h
    echo "#define TLSv1_2_client_method wolfSSLv23_client_method" >> src/wolfssl_fix.h
    
    # Prevent WolfSSL's MIN/MAX from clashing with wget's
    echo "#undef MIN" >> src/wolfssl_fix.h
    echo "#undef MAX" >> src/wolfssl_fix.h
    
    # Force include the fix at the top of openssl.c
    sed -i '1i #include "wolfssl_fix.h"' src/openssl.c
    
    case "$CC" in
        *-gcc)
            _prefix="${CC%gcc}"
            if command -v "${_prefix}ar" >/dev/null 2>&1; then AR="${_prefix}ar"; else AR="ar"; fi
            if command -v "${_prefix}ranlib" >/dev/null 2>&1; then RANLIB="${_prefix}ranlib"; else RANLIB="ranlib"; fi
            if command -v "${_prefix}strip" >/dev/null 2>&1; then STRIP="${_prefix}strip"; else STRIP="strip"; fi
            ;;
        *) AR="ar"; RANLIB="ranlib"; STRIP="strip" ;;
    esac
    
    export CC AR RANLIB
    
    echo "Configuring Wget for AscentOS ..."
    
    # We pass OPENSSL_CFLAGS and OPENSSL_LIBS directly because pkg-config
    # might not be configured for our cross-environment. We installed wolfssl
    # into the PREFIX standard directories.
    ./configure \
        --host=x86_64-linux-musl \
        --prefix="$INSTALL_DIR" \
        --disable-nls \
        --without-libidn \
        --without-libidn2 \
        --without-libuuid \
        --without-libpsl \
        --without-zlib \
        --disable-pcre \
        --disable-pcre2 \
        --disable-iri \
        --disable-ntlm \
        --with-ssl=openssl \
        OPENSSL_CFLAGS="-I$PREFIX/include -I$PREFIX/include/wolfssl -DOPENSSL_EXTRA -DOPENSSL_ALL -DWOLFSSL_QT -DEXTERNAL_OPTS_OPENSLEEXTRA" \
        OPENSSL_LIBS="-L$PREFIX/lib -lwolfssl" \
        CFLAGS="-static -O2 -I$PREFIX/include -I$PREFIX/include/wolfssl -DOPENSSL_EXTRA -DOPENSSL_ALL -DWOLFSSL_QT -DEXTERNAL_OPTS_OPENSLEEXTRA" \
        LDFLAGS="-static -L$PREFIX/lib"
        
    echo "Building Wget ..."
    make -j"$JOBS"
    
    echo "Installing Wget to $INSTALL_DIR ..."
    make install
    
    if command -v "$STRIP" >/dev/null 2>&1; then
        echo "Stripping wget binary ..."
        "$STRIP" "$INSTALL_DIR/bin/wget"
    fi
    
    # Also place an easy-to-grab fully static binary in userland
    cp "$INSTALL_DIR/bin/wget" "$ROOT_DIR/userland/wget.elf"
    
    cd "$ROOT_DIR"
    echo "=========================================="
    echo "Wget ${WGET_VERSION} built successfully!"
    echo "=========================================="
}

case "${1:-build}" in
    clean)
        echo "Cleaning $BUILD_DIR ..."
        rm -rf "$BUILD_DIR"
        ;;
    build|"")
        build_wget
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
