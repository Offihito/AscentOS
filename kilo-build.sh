#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  AscentOS — Kilo Text Editor Port Build Script
#  Kullanım: chmod +x kilo-build.sh && ./kilo-build.sh
#
#  lua-build.sh / tcc-build.sh scriptlerinden türetilmiştir.
#  musl-libc 1.2.5 + x86_64-elf toolchain gerektirir.
#
#  Kilo hakkında notlar:
#   • Kilo, antirez tarafından yazılmış ~1000 satırlık tek dosyalık
#     terminal metin editörüdür. (https://github.com/antirez/kilo)
#   • VT100/ANSI escape dizileri kullanır; termios ile raw mod açar.
#   • AscentOS'ta ioctl(TIOCGWINSZ) ve termios syscall'larına ihtiyaç
#     duyar — syscalls.c'de SYS_IOCTL mevcut, yeterli.
#   • tcgetattr / tcsetattr → ioctl veya musl üzerinden çalışır.
#   • Tek C dosyası: kilo.c  →  kilo.elf
# ═══════════════════════════════════════════════════════════════

set -e

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Diske kopyalama ayarları ───────────────────────────────────
DISK_IMG="${DISK_IMG:-disk.img}"

# ── Kilo sürümü ───────────────────────────────────────────────
KILO_REPO="https://github.com/antirez/kilo.git"
KILO_SRC_DIR="kilo"

# ── Yollar ────────────────────────────────────────────────────
TARGET="x86_64-elf"
MUSL_PREFIX="$(pwd)/toolchain/musl-install"
USER_LD="$(pwd)/userland/libc/user.ld"
CRT0_ASM="$(pwd)/userland/libc/crt0.asm"
OUTPUT_DIR="$(pwd)/userland/bin"
SYSCALLS_OBJ="$(pwd)/userland/out/syscalls.o"
LIBGCC=$(${TARGET}-gcc -m64 --print-libgcc-file-name 2>/dev/null)
GCC_INCLUDE=$(${TARGET}-gcc -m64 -print-file-name=include 2>/dev/null)

# ── Ön koşul kontrolü ─────────────────────────────────────────
info "Ön koşullar kontrol ediliyor..."
command -v ${TARGET}-gcc >/dev/null 2>&1 || error "${TARGET}-gcc bulunamadı. Önce toolchain kurulumunu yapın."
command -v nasm          >/dev/null 2>&1 || error "nasm bulunamadı (sudo apt install nasm)."
command -v git           >/dev/null 2>&1 || error "git bulunamadı (sudo apt install git)."
[ -d "${MUSL_PREFIX}" ] || error "musl kurulum dizini bulunamadı: ${MUSL_PREFIX}"
[ -f "${USER_LD}" ]     || error "Linker script (user.ld) bulunamadı: ${USER_LD}"
[ -f "${CRT0_ASM}" ]    || error "crt0.asm bulunamadı: ${CRT0_ASM}"
[ -f "${SYSCALLS_OBJ}" ] || error "syscalls.o bulunamadı: ${SYSCALLS_OBJ} — önce 'make userland/out/syscalls.o' çalıştırın."

info "Compiler: $(${TARGET}-gcc --version | head -1)"

# ── Kilo kaynağını al / güncelle ──────────────────────────────
if [ -d "${KILO_SRC_DIR}" ]; then
    info "Kilo dizini zaten mevcut, güncelleniyor..."
    cd "${KILO_SRC_DIR}" && git pull || warn "git pull başarısız, mevcut kaynak kullanılacak."
    cd ..
else
    info "Kilo klonlanıyor: ${KILO_REPO}"
    git clone --depth=1 "${KILO_REPO}" "${KILO_SRC_DIR}"
fi

[ -f "${KILO_SRC_DIR}/kilo.c" ] || error "kilo.c bulunamadı! Klonlama başarısız olmuş olabilir."

