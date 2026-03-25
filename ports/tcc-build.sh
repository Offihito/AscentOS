#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  AscentOS — TCC Tam Kurulum Scripti
#  (tcc-build.sh + tcc-install.sh birleştirilmiş hali)
#
#  Adımlar:
#   1. TCC kaynağını al / güncelle
#   2. config.h üret
#   3. Kaynak yamaları uygula
#   4. Uyum katmanını derle
#   5. TCC'yi derle (tcc.elf)
#   6. Binary patch uygula
#   7. AscentOS libc.a üret
#   8. Runtime dosyaları disk imajına kur
#      (crt1.o, crti.o, crtn.o, libc.a, libtcc1.a,
#       libgcc_s.a, tccdefs.h, musl header'ları, test.c)
#   9. tcc.elf'i disk imajına kopyala
#
#  Gereksinimler: x86_64-elf toolchain, nasm, git, musl-libc 1.2.5
#  Kullanım    : chmod +x tcc-setup.sh && ./tcc-setup.sh
# ═══════════════════════════════════════════════════════════════

set -e

# ── Renk & loglama ────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Yapılandırma ──────────────────────────────────────────────
DISK_IMG="${DISK_IMG:-}"
TCC_VERSION="${TCC_VERSION:-0.9.27}"
TCC_SRC_DIR="tinycc"
TCC_REPO="https://repo.or.cz/tinycc.git"

TARGET="x86_64-elf"
SCRIPT_DIR="$(cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"            # projenin kök dizini (bir üst klasör)
DISK_IMG="${DISK_IMG:-${ROOT}/disk.img}"
MUSL_PREFIX="${ROOT}/toolchain/musl-install"
USER_LD="${ROOT}/userland/libc/user.ld"
CRT0_ASM="${ROOT}/userland/libc/crt0.asm"
OUTPUT_DIR="${ROOT}/userland/bin"
SYSCALLS_OBJ="${ROOT}/userland/out/syscalls.o"
SYSCALLS_C="${ROOT}/userland/libc/syscalls.c"
ASCENTOS_LIBC_A="${ROOT}/userland/out/ascentos_libc.a"
LIBGCC=$(${TARGET}-gcc -m64 --print-libgcc-file-name 2>/dev/null)
GCC_INCLUDE=$(${TARGET}-gcc -m64 -print-file-name=include 2>/dev/null)

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 0 — Yardımcı fonksiyonlar
# ════════════════════════════════════════════════════════════════

# Disk üzerinde dizin oluştur
disk_mkdir() {
    local dir="$1"
    if command -v debugfs >/dev/null 2>&1; then
        debugfs -w "${DISK_IMG}" -R "mkdir ${dir}" 2>/dev/null || true
    fi
}

# Dosyayı disk imajına yaz (debugfs > e2cp > loop mount sırası)
disk_write_file() {
    local src="$1"
    local dst="$2"
    if command -v debugfs >/dev/null 2>&1; then
        debugfs -w "${DISK_IMG}" -R "write ${src} ${dst}" 2>/dev/null \
            && info "  → disk: ${dst}" \
            || warn "  → disk yazma başarısız: ${dst}"
    elif command -v e2cp >/dev/null 2>&1; then
        e2cp "${src}" "${DISK_IMG}:/${dst}" \
            && info "  → e2cp: ${dst}" \
            || warn "  → e2cp başarısız: ${dst}"
    else
        local MNT_TMP="/tmp/ascentos_mnt_rt"
        mkdir -p "${MNT_TMP}"
        sudo mount -o loop "${DISK_IMG}" "${MNT_TMP}" || error "mount başarısız"
        sudo mkdir -p "${MNT_TMP}/$(dirname ${dst})"
        sudo cp "${src}" "${MNT_TMP}/${dst}" \
            && info "  → loop: ${dst}" \
            || warn "  → loop yazma başarısız: ${dst}"
        sudo umount "${MNT_TMP}" && rmdir "${MNT_TMP}"
    fi
}

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 1 — Ön koşul kontrolü
# ════════════════════════════════════════════════════════════════
info "Ön koşullar kontrol ediliyor..."
command -v ${TARGET}-gcc >/dev/null 2>&1 || error "${TARGET}-gcc bulunamadı. Önce toolchain kurulumunu yapın."
command -v nasm          >/dev/null 2>&1 || error "nasm bulunamadı (sudo apt install nasm)."
command -v git           >/dev/null 2>&1 || error "git bulunamadı (sudo apt install git)."
[ -d "${MUSL_PREFIX}" ] || error "musl kurulum dizini bulunamadı: ${MUSL_PREFIX}"
[ -f "${USER_LD}" ]     || error "Linker script (user.ld) bulunamadı: ${USER_LD}"
[ -f "${CRT0_ASM}" ]    || error "crt0.asm bulunamadı: ${CRT0_ASM}"
[ -f "${SYSCALLS_OBJ}" ] || error "syscalls.o bulunamadı: ${SYSCALLS_OBJ} — önce 'make userland/out/syscalls.o' çalıştırın."

