#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  AscentOS — Doom Port Build Script
#  (kilo-build.sh / lua-build.sh yapısına uygun)
#
#  Adımlar:
#   1. doomgeneric kaynağını al (doomgeneric içinde Doom kaynak da gelir)
#   2. doomgeneric_ascent.c port katmanını kopyala
#   3. Doom + doomgeneric + port katmanını derle (doom.elf)
#   4. doom.elf + doom1.wad'ı disk imajına kopyala
#
#  Gereksinimler:
#    - x86_64-elf toolchain
#    - nasm
#    - git
#    - musl-libc 1.2.5 (toolchain/musl-install dizininde kurulu)
#    - doom1.wad (shareware veya tam sürüm)
#    - doom_kernel_patch.c'deki kernel değişiklikleri uygulanmış olmalı
#      (SYS_FB_INFO=407, SYS_KB_RAW=408 syscall'ları)
#
#  Kullanım:
#    chmod +x doom-build.sh && ./doom-build.sh
#    DISK_IMG=disk.img WAD_FILE=/path/to/doom1.wad ./doom-build.sh
# ═══════════════════════════════════════════════════════════════

set -e

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Yapılandırma ──────────────────────────────────────────────
WAD_FILE="${WAD_FILE:-doom1.wad}"                       # shareware veya tam wad
DOOM_REPO="https://github.com/ozkl/doomgeneric.git"
DOOM_SRC_DIR="doomgeneric"

TARGET="x86_64-elf"
SCRIPT_DIR="$(cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"            # projenin kök dizini (bir üst klasör)
DISK_IMG="${DISK_IMG:-${ROOT}/disk.img}"
MUSL_PREFIX="${ROOT}/toolchain/musl-install"
USER_LD="${ROOT}/userland/libc/user.ld"
CRT0_ASM="${ROOT}/userland/libc/crt0.asm"
OUTPUT_DIR="${ROOT}/userland/bin"
SYSCALLS_SRC="${ROOT}/userland/libc/syscalls.c"   # kaynak — her seferinde derlenir
SYSCALLS_OBJ="/tmp/ascentos_doom_syscalls.o"      # geçici obj (eski .o'ya bağımlı değil)
LIBGCC=$(${TARGET}-gcc -m64 --print-libgcc-file-name 2>/dev/null)
GCC_INCLUDE=$(${TARGET}-gcc -m64 -print-file-name=include 2>/dev/null)

# Port katmanı dosyası — projenin kök dizininde bulunmalı
PORT_LAYER="${ROOT}/doomgeneric_ascent.c"

# ── Yardımcı: disk'e dosya yaz ────────────────────────────────
disk_mkdir() {
    local dir="$1"
    if command -v debugfs >/dev/null 2>&1; then
        debugfs -w "${DISK_IMG}" -R "mkdir ${dir}" 2>/dev/null || true
    fi
}

disk_write_file() {
    local src="$1"
    local dst="$2"
    if command -v debugfs >/dev/null 2>&1; then
        # Önce sil (varsa), sonra yaz — yoksa write mevcut inode'u güncellemez
        debugfs -w "${DISK_IMG}" -R "rm ${dst}" 2>/dev/null || true
        debugfs -w "${DISK_IMG}" -R "write ${src} ${dst}" \
            && info "  → disk: ${dst}" \
            || { warn "  → debugfs write başarısız: ${dst}, loop mount deneniyor..."; \
                 local MNT_TMP="/tmp/ascentos_mnt_doom"; \
                 mkdir -p "${MNT_TMP}"; \
                 sudo mount -o loop "${DISK_IMG}" "${MNT_TMP}" || error "mount başarısız"; \
                 sudo mkdir -p "${MNT_TMP}/$(dirname ${dst})"; \
                 sudo cp "${src}" "${MNT_TMP}/${dst}" \
                     && info "  → loop fallback: ${dst}" \
                     || warn "  → loop yazma da başarısız: ${dst}"; \
                 sudo umount "${MNT_TMP}" && rmdir "${MNT_TMP}"; }
    elif command -v e2cp >/dev/null 2>&1; then
        e2cp "${src}" "${DISK_IMG}:/${dst}" \
            && info "  → e2cp: ${dst}" \
            || warn "  → e2cp başarısız: ${dst}"
    else
        local MNT_TMP="/tmp/ascentos_mnt_doom"
        mkdir -p "${MNT_TMP}"
        sudo mount -o loop "${DISK_IMG}" "${MNT_TMP}" || error "mount başarısız"
        sudo mkdir -p "${MNT_TMP}/$(dirname ${dst})"
        sudo cp "${src}" "${MNT_TMP}/${dst}" \
            && info "  → loop: ${dst}" \
            || warn "  → loop yazma başarısız: ${dst}"
        sudo umount "${MNT_TMP}" && rmdir "${MNT_TMP}"
    fi
}

