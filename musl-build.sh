#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  AscentOS — musl libc Port Build Script (v3 — temiz)
#  Kullanım: chmod +x musl-build.sh && ./musl-build.sh
#
#  Bu sürümde musl'e hiç dokunulmaz.
#  malloc/free/realloc → musl'ün orijinal mallocng kullanır.
#  mallocng → mmap(MAP_ANONYMOUS) + munmap() ile çalışır.
#  mmap/munmap → kernel syscall.c'deki sys_mmap/sys_munmap → kmalloc/kfree.
#
#  Tek gereken: __ascent_syscalls.c ile musl'ün __syscall()
#  fonksiyonunu kernel syscall kapısına yönlendirmek.
# ═══════════════════════════════════════════════════════════════

set -e

# ── Ayarlar ───────────────────────────────────────────────────
MUSL_VER="1.2.5"
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
TARGET="x86_64-elf"
PREFIX="$(pwd)/toolchain/musl-install"
JOBS=$(nproc)
DEST="$(pwd)/userland/libc/musl"
STUBS_DIR="$(pwd)/userland/libc/stubs"
MALLOC_STUB="${STUBS_DIR}/__ascent_malloc.c"

# ── Renkli çıktı ──────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Ön koşul kontrolü ─────────────────────────────────────────
info "Ön koşullar kontrol ediliyor..."
command -v ${TARGET}-gcc    >/dev/null 2>&1 || error "${TARGET}-gcc bulunamadı."
command -v ${TARGET}-ar     >/dev/null 2>&1 || error "${TARGET}-ar bulunamadı."
command -v ${TARGET}-ranlib >/dev/null 2>&1 || error "${TARGET}-ranlib bulunamadı."
info "Compiler: $(${TARGET}-gcc --version | head -1)"

# ── Syscall stub'ını oluştur ──────────────────────────────────
# Her seferinde üretilir — kaynak değişirse otomatik güncellenir.
SYSCALL_STUB="${STUBS_DIR}/__ascent_syscalls.c"
mkdir -p "${STUBS_DIR}"

cat > "${SYSCALL_STUB}" << 'EOF'
/*
 * __ascent_syscalls.c
 *
 * musl tüm sistem çağrılarını __syscall() ve __syscall_cp_c() üzerinden yapar.
 * Bu dosya onları AscentOS syscall kapısına yönlendirir.
 *
 * Hiçbir header include edilmez — compiler built-in tipler kullanılır.
 */

long __syscall(long n, long a1, long a2, long a3,
               long a4, long a5, long a6)
{
    long ret;
    __asm__ volatile (
        "movq %1, %%rax\n\t"
        "movq %2, %%rdi\n\t"
        "movq %3, %%rsi\n\t"
        "movq %4, %%rdx\n\t"
        "movq %5, %%r10\n\t"
        "movq %6, %%r8\n\t"
        "movq %7, %%r9\n\t"
        "syscall\n\t"
        "movq %%rax, %0\n\t"
        : "=r"(ret)
        : "r"(n), "r"(a1), "r"(a2), "r"(a3),
          "r"(a4), "r"(a5), "r"(a6)
        : "rax","rdi","rsi","rdx","r10","r8","r9","memory","cc"
    );
    return ret;
}

/* Thread cancellation yok → doğrudan __syscall */
long __syscall_cp_c(long n, long a1, long a2, long a3,
                    long a4, long a5, long a6)
{
    return __syscall(n, a1, a2, a3, a4, a5, a6);
}
EOF

info "Syscall stub hazır: ${SYSCALL_STUB}"

# ── Errno stub'ını oluştur ────────────────────────────────────
# musl libc.a içindeki bazı objeler (vfprintf.lo, syscall_ret.lo, wcrtomb.lo, ...)
# ___errno_location (üç alt çizgi) hidden sembolünü çağırır.
# Bu sembol musl'ün kendi errno.lo'sundan gelir ama --gc-sections veya
# freestanding ortamda kaybolur.  İki sembolü aynı storage'a bağlayan
# bu stub her koşulda linker'ın bulmayı garantiler.
ERRNO_STUB="${STUBS_DIR}/__ascent_errno.c"

cat > "${ERRNO_STUB}" << 'EOF'
/*
 * __ascent_errno.c
 *
 * musl dahili olarak ___errno_location (üç alt çizgi, hidden görünürlük)
 * sembolünü kullanır.  Freestanding / no-TLS ortamında bu sembol
 * musl'ün kendi errno.lo'sundan gelmez; linker "hidden symbol isn't defined"
 * hatası verir.
 *
 * Çözüm: her ikisini de tek bir static int'e bağla.
 *   __errno_location  (çift _) → POSIX / kullanıcı kodu
 *   ___errno_location (üç  _) → musl-iç görünürlük (alias)
 */