# libc.a sembol kontrolü
for sym in malloc free memcpy memset; do
    if ${TARGET}-nm "${MUSL_PREFIX}/lib/libc.a" 2>/dev/null | grep -q " T ${sym}$"; then
        info "libc.a içinde '${sym}' bulundu."
    else
        warn "libc.a içinde '${sym}' bulunamadı! musl-build.sh düzgün çalışmamış olabilir."
    fi
done

info "Compiler: $(${TARGET}-gcc --version | head -1)"

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 2 — TCC kaynağını al / güncelle
# ════════════════════════════════════════════════════════════════
if [ -d "${TCC_SRC_DIR}" ]; then
    info "TCC dizini zaten mevcut, güncelleniyor..."
    cd "${TCC_SRC_DIR}" && git pull || warn "git pull başarısız, mevcut kaynak kullanılacak."
    cd ..
else
    info "TCC klonlanıyor: ${TCC_REPO}"
    git clone --depth=1 "${TCC_REPO}" "${TCC_SRC_DIR}"
fi

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 3 — config.h üret (./configure yerine elle)
# ════════════════════════════════════════════════════════════════
info "config.h üretiliyor..."
cat > "${TCC_SRC_DIR}/config.h" << CONFEOF
/* config.h — tcc-setup.sh tarafından AscentOS için otomatik üretildi */
#define TCC_VERSION    "${TCC_VERSION}"
#define TCC_GITHASH    "ascentos-port"

/* Hedef platform */
#define TCC_TARGET_X86_64   1
#undef  TCC_TARGET_I386
#undef  TCC_TARGET_ARM
#undef  TCC_TARGET_ARM64
#undef  TCC_TARGET_RISCV64

/* Host OS: Linux benzeri (AscentOS) */
#define HOST_OS        "Linux"
#define HOST_MACHINE   "x86_64"

/* Özellik bayrakları */
#define CONFIG_TCC_STATIC       1       /* statik bağlama */
#define HAVE_DLOPEN             0       /* dlopen yok */
#define CONFIG_USE_LIBGCC       1       /* libgcc.a kullan */
#undef  CONFIG_TCC_LIBGCC_S               /* shared libgcc_s.so yok */

/* Sistem yolları — TCC çalışma zamanında bu yolları arar */
#define CONFIG_TCC_SYSINCLUDEPATHS  "/usr/include"
#define CONFIG_TCC_LIBPATHS         "/usr/lib:/lib"
#define CONFIG_TCC_CRT_PREFIX       "/usr/lib"
#define CONFIG_TCC_LIBTCC1          "/usr/lib/libtcc1.a"
#define CONFIG_TCC_ELFINTERP        "/lib/ld-musl-x86_64.so.1"
#define CONFIG_LDDIR                "lib"
#define CONFIG_MULTIARCHDIR         "x86_64-linux-musl"

/* Opsiyonel özellikler — AscentOS'ta kapalı */
#undef  CONFIG_TCC_BCHECK           /* bounds checking yok */
#undef  CONFIG_TCC_BACKTRACE        /* backtrace yok */
CONFEOF
info "config.h üretildi: ${TCC_SRC_DIR}/config.h"

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 4 — Yama öncesi temizlik + kaynak yamaları
# ════════════════════════════════════════════════════════════════

# Önceki çalıştırmada bırakılan .bak dosyalarını temizle
cd "${TCC_SRC_DIR}"
for f in tcc.h tccrun.c tcctools.c tccelf.c tcc.c tcclink.c; do
    OLDEST_BAK=$(ls ${f}.bak.* 2>/dev/null | sort | head -1)
    if [ -n "${OLDEST_BAK}" ]; then
        info "${f} orijinal yedekten geri yükleniyor: ${OLDEST_BAK}"
        cp "${OLDEST_BAK}" "${f}"
        rm -f ${f}.bak.*
    fi
done

info "TCC'ye yamalar uygulanıyor..."

# ── tcc.h: AscentOS platform bloğu ──────────────────────────
if [ -f tcc.h ]; then
    cp tcc.h tcc.h.bak.$(date +%s)
    if ! grep -q "ASCENTOS_TCC" tcc.h; then
        cat > /tmp/tcc_patch.h << 'TCC_PATCH'
/* ── AscentOS TCC platform tanımları ──────────────────────────
 * tcc-setup.sh tarafından otomatik eklendi.
 * TCC_TARGET_X86_64 : sadece x86_64 backend derle
 * CONFIG_TCC_STATIC : statik bağlama modu
 * ─────────────────────────────────────────────────────────── */
#define ASCENTOS_TCC        1
#define TCC_TARGET_X86_64   1
#define CONFIG_TCC_STATIC   1

/* ldl / libpthread yok → ilgili guard'ları kapat */
#ifndef HAVE_DLOPEN
#define HAVE_DLOPEN 0
#endif