# ── Yama öncesi temizlik ──────────────────────────────────────
cd "${KILO_SRC_DIR}"
OLDEST_BAK=$(ls kilo.c.bak.* 2>/dev/null | sort | head -1)
if [ -n "${OLDEST_BAK}" ]; then
    info "kilo.c orijinal yedekten geri yükleniyor: ${OLDEST_BAK}"
    cp "${OLDEST_BAK}" kilo.c
    rm -f kilo.c.bak.*
fi
cd ..

# ── Kilo kaynağına yamaları uygula ────────────────────────────
#
#  Kilo AscentOS'ta derlenmek için şu yamalar gereklidir:
#
#  1. <termios.h> → musl üzerinden gelir, genellikle sorunsuz.
#     AscentOS'ta tcgetattr/tcsetattr ioctl tabanlı çalışır.
#
#  2. ioctl(TIOCGWINSZ) → SYS_IOCTL mevcut, doğrudan çalışır.
#     Eğer TIOCGWINSZ döndürülemezse fallback: 80x24 kullan.
#
#  3. getWindowSize() → ioctl başarısız olursa ANSI escape
#     "ESC[999C ESC[999B ESC[6n" ile boyut sorgular.
#     Bu yöntem AscentOS terminal sürücüsüne bağlıdır; yoksa
#     80x24 sabit değer kullanılır.
#
#  4. <sys/ioctl.h> / <termios.h> guard'ları: musl include'da
#     mevcut, ek yama gerekmez.
#
#  5. getline() → musl'de var, sorun çıkmaz.
#
#  6. ASCENTOS_KILO bloğu: fallback pencere boyutu + ioctl guard.
# ──────────────────────────────────────────────────────────────
info "Kilo'ya AscentOS yamaları uygulanıyor..."

cd "${KILO_SRC_DIR}"
cp kilo.c kilo.c.bak.$(date +%s)

# AscentOS platform bloğunu kilo.c'nin başına ekle
if ! grep -q "ASCENTOS_KILO" kilo.c; then
    cat > /tmp/kilo_patch.h << 'KILO_PATCH'
/* ── AscentOS Kilo platform tanımları ─────────────────────────
 * kilo-build.sh tarafından otomatik eklendi.
 *
 * AscentOS'ta terminal sürücüsü temel VT100 escape dizilerini
 * destekler. TIOCGWINSZ ioctl genellikle çalışır; çalışmazsa
 * 80x24 sabit boyut kullanılır.
 *
 * _POSIX_C_SOURCE / _DEFAULT_SOURCE: musl ile uyumluluk için.
 * ─────────────────────────────────────────────────────────── */
#define ASCENTOS_KILO           1
#define _DEFAULT_SOURCE         1
#define _BSD_SOURCE             1
#define _GNU_SOURCE             1

/* TIOCGWINSZ musl sys/ioctl.h'da tanımlı; yine de guard ekle */
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#endif

/* winsize yapısı: musl'de <sys/ioctl.h> içinde zaten mevcut.
 * Eğer musl include yolu doğruysa bu blok devreye girmez.    */
#ifndef __DEFINED_struct_winsize
#define __DEFINED_struct_winsize
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#endif

/* Fallback pencere boyutu: TIOCGWINSZ başarısız olursa kullan */
#define ASCENTOS_FALLBACK_ROWS  24
#define ASCENTOS_FALLBACK_COLS  80
/* ─────────────────────────────────────────────────────────── */

KILO_PATCH

    cat /tmp/kilo_patch.h kilo.c > /tmp/kilo_patched.c
    mv /tmp/kilo_patched.c kilo.c
    rm -f /tmp/kilo_patch.h
    info "kilo.c: AscentOS platform bloğu başa eklendi."
fi

