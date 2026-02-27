#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
#  AscentOS — kilo Text Editor Port Build Script  v1.0
#  Kullanım: chmod +x kilo-build.sh && ./kilo-build.sh
#
#  Ne yapar?
#    1. antirez/kilo kaynak kodunu GitHub'dan indirir
#    2. AscentOS uyumlu kilo_ascentos.c patch'ini uygular
#       (termios → TCGETS/TCSETS ioctl, TIOCGWINSZ, SIGWINCH)
#    3. x86_64-elf-gcc + newlib 4.4 + syscalls.c ile derler
#    4. kilo.elf → userland/out/ altına koyar
#    5. disk.img'e yazar (mcopy)
#
#  Gereksinimler:
#    - x86_64-elf-gcc (cross compiler)
#    - newlib zaten derlenmiş: userland/libc/newlib/{lib,include}
#    - userland/libc/crt0.o ve userland/out/syscalls.o mevcut
#    - curl veya wget
#    - mcopy (mtools) — disk.img'e yazmak için
# ═══════════════════════════════════════════════════════════════════════════

set -e

# ── Renkli çıktı ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
step()  { echo -e "${CYN}[STEP]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Ayarlar ───────────────────────────────────────────────────────────────
KILO_URL="https://raw.githubusercontent.com/antirez/kilo/master/kilo.c"
KILO_ORIG="kilo_orig.c"
KILO_SRC="kilo_ascentos.c"          # AscentOS patch'li kaynak (bu klasörde)
KILO_OUT="userland/out/kilo.elf"

TARGET="x86_64-elf"
USERLAND_CC="${TARGET}-gcc"
USERLAND_LD="${TARGET}-ld"

NEWLIB_INC="userland/libc/newlib/include"
NEWLIB_LIB="userland/libc/newlib/lib"
GCC_INCLUDE="$(${USERLAND_CC} -m64 --print-file-name=include 2>/dev/null || true)"
LIBGCC="$(${USERLAND_CC} -m64 --print-libgcc-file-name 2>/dev/null || true)"

CRT0="userland/libc/crt0.o"
SYSCALLS="userland/out/syscalls.o"

LINKER_SCRIPT="userland/libc/user.ld"

USERLAND_CFLAGS="-m64 -ffreestanding -fno-stack-protector \
    -ffunction-sections -fdata-sections -O2 -Wall -Wextra \
    -I${NEWLIB_INC} \
    -isystem ${GCC_INCLUDE} \
    -D_POSIX_C_SOURCE=200809L \
    -D_XOPEN_SOURCE=700"

USERLAND_LDFLAGS="-n -T ${LINKER_SCRIPT} \
    -static -nostdlib --gc-sections"

# ── Ön koşul kontrolü ─────────────────────────────────────────────────────
step "Ön koşullar kontrol ediliyor..."

command -v "${USERLAND_CC}" >/dev/null 2>&1 || \
    error "${USERLAND_CC} bulunamadı. x86_64-elf cross-compiler PATH'te olmalı."

command -v "${USERLAND_LD}" >/dev/null 2>&1 || \
    error "${USERLAND_LD} bulunamadı."

[ -d "${NEWLIB_INC}" ] || \
    error "newlib include dizini yok: ${NEWLIB_INC}\n       Önce newlib-build.sh'ı çalıştırın."

[ -f "${NEWLIB_LIB}/libc.a" ] || \
    error "libc.a yok: ${NEWLIB_LIB}/libc.a\n       Önce newlib-build.sh'ı çalıştırın."

[ -f "${CRT0}" ] || \
    error "crt0.o yok: ${CRT0}\n       'make userland/libc/crt0.o' çalıştırın."

[ -f "${SYSCALLS}" ] || \
    error "syscalls.o yok: ${SYSCALLS}\n       'make userland/out/syscalls.o' çalıştırın."

[ -f "${LINKER_SCRIPT}" ] || \
    error "Linker script yok: ${LINKER_SCRIPT}"

GCC_VER=$(${USERLAND_CC} --version | head -1)
info "Compiler : ${GCC_VER}"
info "newlib   : ${NEWLIB_INC}"
info "libgcc   : ${LIBGCC}"

# ── Kaynak: ascentos patch'li kilo ────────────────────────────────────────
# kilo_ascentos.c bu script ile aynı klasörde olmalı.
# Script kilo_ascentos.c'yi userland/apps/ altına kopyalar.
step "Kaynak hazırlanıyor..."

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KILO_SRC_PATH="${SCRIPT_DIR}/${KILO_SRC}"

if [ ! -f "${KILO_SRC_PATH}" ]; then
    error "AscentOS kaynak dosyası bulunamadı: ${KILO_SRC_PATH}\n       kilo_ascentos.c bu script ile aynı klasörde olmalı."
fi

# Orijinal kilo'yu referans olarak indir (isteğe bağlı, diff için)
if [ ! -f "${KILO_ORIG}" ]; then
    info "Orijinal kilo.c indiriliyor (diff referansı)..."
    if command -v curl >/dev/null 2>&1; then
        curl -L "${KILO_URL}" -o "${KILO_ORIG}" 2>/dev/null || \
            warn "Orijinal kilo.c indirilemedi (ağ kapalı olabilir) — atlanıyor."
    elif command -v wget >/dev/null 2>&1; then
        wget -q "${KILO_URL}" -O "${KILO_ORIG}" 2>/dev/null || \
            warn "Orijinal kilo.c indirilemedi — atlanıyor."
    else
        warn "curl/wget bulunamadı — orijinal kilo.c atlanıyor."
    fi
fi

mkdir -p userland/apps
cp "${KILO_SRC_PATH}" userland/apps/kilo.c
info "Kaynak kopyalandı: userland/apps/kilo.c"

# ── Derleme ───────────────────────────────────────────────────────────────
step "Derleniyor: kilo.c → kilo.o ..."
mkdir -p userland/out

${USERLAND_CC} ${USERLAND_CFLAGS} \
    -c userland/apps/kilo.c \
    -o userland/out/kilo.o

info "Derleme başarılı: userland/out/kilo.o ($(du -sh userland/out/kilo.o | cut -f1))"

# ── Linkleme ──────────────────────────────────────────────────────────────
step "Linkleniyor: kilo.elf ..."

${USERLAND_LD} ${USERLAND_LDFLAGS} -m elf_x86_64 \
    "${CRT0}"              \
    userland/out/kilo.o    \
    "${SYSCALLS}"          \
    -L"${NEWLIB_LIB}" -lc  \
    ${LIBGCC}              \
    -o "${KILO_OUT}"

ELF_SIZE=$(du -sh "${KILO_OUT}" | cut -f1)
info "Linkleme başarılı: ${KILO_OUT} (${ELF_SIZE})"

# ELF başlığını doğrula
if command -v "${TARGET}-readelf" >/dev/null 2>&1; then
    ENTRY=$(${TARGET}-readelf -h "${KILO_OUT}" 2>/dev/null | \
            grep "Entry point" | awk '{print $4}')
    info "ELF giriş noktası: ${ENTRY}"
fi

# ── disk.img'e yaz ────────────────────────────────────────────────────────
step "disk.img'e yazılıyor..."

if [ ! -f disk.img ]; then
    warn "disk.img bulunamadı — disk'e yazma atlandı."
    warn "Kurulum için: mcopy -i disk.img@@1048576 -o ${KILO_OUT} ::KILO.ELF"
else
    if command -v mcopy >/dev/null 2>&1; then
        mcopy -i disk.img@@1048576 -o "${KILO_OUT}" ::KILO.ELF
        info "disk.img'e yazıldı: KILO.ELF"
        if command -v mdir >/dev/null 2>&1; then
            echo ""
            mdir -i disk.img@@1048576 :: 2>/dev/null | grep -i "\.elf" || true
        fi
    else
        warn "mcopy bulunamadı — disk'e yazma atlandı (mtools kurun)."
        warn "Elle kurulum: mcopy -i disk.img@@1048576 -o ${KILO_OUT} ::KILO.ELF"
    fi
fi

# ── Makefile güncelleme rehberi ────────────────────────────────────────────
echo ""
echo -e "${GRN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║   kilo AscentOS Port BAŞARILI                           ║${NC}"
echo -e "${GRN}╠══════════════════════════════════════════════════════════╣${NC}"
echo -e "${GRN}║${NC}  ELF          : ${KILO_OUT}"
echo -e "${GRN}║${NC}  Boyut        : ${ELF_SIZE}"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  AscentOS'ta kullanım:"
echo -e "${GRN}║${NC}    > kilo dosya.c        (dosya aç/oluştur)"
echo -e "${GRN}║${NC}    > kilo                (boş editör)"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Tuş kombinasyonları:"
echo -e "${GRN}║${NC}    CTRL-S  → Kaydet"
echo -e "${GRN}║${NC}    CTRL-Q  → Çık (değişiklik varsa 3 kez)"
echo -e "${GRN}║${NC}    CTRL-F  → Bul / Ara"
echo -e "${GRN}║${NC}    Oklar   → İmleç hareketi"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Makefile'a eklemek için:"
echo -e "${GRN}║${NC}    USERLAND_APPS := hello calculator kilo"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Veya: make kilo  (aşağıdaki hedefe bakın)"
echo -e "${GRN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YLW}Makefile'a eklenecek hedef:${NC}"
echo "──────────────────────────────────────────────────────────────"
cat << 'MAKEFILE_SNIPPET'

# kilo text editor
.PHONY: kilo
kilo: userland/out/kilo.elf
	@echo "✓ kilo hazır → userland/out/kilo.elf"

userland/out/kilo.elf: userland/apps/kilo.c $(USERLAND_CRT0) $(SYSCALLS_OBJ) | $(NEWLIB_STAMP)
	$(USERLAND_CC) $(USERLAND_CFLAGS) -c -o userland/out/kilo.o $<
	$(USERLAND_LD) $(USERLAND_LDFLAGS) -m elf_x86_64 \
	    $(USERLAND_CRT0) userland/out/kilo.o $(SYSCALLS_OBJ) \
	    -L$(NEWLIB_LIB) -lc $(LIBGCC) -o $@
	@echo "  ✓ $@ hazır"

install-kilo: userland/out/kilo.elf
	mcopy -i disk.img@@1048576 -o $< ::KILO.ELF
	@echo "✓ KILO.ELF disk.img'e yazıldı"

MAKEFILE_SNIPPET
echo "──────────────────────────────────────────────────────────────"