/* realpath stub — tam VFS olmadan da çalışsın */
#ifndef ASCENTOS_NO_REALPATH
#define ASCENTOS_NO_REALPATH 1
#endif
/* ─────────────────────────────────────────────────────────── */
TCC_PATCH
        cat /tmp/tcc_patch.h tcc.h > /tmp/tcc_patched.h
        mv /tmp/tcc_patched.h tcc.h
        rm -f /tmp/tcc_patch.h
        info "tcc.h: AscentOS platform bloğu eklendi."
    fi
else
    error "tcc.h bulunamadı! TCC kaynağı bozuk olabilir."
fi

# ── tccrun.c: mmap(PROT_EXEC) yaması ────────────────────────
if [ -f tccrun.c ]; then
    cp tccrun.c tccrun.c.bak.$(date +%s)
    sed -i 's/mmap(NULL, \(.*\), PROT_READ|PROT_WRITE|PROT_EXEC,/\
    \/* AscentOS: PROT_EXEC kaldırıldı *\/\n    mmap(NULL, \1, PROT_READ|PROT_WRITE,/g' tccrun.c 2>/dev/null || true
    info "tccrun.c yamalandı (PROT_EXEC temizlendi)."
fi

# ── tccelf.c / tcc.c / tcclink.c: libgcc_s.so.1 aramasını kaldır ──
for src_file in tccelf.c tcc.c tcclink.c; do
    if [ -f "${src_file}" ] && grep -q "libgcc_s" "${src_file}" 2>/dev/null; then
        cp "${src_file}" "${src_file}.bak.libgcc.$(date +%s)"
        sed -i 's|"libgcc_s\.so\.1"|""|g' "${src_file}"
        sed -i 's|"libgcc_s"|""|g' "${src_file}"
        info "${src_file}: libgcc_s.so.1 referansı temizlendi."
    fi
done

# ── tcctools.c: realpath stub ────────────────────────────────
if [ -f tcctools.c ]; then
    cp tcctools.c tcctools.c.bak.$(date +%s)
    if ! grep -q "ASCENTOS_NO_REALPATH" tcctools.c; then
        sed -i '1s/^/\/* AscentOS: realpath stub *\/\n#ifdef ASCENTOS_NO_REALPATH\n#define realpath(p,r) (strncpy((r),(p),PATH_MAX),(r))\n#endif\n/' tcctools.c
        info "tcctools.c: realpath stub eklendi."
    fi
fi

cd ..

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 5 — Uyum katmanı & tanım header'ı
# ════════════════════════════════════════════════════════════════
info "ascentos_tcc_compat.c oluşturuluyor..."
cat > "${SCRIPT_DIR}/ascentos_tcc_compat.c" << 'COMPAT_EOF'
/*
 * ascentos_tcc_compat.c — AscentOS TCC uyum katmanı
 *
 * 1. DL stub'ları      — dlopen/dlsym/dlclose/dlerror NULL/hata döner
 * 2. getcwd stub       — "/" döner (minimal VFS ortamı)
 * 3. __secs_to_zone    — musl timezone iç sembolü; UTC sabit döner
 * 4. __syscall_cp      — musl cancellable syscall; doğrudan syscall yapar
 * 5. __progname        — bazı TCC sürümleri extern bildirir
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* ── DL stub'ları ─────────────────────────────────────────── */
void *dlopen(const char *filename, int flags)  { (void)filename; (void)flags; return NULL; }
void *dlsym(void *handle, const char *symbol)  { (void)handle; (void)symbol; return NULL; }
int   dlclose(void *handle)                    { (void)handle; return 0; }
const char *dlerror(void) { return "dlopen not supported on AscentOS"; }

/* ── getcwd stub ──────────────────────────────────────────── */
#ifdef ASCENTOS_GETCWD_STUB
char *getcwd(char *buf, size_t size)
{
    if (!buf || size < 2) { errno = ERANGE; return NULL; }
    buf[0] = '/'; buf[1] = '\0';
    return buf;
}
#endif

/* ── __secs_to_zone (musl timezone iç sembolü, UTC sabit) ── */
void __secs_to_zone(long long t, int local,
                    int *isdst, long *offset,
                    long *oppoff, const char **zonename)
{
    (void)t; (void)local;
    if (isdst)    *isdst    = 0;
    if (offset)   *offset   = 0;
    if (oppoff)   *oppoff   = 0;
    if (zonename) *zonename = "UTC";
}

/* ── __syscall_cp (thread cancellation yok, doğrudan syscall) ── */
long __syscall_cp(long nr,
                  long a, long b, long c,
                  long d, long e, long f)
{
    register long _nr __asm__("rax") = nr;
    register long _a  __asm__("rdi") = a;
    register long _b  __asm__("rsi") = b;
    register long _c  __asm__("rdx") = c;
    register long _d  __asm__("r10") = d;
    register long _e  __asm__("r8")  = e;
    register long _f  __asm__("r9")  = f;
    __asm__ volatile (
        "syscall"
        : "+r"(_nr)
        : "r"(_a),"r"(_b),"r"(_c),"r"(_d),"r"(_e),"r"(_f)
        : "rcx","r11","memory"
    );
    return _nr;
}

