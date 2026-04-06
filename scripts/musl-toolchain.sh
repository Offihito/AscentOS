#!/bin/sh
# AscentOS: bootstrap x86_64-linux-musl (musl-cross-make) if needed, then install
# musl 1.2.5 into a static sysroot under toolchain/musl-sysroot.
#
# Environment (optional):
#   MUSL_TOOLCHAIN_DIR  — where gcc lands (default: $ROOT/toolchain/x86_64-linux-musl)
#   MUSL_SYSROOT        — musl install prefix (default: $ROOT/toolchain/musl-sysroot)
#   MUSL_SKIP_BOOTSTRAP — if set, never run musl-cross-make (fail if no usable CC)
#
# Usage:
#   ./scripts/musl-toolchain.sh          # ensure compiler + build/install sysroot
#   ./scripts/musl-toolchain.sh bootstrap # only musl-cross-make install
#   ./scripts/musl-toolchain.sh sysroot   # only musl libc (needs CC on PATH or under toolchain)

set -e

MUSL_VERSION=1.2.5
MUSL_TARBALL="musl-${MUSL_VERSION}.tar.gz"
MUSL_URL="https://musl.libc.org/releases/${MUSL_TARBALL}"
TARGET=x86_64-linux-musl

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTPUT=${MUSL_TOOLCHAIN_DIR:-"$ROOT_DIR/toolchain/x86_64-linux-musl"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/musl-${MUSL_VERSION}"}
MUSL_CROSS_MAKE_DIR=${MUSL_CROSS_MAKE_DIR:-"$ROOT_DIR/build/musl-cross-make"}
JOBS=$(nproc 2>/dev/null || echo 4)
LOCAL_CROSS_GCC="$OUTPUT/bin/x86_64-linux-musl-gcc"

bootstrap_toolchain() {
	if [ -n "${MUSL_SKIP_BOOTSTRAP:-}" ]; then
		echo "MUSL_SKIP_BOOTSTRAP is set but no cross compiler found." >&2
		exit 1
	fi
	mkdir -p "$(dirname "$MUSL_CROSS_MAKE_DIR")"
	if [ ! -f "$MUSL_CROSS_MAKE_DIR/Makefile" ]; then
		echo "Cloning musl-cross-make -> $MUSL_CROSS_MAKE_DIR"
		rm -rf "$MUSL_CROSS_MAKE_DIR"
		git clone --depth=1 https://github.com/richfelker/musl-cross-make.git \
			"$MUSL_CROSS_MAKE_DIR"
	fi
	cd "$MUSL_CROSS_MAKE_DIR"
	echo "Building $TARGET -> $OUTPUT (this can take a long time) ..."
	make -j"$JOBS" TARGET="$TARGET" OUTPUT="$OUTPUT" install
	cd "$ROOT_DIR"
}

pick_compiler() {
	CC=${CC:-}
	CC_BIN=""
	if [ -z "$CC" ] && [ -x "$LOCAL_CROSS_GCC" ]; then
		CC=$LOCAL_CROSS_GCC
	fi
	if [ -z "$CC" ] && command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
		CC=x86_64-linux-musl-gcc
	fi
	if [ -z "$CC" ] && command -v musl-gcc >/dev/null 2>&1; then
		CC=musl-gcc
	fi
	if [ -z "$CC" ]; then
		return 1
	fi
	CC_BIN=$(printf '%s\n' "$CC" | awk '{print $1}')
	if [ -x "$CC_BIN" ]; then
		return 0
	fi
	if command -v "$CC_BIN" >/dev/null 2>&1; then
		return 0
	fi
	return 1
}

ensure_compiler() {
	if pick_compiler; then
		return 0
	fi
	echo "No x86_64-linux-musl-gcc or musl-gcc found; bootstrapping toolchain ..."
	bootstrap_toolchain
	export PATH="$OUTPUT/bin:$PATH"
	CC="$LOCAL_CROSS_GCC"
	CC_BIN="$LOCAL_CROSS_GCC"
	if [ ! -x "$CC_BIN" ]; then
		echo "Bootstrap failed: missing $LOCAL_CROSS_GCC" >&2
		exit 1
	fi
}

build_sysroot() {
	ensure_compiler

	echo "Using CC=$CC"

	HOST_TRIPLET=${HOST_TRIPLET:-}
	if [ -z "$HOST_TRIPLET" ]; then
		HOST_TRIPLET=$($CC_BIN -dumpmachine 2>/dev/null) || true
	fi
	if [ -z "$HOST_TRIPLET" ]; then
		HOST_TRIPLET=x86_64-linux-musl
	fi
	echo "Using --host=$HOST_TRIPLET"

	AR=${AR:-}
	RANLIB=${RANLIB:-}
	case "$CC_BIN" in
	*-gcc)
		_tpref="${CC_BIN%gcc}"
		if [ -z "$AR" ] && command -v "${_tpref}ar" >/dev/null 2>&1; then
			AR=${_tpref}ar
		fi
		if [ -z "$RANLIB" ] && command -v "${_tpref}ranlib" >/dev/null 2>&1; then
			RANLIB=${_tpref}ranlib
		fi
		;;
	esac
	AR=${AR:-ar}
	RANLIB=${RANLIB:-ranlib}
	export AR RANLIB
	echo "Using AR=$AR RANLIB=$RANLIB"

	mkdir -p "$BUILD_DIR"
	cd "$BUILD_DIR"
	if [ ! -f "$MUSL_TARBALL" ]; then
		echo "Downloading $MUSL_URL ..."
		curl -L -o "$MUSL_TARBALL" "$MUSL_URL"
	fi
	rm -rf "musl-${MUSL_VERSION}"
	tar xf "$MUSL_TARBALL"
	cd "musl-${MUSL_VERSION}"

	./configure \
		--prefix="$PREFIX" \
		--disable-shared \
		--enable-static \
		--host="$HOST_TRIPLET" \
		AR="$AR" \
		RANLIB="$RANLIB"

	make -j"$JOBS"
	make install
	cd "$ROOT_DIR"
	echo "musl ${MUSL_VERSION} installed to: $PREFIX"
}

case "${1:-all}" in
bootstrap)
	bootstrap_toolchain
	echo "Toolchain installed under: $OUTPUT"
	echo "Add to PATH: export PATH=\"$OUTPUT/bin:\$PATH\""
	;;
sysroot)
	build_sysroot
	;;
all | "")
	if [ -x "$LOCAL_CROSS_GCC" ]; then
		export PATH="$OUTPUT/bin:$PATH"
	fi
	build_sysroot
	;;
*)
	echo "Usage: $0 [all|bootstrap|sysroot]" >&2
	exit 1
	;;
esac