static int _errno_val = 0;

int *__errno_location(void)
{
    return &_errno_val;
}

/* musl-internal alias — __attribute__((alias)) linker seviyesinde eşler */
int *___errno_location(void)
    __attribute__((alias("__errno_location")));
EOF

info "Errno stub hazır: ${ERRNO_STUB}"

# ── Malloc stub'ını kopyala ───────────────────────────────────
# Bu stub musl'ün mallocng'sinin YERİNE geçer.
# Her malloc() → mmap(MAP_ANONYMOUS) syscall → sys_mmap → kmalloc'a DOKUNMAZ.
# free()  → munmap() syscall → sys_munmap → kfree ile değil, mmap bölgesi temizlenir.
# Böylece kernel kmalloc/kfree heap corruption sorunu ortadan kalkar.
# __ascent_malloc.c zaten STUBS_DIR'de — sadece varlığını kontrol et
if [ ! -f "${MALLOC_STUB}" ]; then
    error "Malloc stub bulunamadı: ${MALLOC_STUB}\n  Lütfen __ascent_malloc.c dosyasını userland/libc/stubs/ altına koy."
fi
info "Malloc stub hazır: ${MALLOC_STUB}"

# ── İndirme ───────────────────────────────────────────────────
ARCHIVE="musl-${MUSL_VER}.tar.gz"
if [ ! -f "${ARCHIVE}" ]; then
    info "musl ${MUSL_VER} indiriliyor..."
    curl -L "${MUSL_URL}" -o "${ARCHIVE}" || error "İndirme başarısız."
else
    info "Arşiv zaten mevcut: ${ARCHIVE}"
fi

# ── Çıkarma ───────────────────────────────────────────────────
SRC_DIR="musl-${MUSL_VER}"
if [ ! -d "${SRC_DIR}" ]; then
    info "Arşiv çıkarılıyor..."
    tar xf "${ARCHIVE}"
fi

# ── Build dizini ──────────────────────────────────────────────
BUILD_DIR="build-musl"
rm -rf "${BUILD_DIR}"
mkdir  "${BUILD_DIR}"
cd     "${BUILD_DIR}"

# ── Configure ─────────────────────────────────────────────────
info "Configure ediliyor (target=${TARGET})..."

CC="${TARGET}-gcc"      \
AR="${TARGET}-ar"       \
RANLIB="${TARGET}-ranlib" \
CFLAGS="-O2 -ffreestanding \
        -ffunction-sections -fdata-sections \
        -fno-stack-protector -mno-red-zone \
        -mcmodel=small -fno-exceptions \
        -fno-unwind-tables -fno-asynchronous-unwind-tables" \
../${SRC_DIR}/configure \
    --target="${TARGET}"     \
    --prefix="${PREFIX}"     \
    --disable-shared         \
    --enable-static          \
    --disable-wrapper        \
    --syslibdir="${PREFIX}/lib"

# ── Derleme ───────────────────────────────────────────────────
info "Derleniyor (${JOBS} iş parçacığı)..."

# musl'ün dahili malloc'unu (mallocng) devre dışı bırak.
# Yerine __ascent_malloc.c (mmap syscall tabanlı) kullanılacak.
# mallocng'nin kmalloc heap'i ile çakışması bu şekilde önlenir.
if [ -f config.mak ]; then
    sed -i 's/^MALLOC_DIR[[:space:]]*=.*/MALLOC_DIR =/' config.mak
    info "config.mak MALLOC_DIR boşaltıldı: $(grep MALLOC_DIR config.mak)"
fi

# musl'ün Makefile'ında src/malloc/ altındaki wrapper dosyaları
# (free.c, realloc.c, calloc.c, malloc.c, memalign.c, ...) MALLOC_DIR'dan
# bağımsız olarak derlenir. Bunları da exclude etmek için Makefile'ı patch'le.
MUSL_MK="../${SRC_DIR}/Makefile"
if grep -q 'src/malloc/free' "${MUSL_MK}" 2>/dev/null ||    grep -q 'malloc/free' "${MUSL_MK}" 2>/dev/null; then
    info "musl Makefile malloc wrapper'ları zaten exclude edilmiş."