/* ── __progname ───────────────────────────────────────────── */
const char *__progname = "tcc";
COMPAT_EOF
info "ascentos_tcc_compat.c oluşturuldu."

info "ascentos_tcc_defs.h oluşturuluyor..."
cat > "${SCRIPT_DIR}/ascentos_tcc_defs.h" << 'DEFS_EOF'
/* ascentos_tcc_defs.h — gcc -include ile tüm TCC kaynaklarına enjekte edilir */
#ifndef ASCENTOS_TCC_DEFS_H
#define ASCENTOS_TCC_DEFS_H

#ifndef TCC_TARGET_X86_64
#define TCC_TARGET_X86_64 1
#endif

#ifndef HAVE_DLOPEN
#define HAVE_DLOPEN 0
#endif

#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif /* ASCENTOS_TCC_DEFS_H */
DEFS_EOF
info "ascentos_tcc_defs.h oluşturuldu."

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 6 — user.ld'yi hazırla, crt0.asm'yi derle
# ════════════════════════════════════════════════════════════════
PATCHED_LD="${SCRIPT_DIR}/user_tcc.ld"
info "user.ld kopyalanıyor ve __heap_start ekleniyor..."
cp "${USER_LD}" "${PATCHED_LD}"
if ! grep -q "__heap_start" "${PATCHED_LD}"; then
    sed -i 's/__bss_end = \.;/__bss_end = .;\n        __heap_start = .;/' "${PATCHED_LD}"
    info "__heap_start sembolü user_tcc.ld'ye eklendi."
else
    info "__heap_start zaten mevcut, atlandı."
fi
USER_LD="${PATCHED_LD}"

mkdir -p "${OUTPUT_DIR}"

info "crt0.asm derleniyor..."
nasm -f elf64 -o "${ROOT}/userland/out/crt0.o" "${CRT0_ASM}"

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 7 — TCC'yi derle
# ════════════════════════════════════════════════════════════════

# ONE_SOURCE mod tespiti
ONE_SOURCE_MODE=0
if grep -q "ONE_SOURCE" "${TCC_SRC_DIR}/tcc.c" 2>/dev/null; then
    ONE_SOURCE_MODE=1
    info "ONE_SOURCE modu tespit edildi — sadece tcc.c derleniyor."
else
    info "ONE_SOURCE modu yok — kaynak dosyaları ayrı ayrı derleniyor."
fi

# Kaynak listesi
TCC_CORE_SRCS=""
for f in \
    "${TCC_SRC_DIR}/tcc.c" \
    "${TCC_SRC_DIR}/libtcc.c" \
    "${TCC_SRC_DIR}/tccpp.c" \
    "${TCC_SRC_DIR}/tccgen.c" \
    "${TCC_SRC_DIR}/tccelf.c" \
    "${TCC_SRC_DIR}/tccasm.c" \
    "${TCC_SRC_DIR}/tccrun.c" \
    "${TCC_SRC_DIR}/tcctools.c"; do
    [ -f "$f" ] && TCC_CORE_SRCS="${TCC_CORE_SRCS} $f" || warn "Kaynak dosya bulunamadı, atlandı: $f"
done
[ -f "${TCC_SRC_DIR}/x86_64-gen.c" ] && TCC_CORE_SRCS="${TCC_CORE_SRCS} ${TCC_SRC_DIR}/x86_64-gen.c"
[ -f "${TCC_SRC_DIR}/tccmacho.c"   ] && warn "tccmacho.c bulundu ama macOS hedefleri için kullanılmaz, atlandı."

info "Derlenecek kaynak dosyaları:"
for f in $TCC_CORE_SRCS; do echo "  $(basename $f)"; done

# Uyum katmanını derle
info "ascentos_tcc_compat.c derleniyor..."
${TARGET}-gcc \
    -c \
    -static \
    -ffreestanding \
    -fno-stack-protector \
    -mno-red-zone \
    -mcmodel=small \
    -std=c99 \
    -I"${TCC_SRC_DIR}" \
    -I"${MUSL_PREFIX}/include" \
    -isystem "${GCC_INCLUDE}" \
    -Wno-implicit-function-declaration \
    -o "${SCRIPT_DIR}/ascentos_tcc_compat.o" \
    "${SCRIPT_DIR}/ascentos_tcc_compat.c"
info "ascentos_tcc_compat.o derlendi."

# Ana derleme
info "Ana derleme başlıyor..."
if [ "${ONE_SOURCE_MODE}" = "1" ]; then
    COMPILE_SRCS="${TCC_SRC_DIR}/tcc.c"
    ONE_SOURCE_FLAG="-DONE_SOURCE=1"
else
    COMPILE_SRCS="${TCC_CORE_SRCS}"
    ONE_SOURCE_FLAG="-DONE_SOURCE=0"
fi