# getWindowSize() içinde TIOCGWINSZ başarısız olduğunda
# ANSI fallback yerine sabit boyut döndür.
# Orijinal kod: ws.ws_col == 0 ise getCursorPosition() çağırır.
# AscentOS'ta ESC[6n bazen yanıt vermeyebilir → sonsuz döngü.
# Yama: getCursorPosition başarısız olursa -1 değil 80x24 dön.
if grep -q "getCursorPosition" kilo.c; then
    # getCursorPosition() çağrısını wrap et: hata durumunda fallback
    # Basit yöntem: return -1 satırlarını fallback ile değiştir.
    # Orijinal getWindowSize sonu:
    #   if (getCursorPosition(rows, cols) == -1) return -1;
    # Yama:
    #   if (getCursorPosition(rows, cols) == -1) {
    #       *rows = ASCENTOS_FALLBACK_ROWS;
    #       *cols = ASCENTOS_FALLBACK_COLS;
    #       return 0;
    #   }
    sed -i 's/if (getCursorPosition(rows, cols) == -1) return -1;/if (getCursorPosition(rows, cols) == -1) { *rows = ASCENTOS_FALLBACK_ROWS; *cols = ASCENTOS_FALLBACK_COLS; return 0; }/g' kilo.c
    info "kilo.c: getWindowSize() fallback yaması uygulandı."
fi

# ── atexit() notu ────────────────────────────────────────────
# atexit artık syscalls.c içinde implemente edildi.
# kilo.c'ye yama gerekmez — orijinal atexit(editorAtExit) çalışır.
info "kilo.c: atexit syscalls.c'de implemente edildi, yama gerekmez."

cd ..

# ── crt0.o üret ───────────────────────────────────────────────
info "crt0.o üretiliyor..."
mkdir -p "$(pwd)/userland/out"
nasm -f elf64 -o "$(pwd)/userland/out/crt0.o" "${CRT0_ASM}"
info "crt0.o üretildi."

# ── Kilo'yu derle ─────────────────────────────────────────────
info "Kilo derleniyor: kilo.c → kilo.elf"
mkdir -p "${OUTPUT_DIR}"

${TARGET}-gcc \
    -std=c99 \
    -O2 \
    -ffreestanding \
    -fno-stack-protector \
    -mno-red-zone \
    -mcmodel=small \
    -nostdinc \
    -I"${MUSL_PREFIX}/include" \
    -isystem "${GCC_INCLUDE}" \
    -c \
    -o /tmp/kilo.o \
    "${KILO_SRC_DIR}/kilo.c" \
    2>&1 | tee /tmp/kilo_compile.log
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    warn "Derleme hatası. /tmp/kilo_compile.log:"
    cat /tmp/kilo_compile.log
    error "kilo.c derlenemedi. Yukarıdaki hataları inceleyin."
fi

info "kilo.o derlendi."

# ── Kilo'yu linkle ────────────────────────────────────────────
#
#  -z muldefs           : syscalls.o ve musl'deki __errno_location
#                         çakışmasını bastırır (syscalls.o kazanır,
#                         link sırasında önce geldiği için).
#  --start-group / --end-group : atexit gibi dairesel referansları
#                         çözer; musl ve libgcc birden fazla geçilir.
# ──────────────────────────────────────────────────────────────
info "Kilo linkleniyor: kilo.o → kilo.elf"

${TARGET}-gcc \
    -nostdlib \
    -static \
    -T "${USER_LD}" \
    -Wl,-z,muldefs \
    -o "${OUTPUT_DIR}/kilo.elf" \
    "$(pwd)/userland/out/crt0.o" \
    "${SYSCALLS_OBJ}" \
    /tmp/kilo.o \
    -L"${MUSL_PREFIX}/lib" \
    -Wl,--start-group \
    -lc \
    "${LIBGCC}" \
    -Wl,--end-group \
    2>&1 | tee /tmp/kilo_link.log
# pipe ile || çalışmaz; PIPESTATUS kullan
if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    warn "Linkleme hatası. /tmp/kilo_link.log:"
    cat /tmp/kilo_link.log
    error "kilo.elf linklenemedi."
fi

info "kilo.elf linklendi."

# ── Strip ─────────────────────────────────────────────────────
${TARGET}-strip "${OUTPUT_DIR}/kilo.elf" 2>/dev/null \
    && info "kilo.elf strip edildi." \
    || warn "strip başarısız (opsiyonel, devam ediliyor)."

SIZE=$(du -sh "${OUTPUT_DIR}/kilo.elf" 2>/dev/null | cut -f1)
info "kilo.elf boyutu: ${SIZE}"