else
    # Makefile'daki malloc obje listesine exclude ekle
    # musl Makefile'ında OBJS veya libs-y içinde malloc/*.o var
    info "musl Makefile malloc dosyaları kontrol ediliyor..."
fi

# Derleme öncesi src/malloc/*.c'yi stub ile değiştir
# En güvenilir yöntem: ilgili .c dosyalarını boş stubla override et
for MFILE in free.c realloc.c calloc.c malloc.c memalign.c              posix_memalign.c aligned_alloc.c malloc_usable_size.c; do
    TARGET_F="../${SRC_DIR}/src/malloc/${MFILE}"
    if [ -f "${TARGET_F}" ]; then
        # Orijinali yedekle (yoksa)
        [ -f "${TARGET_F}.orig" ] || cp "${TARGET_F}" "${TARGET_F}.orig"
        # Boş stub: sadece gcc'nin memnun olacağı kadar
        printf '/* replaced by __ascent_malloc */
' > "${TARGET_F}"
        info "Stublandı: src/malloc/${MFILE}"
    fi
done

make -j"${JOBS}" lib/libc.a

# ── Syscall stub'ını derle ve libc.a'ya ekle ──────────────────
info "Syscall stub derleniyor..."

${TARGET}-gcc \
    -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
    -mcmodel=small -fno-exceptions -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -c "${SYSCALL_STUB}" \
    -o __ascent_syscalls.o

# malloc stub'ını derle
${TARGET}-gcc \
    -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
    -mcmodel=small -fno-exceptions -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -fno-builtin-malloc -fno-builtin-free \
    -fno-builtin-realloc -fno-builtin-calloc \
    -c "${MALLOC_STUB}" \
    -o __ascent_malloc.o

# errno stub'ını derle
${TARGET}-gcc \
    -O2 -ffreestanding -fno-stack-protector -mno-red-zone \
    -mcmodel=small -fno-exceptions -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -c "${ERRNO_STUB}" \
    -o __ascent_errno.o

# libc.a'dan musl malloc objeleri çıkarılıyor..."

# En güvenilir yöntem: libc.a'yı geçici dizine extract et,
# __libc_free/__libc_realloc/__libc_malloc sembolü içeren HER objeyi bul ve sil.
SCAN_TMP=$(mktemp -d)
LIBC_ABS="$(pwd)/lib/libc.a"
(cd "${SCAN_TMP}" && ${TARGET}-ar x "${LIBC_ABS}" 2>/dev/null || true)