# mcmodel=large ZORUNLU:
#   mcmodel=small → tüm adresler 0x0–0x7FFFFFFF kabul edilir.
#   TCC çalışırken mmap() büyük adresler (0x10000000+) döndürür;
#   small modelde bu adresler 32-bit'e truncate edilir → yanlış bellek
#   erişimi → #UD / #GP kernel panic.
#   large modeli tüm adreslere 64-bit mov ile erişir — güvenli.
#
# -fno-plt: PLT yok (statik binary), doğrudan çağrı — daha küçük/hızlı.
# -fno-pic / -fno-pie: statik binary için PIC overhead yok.
${TARGET}-gcc \
    -static \
    -nostdlib \
    -ffreestanding \
    -fno-stack-protector \
    -mno-red-zone \
    -mcmodel=large \
    -fno-pic \
    -fno-pie \
    -ffunction-sections \
    -fdata-sections \
    -std=c99 \
    -DASCENTOS_TCC \
    -DTCC_TARGET_X86_64 \
    -DCONFIG_TCC_STATIC \
    -DHAVE_DLOPEN=0 \
    ${ONE_SOURCE_FLAG} \
    -include "${SCRIPT_DIR}/ascentos_tcc_defs.h" \
    -I"${TCC_SRC_DIR}" \
    -I"${MUSL_PREFIX}/include" \
    -isystem "${GCC_INCLUDE}" \
    -Wno-implicit-function-declaration \
    -Wno-builtin-declaration-mismatch \
    -Wno-deprecated-declarations \
    -Wno-pointer-sign \
    -fno-builtin-malloc -fno-builtin-free \
    -L"${MUSL_PREFIX}/lib" \
    -T "${USER_LD}" \
    -Wl,--gc-sections \
    -Wl,--allow-multiple-definition \
    -o "${OUTPUT_DIR}/tcc.elf" \
    "${ROOT}/userland/out/crt0.o" \
    "${SCRIPT_DIR}/ascentos_tcc_compat.o" \
    ${COMPILE_SRCS} \
    "${SYSCALLS_OBJ}" \
    -lc \
    "${LIBGCC}"

[ -f "${OUTPUT_DIR}/tcc.elf" ] \
    && { SIZE=$(du -sh "${OUTPUT_DIR}/tcc.elf" | cut -f1); info "TCC derlendi: ${OUTPUT_DIR}/tcc.elf (${SIZE})"; } \
    || error "tcc.elf oluşturulamadı!"

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 8 — Binary patch
# ════════════════════════════════════════════════════════════════
info "Binary patch uygulanıyor: ${OUTPUT_DIR}/tcc.elf"
python3 - "${OUTPUT_DIR}/tcc.elf" << 'BPATCH_RUN'
import sys, re
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
patches = []

p1 = b"libgcc_s.so.1"
if p1 in data:
    data = bytearray(bytes(data).replace(p1, bytes(len(p1))))
    patches.append("libgcc_s.so.1 -> null")

needle = b"pthread" + bytes([0]) + b"c" + bytes([0])
m2 = re.search(needle + b"(/lib/)(" + bytes(13) + b")", bytes(data))
if m2:
    i = m2.start(1)
    data[i:i+5] = bytes(5)
    patches.append("/lib/ prefix -> null")

m3 = re.search(needle + b"(" + bytes(18) + b")", bytes(data))
if m3:
    i = m3.start(1)
    data[i:i+18] = b"libgcc_s.a" + bytes(8)
    patches.append("null*18 -> libgcc_s.a")

p4 = b"libgcc_s.a" + bytes(1)
if p4 in bytes(data):
    data = bytearray(bytes(data).replace(p4, b"libtcc1.a" + bytes(2)))
    patches.append("libgcc_s.a -> libtcc1.a")

p5 = b"/lib/tcc" + bytes(1)
if p5 in bytes(data):
    data = bytearray(bytes(data).replace(p5, b"/usr/lib" + bytes(1), 1))
    patches.append("CRT prefix /lib/tcc -> /usr/lib")

open(path, "wb").write(data)
for p in patches:
    print("[PATCH] " + p)
if not patches:
    print("[PATCH] zaten uygulanmis.")
BPATCH_RUN
info "Binary patch tamamlandı."

# ════════════════════════════════════════════════════════════════
#  BÖLÜM 9 — Disk imajına runtime dosyaları kur
# ════════════════════════════════════════════════════════════════
if [ ! -f "${DISK_IMG}" ]; then
    warn "Disk imajı bulunamadı: ${DISK_IMG} — runtime kurulumu atlandı."
    warn "  → Önce 'make disk.img' ile imajı oluşturun."
