#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  AscentOS — newlib Port Build Script
#  Kullanım: chmod +x newlib-build.sh && ./newlib-build.sh
#
#  Ne yapar?
#    1. newlib kaynak kodunu indirir
#    2. x86_64-ascentos-elf hedefi için configure eder
#    3. Derler ve PREFIX altına kurar
#    4. Kütüphaneleri userland/libc/newlib/ altına kopyalar
# ═══════════════════════════════════════════════════════════════

set -e  # herhangi bir hata → dur

# ── Ayarlar ───────────────────────────────────────────────────
NEWLIB_VER="4.4.0.20231231"        # Kararlı son sürüm
NEWLIB_URL="https://sourceware.org/pub/newlib/newlib-${NEWLIB_VER}.tar.gz"
TARGET="x86_64-elf"               # x86_64-elf-gcc varsa bu; özel triple varsa değiştir
PREFIX="$(pwd)/toolchain/newlib-install"
JOBS=$(nproc)
DEST="$(pwd)/userland/libc/newlib"

# ── Renkli çıktı ──────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Ön koşul kontrolü ─────────────────────────────────────────
info "Ön koşullar kontrol ediliyor..."

command -v ${TARGET}-gcc  >/dev/null 2>&1 || \
    error "${TARGET}-gcc bulunamadı. x86_64-elf cross-compiler PATH'te olmalı."

command -v ${TARGET}-ar   >/dev/null 2>&1 || \
    error "${TARGET}-ar bulunamadı."

GCC_VER=$(${TARGET}-gcc --version | head -1)
info "Compiler: ${GCC_VER}"

# ── İndirme ───────────────────────────────────────────────────
ARCHIVE="newlib-${NEWLIB_VER}.tar.gz"

if [ ! -f "${ARCHIVE}" ]; then
    info "newlib ${NEWLIB_VER} indiriliyor..."
    curl -L "${NEWLIB_URL}" -o "${ARCHIVE}" || \
        error "İndirme başarısız. URL: ${NEWLIB_URL}"
else
    info "Arşiv zaten mevcut: ${ARCHIVE}"
fi

# ── Çıkarma ───────────────────────────────────────────────────
SRC_DIR="newlib-${NEWLIB_VER}"

if [ ! -d "${SRC_DIR}" ]; then
    info "Arşiv çıkarılıyor..."
    tar xf "${ARCHIVE}"
fi

# ── Build dizini ──────────────────────────────────────────────
BUILD_DIR="build-newlib"
rm -rf "${BUILD_DIR}"
mkdir  "${BUILD_DIR}"

# ── Configure ─────────────────────────────────────────────────
info "Configure ediliyor (target=${TARGET})..."

cd "${BUILD_DIR}"

# Önemli flag açıklamaları:
#   --disable-newlib-supplied-syscalls → newlib kendi syscall stub'larını YAZMASIN
#                                        Biz syscalls.c ile yazacağız
#   --enable-newlib-reent-small        → küçük reentrancy yapısı (OS için yeterli)
#   --disable-newlib-fvwrite-in-streamio → buffer flush optimizasyonu kapat
#   --disable-multilib                 → sadece x86_64, çok kütüphane üretme

../${SRC_DIR}/configure \
    --target="${TARGET}"                        \
    --prefix="${PREFIX}"                        \
    --disable-newlib-supplied-syscalls          \
    --enable-newlib-reent-small                 \
    --disable-newlib-fvwrite-in-streamio        \
    --disable-newlib-wide-orient                \
    --enable-newlib-nano-malloc                 \
    --disable-nls                               \
    --disable-multilib                          \
    CFLAGS_FOR_TARGET="-O2 -ffreestanding -ffunction-sections -fdata-sections -mcmodel=small"

# ── Derleme ───────────────────────────────────────────────────
info "Derleniyor (${JOBS} iş parçacığı)..."
make -j"${JOBS}"

# ── Kurulum ───────────────────────────────────────────────────
info "Kuruluyor: ${PREFIX}"
make install

cd ..

# ── Kütüphaneleri proje içine kopyala ─────────────────────────
info "Kütüphaneler kopyalanıyor: ${DEST}"

mkdir -p "${DEST}/lib"
mkdir -p "${DEST}/include"

# libc.a ve libm.a
LIB_SRC="${PREFIX}/${TARGET}/lib"
INC_SRC="${PREFIX}/${TARGET}/include"

cp "${LIB_SRC}/libc.a"           "${DEST}/lib/"
cp "${LIB_SRC}/libm.a"           "${DEST}/lib/"  2>/dev/null || warn "libm.a yok (normal)"
cp "${LIB_SRC}/libnosys.a"       "${DEST}/lib/"  2>/dev/null || true

# Header'lar
cp -r "${INC_SRC}/."             "${DEST}/include/"

# ── Özet ──────────────────────────────────────────────────────
echo ""
echo -e "${GRN}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║   newlib port BAŞARILI                          ║${NC}"
echo -e "${GRN}╠══════════════════════════════════════════════════╣${NC}"
echo -e "${GRN}║${NC}  Kütüphaneler : ${DEST}/lib/"
echo -e "${GRN}║${NC}  Header'lar   : ${DEST}/include/"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Sonraki adım : Makefile güncelle (aşağıda)"
echo -e "${GRN}║${NC}    -I userland/libc/newlib/include"
echo -e "${GRN}║${NC}    -L userland/libc/newlib/lib -lc"
echo -e "${GRN}╚══════════════════════════════════════════════════╝${NC}"