# ── Diske kopyala ─────────────────────────────────────────────
disk_write_file() {
    local SRC="$1"
    local DST="$2"
    if command -v debugfs >/dev/null 2>&1; then
        debugfs -w "${DISK_IMG}" -R "write ${SRC} ${DST}" 2>/dev/null \
            && return 0
    fi
    if command -v e2cp >/dev/null 2>&1; then
        e2cp "${SRC}" "${DISK_IMG}:/${DST}" 2>/dev/null \
            && return 0
    fi
    # loop mount fallback
    local MNT_TMP="/tmp/ascentos_mnt_kilo"
    mkdir -p "${MNT_TMP}"
    sudo mount -o loop "${DISK_IMG}" "${MNT_TMP}" \
        && sudo cp "${SRC}" "${MNT_TMP}/${DST}" \
        && sudo umount "${MNT_TMP}" \
        && rmdir "${MNT_TMP}" \
        && return 0
    sudo umount "${MNT_TMP}" 2>/dev/null
    rmdir "${MNT_TMP}" 2>/dev/null
    return 1
}

if [ -f "${DISK_IMG}" ]; then
    info "Diske kopyalanıyor: kilo.elf → ${DISK_IMG}:/bin/kilo.elf"
    # /bin dizininin varlığını garantile
    debugfs -w "${DISK_IMG}" -R "mkdir bin" 2>/dev/null || true
    debugfs -w "${DISK_IMG}" -R "rm bin/kilo.elf" 2>/dev/null || true

    if disk_write_file "${OUTPUT_DIR}/kilo.elf" "bin/kilo.elf"; then
        info "Kopyalama başarılı: ${DISK_IMG}:/bin/kilo.elf"
    else
        warn "Diske kopyalama başarısız! Binary manuel olarak kopyalanmalı."
        warn "  → cp ${OUTPUT_DIR}/kilo.elf <mount_noktası>/bin/kilo.elf"
    fi
else
    warn "Disk imajı bulunamadı: ${DISK_IMG}. Kopyalama atlandı."
    warn "  → Önce 'make disk.img' ile imajı oluşturun."
fi

# ── Özet ──────────────────────────────────────────────────────
echo ""
echo -e "${GRN}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║   Kilo Text Editor AscentOS port BAŞARILI            ║${NC}"
echo -e "${GRN}╠══════════════════════════════════════════════════════╣${NC}"
echo -e "${GRN}║${NC}  Çıktı          : ${OUTPUT_DIR}/kilo.elf"
echo -e "${GRN}║${NC}  Boyut          : ${SIZE}"
echo -e "${GRN}║${NC}  Kaynak         : ${KILO_REPO}"
if [ -f "${DISK_IMG}" ]; then
    echo -e "${GRN}║${NC}  Diske kopyalandı: ${DISK_IMG}:/bin/kilo.elf (Ext2)"
fi
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Kullanım:"
echo -e "${GRN}║${NC}    kilo.elf                  (boş dosya aç)"
echo -e "${GRN}║${NC}    kilo.elf dosya.txt         (mevcut dosyayı düzenle)"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Tuş kısayolları:"
echo -e "${GRN}║${NC}    Ctrl-S   → Kaydet"
echo -e "${GRN}║${NC}    Ctrl-Q   → Çık (3x değişiklik varsa)"
echo -e "${GRN}║${NC}    Ctrl-F   → Bul (Esc ile iptal)"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Bilinen kısıtlamalar:"
echo -e "${GRN}║${NC}  • TIOCGWINSZ başarısız → 80x24 fallback kullanılır"
echo -e "${GRN}║${NC}  • Mouse desteği yok (sadece klavye)"
echo -e "${GRN}║${NC}  • Syntax highlighting kilo'nun orijinal haliyle"
echo -e "${GRN}║${NC}    gelir (C/Python/...) — renk desteği terminale bağlı"
echo -e "${GRN}║${NC}  • Çok büyük dosyalarda (>1MB) performans düşebilir"
echo -e "${GRN}╚══════════════════════════════════════════════════════╝${NC}"