else
    info "Disk dizinleri oluşturuluyor..."
    disk_mkdir "usr"
    disk_mkdir "usr/lib"
    disk_mkdir "usr/include"
    disk_mkdir "lib"
    disk_mkdir "bin"

    # ── AscentOS libc.a üret ──────────────────────────────────
    info "AscentOS libc.a üretiliyor (syscalls.c'den)..."
    ${TARGET}-gcc \
        -c \
        -static \
        -ffreestanding \
        -fno-stack-protector \
        -mno-red-zone \
        -mcmodel=small \
        -std=c99 \
        -I"${MUSL_PREFIX}/include" \
        -isystem "${GCC_INCLUDE}" \
        -Wno-implicit-function-declaration \
        -Wno-builtin-declaration-mismatch \
        -o /tmp/ascentos_syscalls_libc.o \
        "${SYSCALLS_C}" \
        || warn "syscalls.c derlenemedi, libc.a atlanıyor."

    # crt1_extras.o: __heap_start + environ sembolleri
    cat > /tmp/crt1_extras.asm << 'CRT1EX_EOF'
bits 64
section .bss
align 4096
global __heap_start
global environ
global __environ
__heap_start:
_heap_pad:    resb 0
environ:      resq 1
__environ:    equ environ
CRT1EX_EOF
    nasm -f elf64 -o /tmp/crt1_extras.o /tmp/crt1_extras.asm \
        && info "crt1_extras.o üretildi." \
        || warn "crt1_extras.o üretilemedi!"

    if [ -f /tmp/ascentos_syscalls_libc.o ]; then
        EXTRAS=""
        [ -f /tmp/crt1_extras.o ] && EXTRAS="/tmp/crt1_extras.o"
        ${TARGET}-ar rcs "${ASCENTOS_LIBC_A}" \
            /tmp/ascentos_syscalls_libc.o \
            ${EXTRAS} \
            && info "ascentos_libc.a oluşturuldu." \
            || warn "ar başarısız, libc.a atlanıyor."
    fi

    # ── crt1.o ───────────────────────────────────────────────
    info "crt1.o üretiliyor (crt0.asm'den)..."
    nasm -f elf64 -o /tmp/ascentos_crt1.o "${CRT0_ASM}"
    disk_write_file /tmp/ascentos_crt1.o "usr/lib/crt1.o"

    # ── crti.o stub ──────────────────────────────────────────
    cat > /tmp/crti_stub.s << 'CRTI_EOF'
section .text
global _init
global _fini
_init: ret
_fini: ret
CRTI_EOF
    nasm -f elf64 -o /tmp/ascentos_crti.o /tmp/crti_stub.s
    disk_write_file /tmp/ascentos_crti.o "usr/lib/crti.o"

    # ── crtn.o stub ──────────────────────────────────────────
    cat > /tmp/crtn_stub.s << 'CRTN_EOF'
