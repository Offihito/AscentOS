#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  AscentOS — kilo Text Editor Port Build Script (diske kopyalamalı)
#  Kullanım: chmod +x kilo-build.sh && ./kilo-build.sh
# ═══════════════════════════════════════════════════════════════

set -e

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Diske kopyalama ayarları ───────────────────────────────────
DISK_IMG="${DISK_IMG:-disk.img}"          # Disk imajı dosyası (varsayılan: disk.img)
DISK_OFFSET="${DISK_OFFSET:-1048576}"     # FAT32 bölümünün offset'i (bayt) (varsayılan: 1048576)

TARGET="x86_64-elf"
MUSL_PREFIX="$(pwd)/toolchain/musl-install"
USER_LD="$(pwd)/userland/libc/user.ld"
CRT0_ASM="$(pwd)/userland/libc/crt0.asm"
KILO_SRC_DIR="kilo"
KILO_REPO="https://github.com/antirez/kilo.git"
OUTPUT_DIR="$(pwd)/userland/bin"
SYSCALLS_OBJ="$(pwd)/userland/out/syscalls.o"
LIBGCC=$(${TARGET}-gcc -m64 --print-libgcc-file-name 2>/dev/null)
GCC_INCLUDE=$(${TARGET}-gcc -m64 -print-file-name=include 2>/dev/null)

# ── Ön koşul kontrolü ─────────────────────────────────────────
info "Ön koşullar kontrol ediliyor..."
command -v ${TARGET}-gcc >/dev/null 2>&1 || error "${TARGET}-gcc bulunamadı. Önce musl-build.sh çalıştırın."
command -v nasm >/dev/null 2>&1 || error "nasm bulunamadı (sudo apt install nasm)."
[ -d "${MUSL_PREFIX}" ] || error "musl kurulum dizini bulunamadı: ${MUSL_PREFIX}"
[ -f "${USER_LD}" ]     || error "Linker script (user.ld) bulunamadı: ${USER_LD}"
[ -f "${CRT0_ASM}" ]    || error "crt0.asm bulunamadı: ${CRT0_ASM}"
[ -f "${SYSCALLS_OBJ}" ] || error "syscalls.o bulunamadı: ${SYSCALLS_OBJ} — önce 'make userland/out/syscalls.o' çalıştırın."

# libc'de atexit varlığını kontrol et
if ${TARGET}-nm "${MUSL_PREFIX}/lib/libc.a" 2>/dev/null | grep -q "atexit"; then
    info "libc.a içinde atexit sembolü bulundu."
else
    warn "libc.a içinde atexit sembolü bulunamadı! (musl-build.sh düzgün çalışmamış olabilir)"
fi

info "Compiler: $(${TARGET}-gcc --version | head -1)"

# ── Kilo kaynağını al / güncelle ──────────────────────────────
if [ -d "${KILO_SRC_DIR}" ]; then
    info "kilo dizini zaten mevcut, güncelleniyor..."
    cd "${KILO_SRC_DIR}" && git pull || warn "git pull başarısız, mevcut kaynak kullanılacak."
    cd ..
else
    info "kilo klonlanıyor: ${KILO_REPO}"
    git clone "${KILO_REPO}" "${KILO_SRC_DIR}"
fi

# ── kilo.c'ye yamaları uygula (kesin yöntem) ───────────────────
cd "${KILO_SRC_DIR}"
if [ -f kilo.c ]; then
    # Yedek al
    cp kilo.c kilo.c.bak.$(date +%s)
    info "kilo.c'ye yamalar uygulanıyor..."

    # 1. atexit(editorAtExit); satırını bul ve yorum satırına çevir
    sed -i 's/^\([[:space:]]*\)atexit(editorAtExit);/\1\/\/ atexit(editorAtExit);/' kilo.c

    # 2. Tüm disableRawMode() çağrılarını disableRawMode(0) ile değiştir
    sed -i 's/disableRawMode();/disableRawMode(0);/g' kilo.c

    # 3. main fonksiyonu içinde, return 0; satırından hemen önce disableRawMode(0); ekle (eğer yoksa)
    if ! grep -q "disableRawMode(0);" kilo.c; then
        sed -i '/return 0;/i \    disableRawMode(0);' kilo.c
    fi

    # 4. Son bir kontrol: hala "atexit" geçiyor mu?
    if grep -n "atexit" kilo.c | grep -v "^[0-9]*:[[:space:]]*//"; then
        warn "UYARI: kilo.c içinde hala 'atexit' geçiyor (yorum satırı dışında). Lütfen manuel olarak kontrol edin."
    else
        info "atexit temizlendi."
    fi