# ── Ön koşul kontrolü ─────────────────────────────────────────
info "Ön koşullar kontrol ediliyor..."
command -v ${TARGET}-gcc >/dev/null 2>&1 || error "${TARGET}-gcc bulunamadı."
command -v nasm          >/dev/null 2>&1 || error "nasm bulunamadı."
command -v git           >/dev/null 2>&1 || error "git bulunamadı."
[ -d "${MUSL_PREFIX}" ] || error "musl kurulum dizini bulunamadı: ${MUSL_PREFIX}"
[ -f "${USER_LD}" ]     || error "Linker script bulunamadı: ${USER_LD}"
[ -f "${CRT0_ASM}" ]    || error "crt0.asm bulunamadı: ${CRT0_ASM}"
[ -f "${SYSCALLS_SRC}" ] || error "syscalls.c bulunamadı: ${SYSCALLS_SRC}"
[ -f "${PORT_LAYER}" ]  || error "doomgeneric_ascent.c bulunamadı: ${PORT_LAYER}"

if [ ! -f "${WAD_FILE}" ]; then
    warn "WAD dosyası bulunamadı: ${WAD_FILE}"
    info "Shareware doom1.wad indiriliyor..."
    WAD_URL="https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad"
    if command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O doom1.wad "${WAD_URL}" \
            && WAD_FILE="$(pwd)/doom1.wad" \
            && info "doom1.wad indirildi." \
            || warn "İndirme başarısız — WAD olmadan devam ediliyor."
    elif command -v curl >/dev/null 2>&1; then
        curl -L --progress-bar -o doom1.wad "${WAD_URL}" \
            && WAD_FILE="$(pwd)/doom1.wad" \
            && info "doom1.wad indirildi." \
            || warn "İndirme başarısız — WAD olmadan devam ediliyor."
    else
        warn "wget veya curl bulunamadı. WAD manuel olarak indirilmeli:"
        warn "  ${WAD_URL}"
    fi
fi

info "Compiler: $(${TARGET}-gcc --version | head -1)"
mkdir -p "${OUTPUT_DIR}"

# ── BÖLÜM 1: doomgeneric kaynağını al ────────────────────────
info "doomgeneric kaynağı alınıyor..."
if [ -d "${DOOM_SRC_DIR}" ]; then
    info "doomgeneric dizini mevcut, güncelleniyor..."
    cd "${DOOM_SRC_DIR}" && git pull --quiet || warn "git pull başarısız, mevcut kaynak kullanılıyor."
    cd ..
else
    git clone --depth=1 "${DOOM_REPO}" "${DOOM_SRC_DIR}" \
        || error "doomgeneric clone başarısız."
fi

# ── BÖLÜM 2: Port katmanını kopyala ──────────────────────────
info "Port katmanı kopyalanıyor..."
cp "${PORT_LAYER}" "${DOOM_SRC_DIR}/doomgeneric/doomgeneric_ascent.c"

# ── BÖLÜM 2b: r_things.c clamp patch'i uygula ───────────────
# R_SortVisSprites, vissprite_p'nin array sınırını aşıp aşmadığını
# kontrol etmiyor. Taşma olursa list walk geçersiz belleğe ulaşır → #GP.
# Bu patch fonksiyon başına bir clamp guard ekler.
info "r_things.c clamp patch'i uygulanıyor..."
RTHINGS="${DOOM_SRC_DIR}/doomgeneric/r_things.c"
PATCH_MARKER="/* ASCENT_VISSPRITE_CLAMP */"

if grep -q "${PATCH_MARKER}" "${RTHINGS}"; then
    info "  r_things.c patch zaten uygulanmış, atlanıyor."
else
    # R_SortVisSprites fonksiyonunun ilk satırından önce clamp guard ekle.
    # "count = vissprite_p - vissprites;" satırından önce yerleştir.
    sed -i "s|    count = vissprite_p - vissprites;|    ${PATCH_MARKER}\n    if (vissprite_p > \&vissprites[MAXVISSPRITES])\n        vissprite_p = \&vissprites[MAXVISSPRITES];\n    count = vissprite_p - vissprites;|" "${RTHINGS}" \
        || error "r_things.c patch başarısız."
    info "  r_things.c clamp patch'i uygulandı (MAXVISSPRITES=512 guard)."