section .text
CRTN_EOF
    nasm -f elf64 -o /tmp/ascentos_crtn.o /tmp/crtn_stub.s
    disk_write_file /tmp/ascentos_crtn.o "usr/lib/crtn.o"

    # ── libc.a (musl tam + syscalls.o birleşik) ──────────────
    # NEDEN: disk imajına sadece syscalls.o giderse TCC'nin
    # derlediği programlarda printf/malloc/free gibi musl sembolleri
    # bulunamaz ("undefined symbol 'printf'" hatası).
    # ÇÖZÜM: musl'ün tam libc.a'sını al, üstüne syscalls.o ekle.
    # Böylece TCC çalışma zamanında hem printf/malloc/free hem de
    # AscentOS'a özgü syscall wrapper'larını bulur.
    COMBINED_LIBC_A="/tmp/ascentos_combined_libc.a"
    if [ -f "${MUSL_PREFIX}/lib/libc.a" ]; then
        info "Disk için birleşik libc.a üretiliyor (musl + syscalls)..."
        cp "${MUSL_PREFIX}/lib/libc.a" "${COMBINED_LIBC_A}"

        # syscalls.o'yu ekle (AscentOS'a özgü wrapper'lar: sleep, yield, debug…)
        # __syscall zaten musl libc.a içinde var; syscalls.o çakışırsa
        # --allow-multiple-definition ile link edildiği için sorun olmaz.
        if [ -f "${SYSCALLS_OBJ}" ]; then
            ${TARGET}-ar rcs "${COMBINED_LIBC_A}" "${SYSCALLS_OBJ}" \
                && info "syscalls.o birleşik libc.a'ya eklendi." \
                || warn "syscalls.o eklenemedi, musl-only libc.a kullanılacak."
        fi

        # crt1_extras.o'yu da ekle (__heap_start + environ sembolleri)
        if [ -f /tmp/crt1_extras.o ]; then
            ${TARGET}-ar rcs "${COMBINED_LIBC_A}" /tmp/crt1_extras.o \
                && info "crt1_extras.o birleşik libc.a'ya eklendi." \
                || warn "crt1_extras.o eklenemedi."
        fi

        ${TARGET}-ranlib "${COMBINED_LIBC_A}"

        # Sembol doğrulama
        info "Birleşik libc.a sembol kontrolü..."
        for sym in printf malloc free realloc calloc sprintf fprintf; do
            COUNT=$(${TARGET}-nm "${COMBINED_LIBC_A}" 2>/dev/null | grep -c " T ${sym}$" || true)
            if [ "${COUNT}" -gt 0 ]; then
                info "  ✓ ${sym}"
            else
                warn "  ✗ ${sym} — birleşik libc.a'da bulunamadı!"
            fi
        done

        debugfs -w "${DISK_IMG}" -R "rm usr/lib/libc.a" 2>/dev/null || true
        disk_write_file "${COMBINED_LIBC_A}" "usr/lib/libc.a"
        info "Birleşik libc.a diske yazıldı (musl + AscentOS syscalls)."
    else
        warn "musl libc.a bulunamadı: ${MUSL_PREFIX}/lib/libc.a"
        warn "  → Önce './musl-build.sh' çalıştırın!"
        # Son çare: eski minimal libc.a
        if [ -f "${ASCENTOS_LIBC_A}" ]; then
            disk_write_file "${ASCENTOS_LIBC_A}" "usr/lib/libc.a"
            warn "Minimal AscentOS libc.a diske yazıldı (printf/malloc ÇALIŞMAZ)."
        fi
    fi

    # ── libtcc1.a ────────────────────────────────────────────
    info "libtcc1.a üretiliyor..."
    LIBTCC1_C="${TCC_SRC_DIR}/lib/libtcc1.c"
    if [ -f "${LIBTCC1_C}" ]; then
        ${TARGET}-gcc \
            -c -ffreestanding -fno-stack-protector -mno-red-zone \
            -mcmodel=small -std=c99 \
            -I"${TCC_SRC_DIR}" \
            -I"${MUSL_PREFIX}/include" \
            -isystem "${GCC_INCLUDE}" \
            -o /tmp/libtcc1.o "${LIBTCC1_C}" 2>/dev/null \
            && ${TARGET}-ar rcs /tmp/libtcc1.a /tmp/libtcc1.o \
            && info "libtcc1.a libtcc1.c'den üretildi." \
            || warn "libtcc1.c derlenemedi."
    fi
    if [ ! -f /tmp/libtcc1.a ] && [ -f "${LIBGCC}" ]; then
        cp "${LIBGCC}" /tmp/libtcc1.a
        info "libtcc1.a olarak libgcc.a kopyalandı."
    fi
    if [ -f /tmp/libtcc1.a ]; then
        disk_write_file /tmp/libtcc1.a "usr/lib/libtcc1.a"
        disk_write_file /tmp/libtcc1.a "lib/libtcc1.a"
        # TCC bazen root'ta da arar
        debugfs -w "${DISK_IMG}" -R "rm libtcc1.a" 2>/dev/null || true
        disk_write_file /tmp/libtcc1.a "libtcc1.a"
        info "libtcc1.a diske yazıldı (usr/lib, lib, /)."
    fi

    # ── libgcc_s.a boş stub ──────────────────────────────────
    echo '!<arch>' > /tmp/libgcc_s.a
    debugfs -w "${DISK_IMG}" -R "rm lib/libgcc_s.a"    2>/dev/null || true
    debugfs -w "${DISK_IMG}" -R "rm usr/lib/libgcc_s.a" 2>/dev/null || true
    disk_write_file /tmp/libgcc_s.a "lib/libgcc_s.a"
    disk_write_file /tmp/libgcc_s.a "usr/lib/libgcc_s.a"
    info "libgcc_s.a stub diske yazıldı."

    # ── tccdefs.h ────────────────────────────────────────────
    TCCDEFS_SRC=""
    for candidate in \
        "${TCC_SRC_DIR}/tccdefs.h" \
        "${TCC_SRC_DIR}/include/tccdefs.h" \
        "${TCC_SRC_DIR}/lib/tccdefs.h"; do
        [ -f "${candidate}" ] && { TCCDEFS_SRC="${candidate}"; break; }
    done

    if [ -z "${TCCDEFS_SRC}" ]; then
        warn "tccdefs.h kaynakta bulunamadı! Minimal stub oluşturuluyor..."
        cat > /tmp/tccdefs.h << 'TCCDEFS_EOF'