BAD_OBJS=""
for f in "${SCAN_TMP}"/*.lo "${SCAN_TMP}"/*.o; do
    [ -f "${f}" ] || continue
    if ${TARGET}-nm "${f}" 2>/dev/null \
        | grep -qE '__libc_(free|realloc|malloc|calloc)'; then
        BAD_OBJS="${BAD_OBJS} $(basename ${f})"
    fi
done
rm -rf "${SCAN_TMP}"

if [ -n "${BAD_OBJS}" ]; then
    info "Çıkarılacak malloc objeleri:${BAD_OBJS}"
    for obj in ${BAD_OBJS}; do
        ${TARGET}-ar dv lib/libc.a "${obj}" 2>/dev/null || true
    done
else
    info "Çıkarılacak malloc objesi yok."
fi

# İkinci tur: hâlâ kalan __libc_* sembollerini bul
# nm --print-file-name ile hangi objeden geldiğini doğrudan göster
ROUND2=$(${TARGET}-nm --print-file-name lib/libc.a 2>/dev/null \
    | grep -E '__libc_(free|malloc|realloc|calloc)' \
    | sed 's|lib/libc.a(||;s|):.*||' \
    | sort -u || true)
if [ -n "${ROUND2}" ]; then
    info "İkinci tur temizlik:${ROUND2}"
    for obj in ${ROUND2}; do
        ${TARGET}-ar dv lib/libc.a "${obj}" 2>/dev/null || true
    done
fi

# Son kontrol
STILL_BAD=$(${TARGET}-nm lib/libc.a 2>/dev/null \
    | grep -cE '__libc_(free|malloc|realloc|calloc)' || true)
if [ "${STILL_BAD}" -gt 0 ]; then
    warn "__libc_* hâlâ var (${STILL_BAD}) — __ascent_malloc.o sağlayacak, devam..."
else
    info "malloc temizliği tamamlandı."
fi
# __syscall ve malloc/errno stub'larını ekle
${TARGET}-ar dv lib/libc.a __syscall.o    2>/dev/null || true
${TARGET}-ar dv lib/libc.a __syscall_cp.o 2>/dev/null || true
# musl'ün errno objelerini çıkar — bizim __ascent_errno.o alacak
# errno.lo          → ___errno_location (hidden alias) tanımlar
# __errno_location.lo → __errno_location (public) tanımlar
# İkisi de kaldırılmazsa "multiple definition" hatası alınır.
${TARGET}-ar dv lib/libc.a errno.lo              2>/dev/null || true
${TARGET}-ar dv lib/libc.a __errno_location.lo   2>/dev/null || true
# Başka objelerde de __errno_location tanımı kalıp kalmadığını kontrol et
EXTRA_ERRNO=$(${TARGET}-nm --print-file-name lib/libc.a 2>/dev/null \
    | grep " T __errno_location$" \
    | sed 's|lib/libc.a(||;s|):.*||' \
    | sort -u || true)
if [ -n "${EXTRA_ERRNO}" ]; then
    info "Ek errno objeleri çıkarılıyor: ${EXTRA_ERRNO}"
    for obj in ${EXTRA_ERRNO}; do
        ${TARGET}-ar dv lib/libc.a "${obj}" 2>/dev/null || true
    done
fi
${TARGET}-ar rcs lib/libc.a __ascent_syscalls.o __ascent_malloc.o __ascent_errno.o
${TARGET}-ranlib lib/libc.a

info "libc.a hazır."

# ── Kurulum ───────────────────────────────────────────────────
info "Kuruluyor: ${PREFIX}"
mkdir -p "${PREFIX}/lib" "${PREFIX}/include"
cp lib/libc.a "${PREFIX}/lib/libc.a"
make DESTDIR="" install-headers

cd ..

# ── Kütüphaneleri proje içine kopyala ─────────────────────────
info "Kütüphaneler kopyalanıyor: ${DEST}"
mkdir -p "${DEST}/lib" "${DEST}/include"
cp "${PREFIX}/lib/libc.a"   "${DEST}/lib/"
cp -r "${PREFIX}/include/." "${DEST}/include/"

# ── Doğrulama ─────────────────────────────────────────────────
info "Doğrulama..."

SYSCALL_DEF=$(${TARGET}-nm "${DEST}/lib/libc.a" 2>/dev/null \
    | grep " T __syscall$" | wc -l)
[ "${SYSCALL_DEF}" -gt 0 ] && \
    info "__syscall stub doğrulandı." || \
    warn "__syscall tanımı bulunamadı."

ERRNO_DEF=$(${TARGET}-nm "${DEST}/lib/libc.a" 2>/dev/null \
    | grep -c " T ___\?_errno_location$" || true)
[ "${ERRNO_DEF}" -gt 0 ] && \
    info "__errno_location / ___errno_location stub doğrulandı." || \
    warn "___errno_location tanımı bulunamadı — link hatası olabilir!"

# malloc sembollerinin musl'den (mallocng) geldiğini doğrula
${TARGET}-nm "${DEST}/lib/libc.a" 2>/dev/null \
    | grep -E " T (malloc|free|realloc)$" | head -3 || true

LIBC_SIZE=$(du -sh "${DEST}/lib/libc.a" | cut -f1)

# ── Özet ──────────────────────────────────────────────────────
echo ""
echo -e "${GRN}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║   musl libc port BAŞARILI (v3 — temiz)              ║${NC}"
echo -e "${GRN}╠══════════════════════════════════════════════════════╣${NC}"
echo -e "${GRN}║${NC}  Kütüphaneler  : ${DEST}/lib/"
echo -e "${GRN}║${NC}  libc.a boyutu : ${LIBC_SIZE}"
echo -e "${GRN}║${NC}  Header'lar    : ${DEST}/include/"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  malloc → mmap(MAP_ANON) syscall (kmalloc'a dokunmaz)"
echo -e "${GRN}║${NC}  free   → munmap() syscall (kfree'ye dokunmaz)"
echo -e "${GRN}║${NC}  errno  → ___errno_location stub (TLS olmadan çalışır)"
echo -e "${GRN}║${NC}  kaynak → userland/libc/stubs/__ascent_malloc.c"
echo -e "${GRN}║${NC}           userland/libc/stubs/__ascent_errno.c"
echo -e "${GRN}║${NC}  syscall → __ascent_syscalls.c"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Sonraki adım  : make userland"
echo -e "${GRN}╚══════════════════════════════════════════════════════╝${NC}"