fi
info "crt0.o derleniyor..."
CRT0_OBJ="/tmp/ascentos_doom_crt0.o"
nasm -f elf64 "${CRT0_ASM}" -o "${CRT0_OBJ}" \
    || error "crt0.asm derleme başarısız."

# ── BÖLÜM 3b: syscalls.o üret (SYS_FB_BLIT dahil güncel sürüm) ──
info "syscalls.o derleniyor..."
${TARGET}-gcc \
    -m64 \
    -fno-stack-protector \
    -O2 \
    -std=gnu99 \
    -ffreestanding \
    -I${MUSL_PREFIX}/include \
    -isystem ${GCC_INCLUDE} \
    --sysroot=${MUSL_PREFIX} \
    -Wno-implicit-function-declaration \
    -c "${SYSCALLS_SRC}" -o "${SYSCALLS_OBJ}" \
    || error "syscalls.c derleme başarısız."
info "syscalls.o hazır: ${SYSCALLS_OBJ}"

# ── BÖLÜM 4: Doom kaynak listesini derle ──────────────────────
#
# doomgeneric/doomgeneric/ altındaki TÜM .c dosyaları derlenir.
# doomgeneric_example.c hariç (referans implementasyon — bizimkiyle çakışır).
# ─────────────────────────────────────────────────────────────
DOOM_SRC_SUBDIR="${DOOM_SRC_DIR}/doomgeneric"

info "Doom kaynak dosyaları listeleniyor..."
# Harici bağımlılık gerektiren dosyalar hariç tutulur:
#   doomgeneric_*.c      → diğer platform port dosyaları (sdl, xlib, allegro, win…)
#   i_allegromusic.c     → Allegro müzik (allegro/base.h gerektirir)
#   i_allegrosound.c     → Allegro ses   (allegro.h gerektirir)
#   i_sdlmusic.c         → SDL2 müzik    (SDL_mixer.h gerektirir)
#   i_sdlsound.c         → SDL2 ses      (SDL_mixer.h gerektirir)
#   i_cdmus.c            → CD audio      (platform bağımlı)
#   statdump.c           → kendi main() içerir, binary çakışır
#
# i_system.c, i_video.c, i_input.c, i_joystick.c DAHİL edilir.
# i_sound.c FEATURE_SOUND tanımlı değilse SDL bağımlılığı olmadan derlenir.
DOOM_SOURCES=$(find "${DOOM_SRC_SUBDIR}" -name "*.c" \
    ! -name "doomgeneric_*.c" \
    ! -name "i_allegromusic.c" \
    ! -name "i_allegrosound.c" \
    ! -name "i_sdlmusic.c" \
    ! -name "i_sdlsound.c" \
    ! -name "i_cdmus.c" \
    ! -name "i_sound.c" \
    ! -name "i_music.c" \
    ! -name "statdump.c" \
    | sort)
# Bizim port katmanımızı ekle
DOOM_SOURCES="${DOOM_SOURCES} ${DOOM_SRC_SUBDIR}/doomgeneric_ascent.c"

DOOM_OBJS_DIR="/tmp/ascentos_doom_objs"
mkdir -p "${DOOM_OBJS_DIR}"

COMMON_CFLAGS="\
    -m64 \
    -fno-stack-protector \
    -O2 \
    -std=gnu99 \
    -D_GNU_SOURCE \
    -DNORMALUNIX -DLINUX \
    -DFEATURE_SOUND \
    -UFEATURE_MULTIPLAYER \
    -DPACKAGEVERSION='\"AscentOS-Doom\"' \
    -I${DOOM_SRC_SUBDIR} \
    -I${MUSL_PREFIX}/include \
    -isystem ${GCC_INCLUDE} \
    --sysroot=${MUSL_PREFIX}"

COMMON_CFLAGS="${COMMON_CFLAGS} -Wno-implicit-function-declaration -Wno-int-conversion -Wno-unused-result"

# R_SortVisSprites linked list corruption fix:
# Varsayılan MAXVISSPRITES=128 yoğun sahnelerde taşıyor →
# vissprite_t list walk'u geçersiz pointer'a ulaşıyor → #GP.
# 512'ye çıkarmak + r_things.c clamp patch'i birlikte uygulanır.
COMMON_CFLAGS="${COMMON_CFLAGS} -DMAXVISSPRITES=512"