else
    error "kilo.c bulunamadı!"
fi
cd ..

# ── Çıktı dizinini oluştur ────────────────────────────────────
mkdir -p "${OUTPUT_DIR}"

# ── crt0.asm'yi derle ─────────────────────────────────────────
info "crt0.asm derleniyor..."
nasm -f elf64 -o crt0.o "${CRT0_ASM}"

# ── Derleme (libc'yi en sona koy) ─────────────────────────────
info "Derleme başlıyor..."
cd "${KILO_SRC_DIR}"

${TARGET}-gcc \
    -static \
    -nostdlib \
    -ffreestanding \
    -fno-stack-protector \
    -mno-red-zone \
    -mcmodel=small \
    -ffunction-sections \
    -fdata-sections \
    -I"${MUSL_PREFIX}/include" \
    -isystem "${GCC_INCLUDE}" \
    -Wno-builtin-declaration-mismatch \
    -fno-builtin-malloc -fno-builtin-free \
    -L"${MUSL_PREFIX}/lib" \
    -T "${USER_LD}" \
    -Wl,--gc-sections \
    -Wl,-u,___errno_location \
    -Wl,-u,__errno_location \
    -o "${OUTPUT_DIR}/kilo.elf" \
    ../crt0.o \
    kilo.c \
    "${SYSCALLS_OBJ}" \
    -lc \
    "${LIBGCC}"

cd ..

# ── Doğrulama ─────────────────────────────────────────────────
if [ -f "${OUTPUT_DIR}/kilo.elf" ]; then
    SIZE=$(du -sh "${OUTPUT_DIR}/kilo.elf" | cut -f1)
    info "kilo başarıyla derlendi: ${OUTPUT_DIR}/kilo.elf (${SIZE})"
else
    error "kilo.elf oluşturulamadı!"
fi

# ── Diske kopyalama (mtools ile) ──────────────────────────────
if command -v mcopy >/dev/null 2>&1; then
    if [ -f "${DISK_IMG}" ]; then
        info "Diske kopyalanıyor: ${OUTPUT_DIR}/kilo.elf -> ${DISK_IMG}@@${DISK_OFFSET}::KILO.ELF"
        mcopy -i "${DISK_IMG}@@${DISK_OFFSET}" -o "${OUTPUT_DIR}/kilo.elf" ::KILO.ELF
        if [ $? -eq 0 ]; then
            info "Kopyalama başarılı."
        else
            warn "Kopyalama başarısız!"
        fi
    else
        warn "Disk imajı bulunamadı: ${DISK_IMG}. Kopyalama atlandı."
    fi
else
    warn "mcopy (mtools paketi) bulunamadı. Lütfen 'sudo apt install mtools' ile kurun."
fi

# ── Özet ──────────────────────────────────────────────────────
echo ""
echo -e "${GRN}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║   kilo text editor port BAŞARILI                     ║${NC}"
echo -e "${GRN}╠══════════════════════════════════════════════════════╣${NC}"
echo -e "${GRN}║${NC}  Çıktı          : ${OUTPUT_DIR}/kilo.elf"
echo -e "${GRN}║${NC}  Boyut          : ${SIZE}"
echo -e "${GRN}║${NC}  Kullanılan libc: musl ${MUSL_PREFIX}"
echo -e "${GRN}║${NC}  crt0           : ${CRT0_ASM}"
if [ -f "${DISK_IMG}" ]; then
    echo -e "${GRN}║${NC}  Diske kopyalandı: ${DISK_IMG}@@${DISK_OFFSET}::KILO.ELF"
fi
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  ⚠️  ÖNEMLİ: kilo ANSI escape kodları kullanır."
echo -e "${GRN}║${NC}     Framebuffer'da düzgün görünmesi için"
echo -e "${GRN}║${NC}     bir terminal emülatörü (VT100) gerekir."
echo -e "${GRN}║${NC}     Şimdilik SERIAL KONSOL üzerinden test edin."
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Kullanım: ./kilo.elf (FAT32'de /bin/kilo olarak)"
echo -e "${GRN}╚══════════════════════════════════════════════════════╝${NC}"