/* tccdefs.h — AscentOS minimal stub */
#ifndef _TCCDEFS_H
#define _TCCDEFS_H
typedef unsigned char  __u_char;
typedef unsigned short __u_short;
typedef unsigned int   __u_int;
typedef unsigned long  __u_long;
typedef signed char    __int8_t;
typedef unsigned char  __uint8_t;
typedef signed short   __int16_t;
typedef unsigned short __uint16_t;
typedef signed int     __int32_t;
typedef unsigned int   __uint32_t;
typedef signed long    __int64_t;
typedef unsigned long  __uint64_t;
typedef long           __intptr_t;
typedef unsigned long  __uintptr_t;
typedef unsigned long  __size_t;
typedef long           __ptrdiff_t;
#endif /* _TCCDEFS_H */
TCCDEFS_EOF
        TCCDEFS_SRC="/tmp/tccdefs.h"
    fi
    disk_write_file "${TCCDEFS_SRC}" "usr/include/tccdefs.h"

    # ── Tüm musl header'ları (tam ağaç) ──────────────────────
    # Seçici liste yerine tüm include dizini kopyalanıyor.
    # stdio.h → features.h → bits/alltypes.h gibi iç bağımlılıklar
    # bu sayede eksiksiz karşılanır.
    info "Musl header ağacı taranıyor: ${MUSL_PREFIX}/include"
    find "${MUSL_PREFIX}/include" -type f -name "*.h" | while read -r src; do
        # Göreceli yolu hesapla: toolchain/musl-install/include/foo/bar.h -> foo/bar.h
        rel="${src#${MUSL_PREFIX}/include/}"
        dst="usr/include/${rel}"
        subdir=$(dirname "${rel}")
        if [ "${subdir}" != "." ]; then
            disk_mkdir "usr/include/${subdir}"
        fi
        disk_write_file "${src}" "${dst}"
    done
    info "Musl header'ları kopyalandı."

    # ── test.c (hem libc hem syscall tabanlı) ────────────────
    info "test.c oluşturuluyor..."
    cat > /tmp/ascentos_test.c << 'TESTC_EOF'
/* AscentOS TCC test — libc (printf/malloc) + syscall doğrulama
 * Derleme: tcc.elf -o /bin/test.elf test.c
 * Çalıştır: /bin/test.elf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    /* 1. printf testi */
    printf("Merhaba AscentOS! TCC + musl libc calisiyor.\n");

    /* 2. malloc/free testi */
    char *buf = (char *)malloc(64);
    if (buf) {
        strcpy(buf, "malloc/free calisiyor");
        printf("malloc: %s\n", buf);
        free(buf);
    } else {
        printf("malloc BASARISIZ!\n");
    }

    /* 3. sprintf testi */
    char tmp[64];
    sprintf(tmp, "sprintf: 6 * 7 = %d\n", 6 * 7);
    printf("%s", tmp);

    return 0;
}
TESTC_EOF
    debugfs -w "${DISK_IMG}" -R "rm test.c" 2>/dev/null || true
    disk_write_file /tmp/ascentos_test.c "test.c"
    info "test.c diske yazıldı."

    # ── tcc.elf'i disk'e kopyala ─────────────────────────────
    info "tcc.elf diske kopyalanıyor: bin/tcc.elf"
    disk_write_file "${OUTPUT_DIR}/tcc.elf" "bin/tcc.elf"
fi

# ════════════════════════════════════════════════════════════════
#  Özet
# ════════════════════════════════════════════════════════════════
echo ""
echo -e "${GRN}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║   AscentOS TCC Kurulumu TAMAMLANDI                   ║${NC}"
echo -e "${GRN}╠══════════════════════════════════════════════════════╣${NC}"
echo -e "${GRN}║${NC}  Derleme çıktısı  : ${OUTPUT_DIR}/tcc.elf (${SIZE})"
echo -e "${GRN}║${NC}  TCC sürümü       : ${TCC_VERSION} (x86_64 backend)"
echo -e "${GRN}║${NC}  Kullanılan libc  : ${MUSL_PREFIX}"
if [ -f "${DISK_IMG}" ]; then
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Diske yazılan dosyalar:"
echo -e "${GRN}║${NC}    /bin/tcc.elf          — derleyici"
echo -e "${GRN}║${NC}    /usr/lib/crt1.o        — C runtime entry"
echo -e "${GRN}║${NC}    /usr/lib/crti.o        — constructor stub"
echo -e "${GRN}║${NC}    /usr/lib/crtn.o        — destructor stub"
echo -e "${GRN}║${NC}    /usr/lib/libc.a        — musl tam libc (printf/malloc/free dahil)"
echo -e "${GRN}║${NC}    /usr/lib/libtcc1.a     — TCC runtime yardımcıları"
echo -e "${GRN}║${NC}    /usr/include/tccdefs.h — TCC iç tanımlar"
echo -e "${GRN}║${NC}    /usr/include/*.h       — Temel musl header'ları"
echo -e "${GRN}║${NC}    /test.c                — Syscall tabanlı test programı"
fi
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Bilinen kısıtlamalar:"
echo -e "${GRN}║${NC}  • dlopen()   → stub; statik derleme zorunlu"
echo -e "${GRN}║${NC}  • tcc -run   → mmap(EXEC) yok; bellek çalıştırma kısıtlı"
echo -e "${GRN}║${NC}  • pthread    → yok (tek iş parçacıklı ortam)"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Temel kullanım:"
echo -e "${GRN}║${NC}    tcc.elf -o /bin/hello.elf hello.c          (libc otomatik)"
echo -e "${GRN}║${NC}    tcc.elf -I/usr/include -o prog.elf prog.c  (musl header)"
echo -e "${GRN}║${NC}    tcc.elf -nostdlib -o raw.elf /usr/lib/crt1.o raw.c  (syscall)"
echo -e "${GRN}╚══════════════════════════════════════════════════════╝${NC}"