DOOM_OBJ_LIST=""

info "Doom dosyaları derleniyor (bu biraz sürer)..."
for src in ${DOOM_SOURCES}; do
    base=$(basename "${src}" .c)
    obj="${DOOM_OBJS_DIR}/${base}.o"
    ${TARGET}-gcc ${COMMON_CFLAGS} -c "${src}" -o "${obj}" \
        || error "Derleme başarısız: ${src}"
    DOOM_OBJ_LIST="${DOOM_OBJ_LIST} ${obj}"
    echo -n "."
done
echo ""
info "Tüm Doom dosyaları derlendi."

# ── BÖLÜM 5: Link ─────────────────────────────────────────────
info "doom.elf linkleniyor..."

DOOM_ELF="${OUTPUT_DIR}/doom.elf"

${TARGET}-gcc \
    -m64 \
    -nostdlib \
    -static \
    -T "${USER_LD}" \
    "${CRT0_OBJ}" \
    ${DOOM_OBJ_LIST} \
    "${SYSCALLS_OBJ}" \
    "${MUSL_PREFIX}/lib/libc.a" \
    "${LIBGCC}" \
    -Wl,-z,muldefs \
    -Wl,--no-warn-execstack \
    -o "${DOOM_ELF}" \
    || error "Link başarısız."

info "doom.elf oluşturuldu: ${DOOM_ELF} ($(du -sh ${DOOM_ELF} | cut -f1))"

# ── BÖLÜM 6: Disk imajına kopyala ─────────────────────────────
if [ -f "${DISK_IMG}" ]; then
    info "doom.elf disk imajına yazılıyor..."
    info "  Kaynak: ${DOOM_ELF} ($(du -sh ${DOOM_ELF} | cut -f1))"
    disk_mkdir "bin"
    disk_write_file "${DOOM_ELF}" "bin/doom.elf"

    # Yazmanın başarılı olduğunu doğrula
    info "Yazma doğrulanıyor..."
    if command -v debugfs >/dev/null 2>&1; then
        DISK_SIZE=$(debugfs -R "stat bin/doom.elf" "${DISK_IMG}" 2>/dev/null | grep "Size:" | head -1)
        info "  Diskteki doom.elf: ${DISK_SIZE}"
    fi

    if [ -f "${WAD_FILE}" ]; then
        info "WAD dosyası disk imajına yazılıyor..."
        disk_write_file "${WAD_FILE}" "doom1.wad"
        info "Çalıştırmak için: doom.elf -iwad /doom1.wad"
    else
        warn "WAD dosyası bulunamadı — doom.elf yazıldı ama çalıştırılamaz."
        warn "WAD'ı sonradan eklemek için:"
        warn "  wget -O doom1.wad https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad"
        warn "  Sonra bu script'i tekrar çalıştır."
    fi
else
    warn "Disk imajı bulunamadı (${DISK_IMG}) — binary yerel dizinde bırakıldı:"
    warn "  ${DOOM_ELF}"
    warn "Manuel olarak diske kopyalayın:"
    warn "  sudo mount -o loop disk.img /mnt && sudo cp ${DOOM_ELF} /mnt/bin/doom.elf && sudo umount /mnt"
fi

# ── BÖLÜM 7: Özet ─────────────────────────────────────────────
echo ""
echo -e "${GRN}╔════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║        AscentOS Doom Port — HAZIR          ║${NC}"
echo -e "${GRN}╚════════════════════════════════════════════╝${NC}"
echo ""
info "Çalıştırma:"
echo "  AscentOS shellde:  doom.elf -iwad /doom1.wad"
echo ""
info "Kontroller:"
echo "  WASD           → hareket"
echo "  E veya Ctrl    → ateş"
echo "  Space          → kapı/kullan"
echo "  ESC            → menü"
echo ""
info "Eğer ekran boyutu 1280x800 veya üstüyse Doom 3x scale (960x600) olarak çalışır."
info "640x480 için 2x scale, daha düşük için 1x scale."
echo ""

# ── Temizlik ──────────────────────────────────────────────────
rm -rf "${DOOM_OBJS_DIR}" "${CRT0_OBJ}" "${SYSCALLS_OBJ}"
info "Geçici dosyalar temizlendi."