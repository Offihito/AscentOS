#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  AscentOS — Lua 5.4 Port Build Script (diske kopyalamalı)
#  Kullanım: chmod +x lua-build.sh && ./lua-build.sh
#
#  Kilo port scriptinden türetilmiştir.
#  musl-libc 1.2.5 + x86_64-elf toolchain gerektirir.
# ═══════════════════════════════════════════════════════════════

set -e

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GRN}[INFO]${NC} $*"; }
warn()  { echo -e "${YLW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*"; exit 1; }

# ── Lua sürümü ────────────────────────────────────────────────
LUA_VERSION="${LUA_VERSION:-5.4.7}"
LUA_MAJOR="5.4"
LUA_SRC_DIR="lua-${LUA_VERSION}"
LUA_TARBALL="${LUA_SRC_DIR}.tar.gz"
LUA_URL="https://www.lua.org/ftp/${LUA_TARBALL}"

# ── Yollar ────────────────────────────────────────────────────
TARGET="x86_64-elf"
SCRIPT_DIR="$(cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"            # projenin kök dizini (bir üst klasör)
DISK_IMG="${DISK_IMG:-${ROOT}/disk.img}"
MUSL_PREFIX="${ROOT}/toolchain/musl-install"
USER_LD="${ROOT}/userland/libc/user.ld"
CRT0_ASM="${ROOT}/userland/libc/crt0.asm"
OUTPUT_DIR="${ROOT}/userland/bin"
SYSCALLS_OBJ="${ROOT}/userland/out/syscalls.o"
LIBGCC=$(${TARGET}-gcc -m64 --print-libgcc-file-name 2>/dev/null)
GCC_INCLUDE=$(${TARGET}-gcc -m64 -print-file-name=include 2>/dev/null)

# ── Ön koşul kontrolü ─────────────────────────────────────────
info "Ön koşullar kontrol ediliyor..."
command -v ${TARGET}-gcc >/dev/null 2>&1 || error "${TARGET}-gcc bulunamadı. Önce toolchain kurulumunu yapın."
command -v nasm >/dev/null 2>&1         || error "nasm bulunamadı (sudo apt install nasm)."
command -v wget >/dev/null 2>&1 || command -v curl >/dev/null 2>&1 \
    || error "wget veya curl bulunamadı. İndirme için birine ihtiyaç var."
[ -d "${MUSL_PREFIX}" ] || error "musl kurulum dizini bulunamadı: ${MUSL_PREFIX}"
[ -f "${USER_LD}" ]     || error "Linker script (user.ld) bulunamadı: ${USER_LD}"
[ -f "${CRT0_ASM}" ]    || error "crt0.asm bulunamadı: ${CRT0_ASM}"
[ -f "${SYSCALLS_OBJ}" ] || error "syscalls.o bulunamadı: ${SYSCALLS_OBJ} — önce 'make userland/out/syscalls.o' çalıştırın."

info "Compiler: $(${TARGET}-gcc --version | head -1)"

# ── Lua kaynağını indir / kontrol et ──────────────────────────
if [ -d "${LUA_SRC_DIR}" ]; then
    info "Lua kaynak dizini zaten mevcut: ${LUA_SRC_DIR}"
else
    if [ ! -f "${LUA_TARBALL}" ]; then
        info "Lua ${LUA_VERSION} indiriliyor: ${LUA_URL}"
        if command -v wget >/dev/null 2>&1; then
            wget -q --show-progress "${LUA_URL}" -O "${LUA_TARBALL}" \
                || error "İndirme başarısız: ${LUA_URL}"
        else
            curl -L --progress-bar "${LUA_URL}" -o "${LUA_TARBALL}" \
                || error "İndirme başarısız: ${LUA_URL}"
        fi
    else
        info "Tarball zaten mevcut: ${LUA_TARBALL}"
    fi
    info "Çıkartılıyor: ${LUA_TARBALL}"
    tar -xzf "${LUA_TARBALL}"
fi

# ── Lua kaynak dosyalarına yamaları uygula ────────────────────
#
#  Lua 5.4, libc'nin tam POSIX desteğini varsayar.
#  AscentOS/musl ortamı için aşağıdaki yamalar gereklidir:
#
#  1. luaconf.h → LUA_USE_POSIX yerine LUA_USE_C89 kullan
#     (popen, tmpnam gibi POSIX fonksiyonları yok)
#  2. loslib.c  → os.execute() ve os.tmpname() stub'la
#     (execve/fork var ama popen/system istikrarsız olabilir)
#  3. liolib.c  → stdin/stdout/stderr'in isatty() kontrolü
#     (musl isatty() ioctl ile çalışır; AscentOS'ta genellikle OK)
#  4. linit.c   → isteğe bağlı modülleri kaldırma seçeneği
# ──────────────────────────────────────────────────────────────
info "Yamalar uygulanıyor..."
LUA_SRC="${LUA_SRC_DIR}/src"
cd "${LUA_SRC}"

# ── Önceki bozuk yama kalıntılarını temizle ───────────────────
# Eski çalıştırmada sed loslib.c veya luaconf.h'ı bozmuş olabilir.
# En eski .bak dosyasından orijinali geri yükle; sonra yeniden yamala.
OLDEST_LOS_BAK=$(ls loslib.c.bak.* 2>/dev/null | sort | head -1)
if [ -n "${OLDEST_LOS_BAK}" ]; then
    info "loslib.c orijinal yedekten geri yükleniyor: ${OLDEST_LOS_BAK}"
    cp "${OLDEST_LOS_BAK}" loslib.c
    rm -f loslib.c.bak.*
fi
OLDEST_CONF_BAK=$(ls luaconf.h.bak.* 2>/dev/null | sort | head -1)
if [ -n "${OLDEST_CONF_BAK}" ]; then
    info "luaconf.h orijinal yedekten geri yükleniyor: ${OLDEST_CONF_BAK}"
    cp "${OLDEST_CONF_BAK}" luaconf.h
    rm -f luaconf.h.bak.*
fi

# luaconf.h yaması: LUA_USE_LINUX yerine AscentOS-uyumlu konfigürasyon
if [ -f luaconf.h ]; then
    cp luaconf.h luaconf.h.bak.$(date +%s)

    # 1. LUA_USE_LINUX / LUA_USE_POSIX / LUA_USE_DLOPEN → devre dışı
    #    Bu tanımlar dlopen, readline, mkstemp gibi şeyleri aktive eder;
    #    AscentOS'ta bunların hiçbiri yok.
    sed -i 's/^#define LUA_USE_LINUX/\/\/ #define LUA_USE_LINUX  \/\* AscentOS: devre dışı *\//' luaconf.h
    sed -i 's/^#define LUA_USE_POSIX/\/\/ #define LUA_USE_POSIX  \/\* AscentOS: devre dışı *\//' luaconf.h
    sed -i 's/^#define LUA_USE_DLOPEN/\/\/ #define LUA_USE_DLOPEN \/\* AscentOS: dlopen yok *\//' luaconf.h

    # 2. Dosyanın en başına AscentOS platform bloğu ekle.
    #    Bu blok:
    #      - LUA_USE_C89   : system(), mkstemp() yerine ISO C yolunu seçer
    #      - l_system(cmd) : loslib.c'deki #if !defined(l_system) guard'ını
    #                        kullanarak system() çağrısını engeller.
    #                        NULL → 0 (shell kontrolü), diğer → -1 (hata)
    #      - lua_tmpnam    : LUA_USE_C89 ile tmpnam() ISO C yolunu kullanır;
    #                        /tmp yoksa os.tmpname() hata döner (güvenli).
    if ! grep -q "ASCENTOS_PLATFORM" luaconf.h; then
        cat > /tmp/ascent_patch.h << 'PATCH'
/* ── AscentOS platform tanımları ──────────────────────────────
 * Bu blok lua-build.sh tarafından otomatik eklendi.
 * LUA_USE_C89: ISO C 89 uyumluluk modu (en geniş taşınabilirlik)
 * l_system   : os.execute() için system() yerine stub
 *              (AscentOS'ta /bin/sh yok; fork+execve kullanın)
 * ──────────────────────────────────────────────────────────── */
#define ASCENTOS_PLATFORM  1
#define LUA_USE_C89        1

/* loslib.c: #if !defined(l_system) guard'ı bu tanımı görür ve
 * varsayılan system(cmd) çağrısını KULLANMAZ.
 * NULL → 0 (shell var mı? hayır), diğer → -1 (hata) */
#define l_system(cmd)   ((cmd) == NULL ? 0 : -1)

/* ──────────────────────────────────────────────────────────── */
PATCH
        # Orijinal luaconf.h'ın başına yapıştır
        cat /tmp/ascent_patch.h luaconf.h > /tmp/luaconf_patched.h
        mv /tmp/luaconf_patched.h luaconf.h
        rm -f /tmp/ascent_patch.h
    fi

    info "Yamalar uygulandı."
else
    error "src/luaconf.h bulunamadı! Lua kaynağı bozuk olabilir."
fi

cd - > /dev/null  # src/ dizininden çık
mkdir -p "${OUTPUT_DIR}"

# ── user.ld'yi patch'le: __heap_start sembolü ekle ────────────
#
#  syscalls.c sbrk() implementasyonu:
#    extern char __heap_start;
#    if (!_heap_ptr) _heap_ptr = &__heap_start;
#
#  Bu sembol user.ld'de tanımlanmamışsa linker hata verir.
#  Orijinal user.ld'yi kopyalayıp .bss bitiminden sonra
#  __heap_start sembolünü ekliyoruz.
# ──────────────────────────────────────────────────────────────
PATCHED_LD="${ROOT}/user_lua.ld"
info "user.ld kopyalanıyor ve __heap_start ekleniyor..."
cp "${USER_LD}" "${PATCHED_LD}"

# .bss sonrasına __heap_start ekle (zaten yoksa)
if ! grep -q "__heap_start" "${PATCHED_LD}"; then
    # "__bss_end = .;" satırından sonrasına ekle
    sed -i 's/__bss_end = \.;/__bss_end = .;\n        __heap_start = .;/' "${PATCHED_LD}"
    info "__heap_start sembolü user_lua.ld'ye eklendi."
else
    info "__heap_start zaten mevcut, atlandı."
fi

# Linker script değişkenini güncelle
USER_LD="${PATCHED_LD}"

# ── ascentos_compat.c oluştur ─────────────────────────────────
#
#  musl 1.2.5'in statik libc.a'sında bazı iç semboller
#  (locale, timezone) derleme konfigürasyonuna bağlı olarak
#  eksik olabilir. Lua bu sembolleri loslib.c (os.date/os.time)
#  ve lstrlib.c (string.format %c/%x) üzerinden kullanır.
#
#  Çözüm: Eksik sembolleri stub olarak sağlayan ascentos_compat.c
#  dosyasını oluşturup derlemeye ekle.
# ──────────────────────────────────────────────────────────────
info "ascentos_compat.c oluşturuluyor (musl stub'ları + güvenli allocator)..."
cat > ascentos_compat.c << 'COMPAT_EOF'
/*
 * ascentos_compat.c — AscentOS Lua 5.4 uyum katmanı
 *
 * 1. LOCALE/TIMEZONE STUB'LARI
 * 2. LUA ALLOCATOR — sbrk tabanlı basit bump allocator
 *    syscalls.c'deki sbrk() kullanılır (BSS'ten __heap_start)
 *    mmap/kernel heap kullanılmaz → mcmodel=small uyumlu adresler
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"

/* ── Locale/timezone stub'ları ─────────────────────────────── */
void __locale_lock(void)   {}
void __locale_unlock(void) {}
void* __get_locale(void)   { return NULL; }

typedef struct { int off; const char *name; long long transition; } _tz_rule;
const _tz_rule __utc = { 0, "UTC", 0 };

void __secs_to_zone(long long t, int local,
                    int *isdst, long *offset, long *oppoff,
                    const char **zonename)
{
    (void)t; (void)local;
    if (isdst)    *isdst    = 0;
    if (offset)   *offset   = 0;
    if (oppoff)   *oppoff   = 0;
    if (zonename) *zonename = "UTC";
}

const char *__tm_to_tzname(const struct tm *tm) { (void)tm; return "UTC"; }

/* ── Lua allocator: sbrk tabanlı ───────────────────────────────
 * syscalls.c'deki sbrk() BSS sonrası __heap_start'tan çalışır.
 * Bu adresler mcmodel=small (<2GB) aralığında kalır.
 * mmap veya kernel heap KULLANILMAZ.
 */
extern void *sbrk(long incr);

#define ALIGN16(n)  (((n) + 15UL) & ~15UL)
#define HDR_MAGIC   0xCAFEBABEu
#define HDR_FREE    0xDEADBEEFu

typedef struct { unsigned magic; unsigned size; } hdr_t;
#define HDR_SZ sizeof(hdr_t)

/* Toplam heap: sbrk(CHUNK) ile büyütülür */
#define HEAP_CHUNK  (1024 * 1024)   /* 1MB adımlarla büyü */

static char *_brk_base = 0;
static char *_brk_ptr  = 0;
static char *_brk_end  = 0;

static void *lua_bump_alloc(unsigned sz) {
    sz = (unsigned)ALIGN16(sz);
    unsigned total = sz + (unsigned)HDR_SZ;
    /* Yeterli alan yoksa sbrk ile genişlet */
    while (!_brk_base || (_brk_ptr + total > _brk_end)) {
        unsigned grow = total > HEAP_CHUNK ? total : HEAP_CHUNK;
        if (!_brk_base) {
            _brk_base = (char *)sbrk(0);   /* mevcut break */
            if (_brk_base == (char *)-1) return NULL;
        }
        char *got = (char *)sbrk((long)grow);
        if (got == (char *)-1) return NULL;
        if (!_brk_ptr) _brk_ptr = _brk_base;
        _brk_end = _brk_base + (size_t)(got - _brk_base) + grow;
    }
    hdr_t *h = (hdr_t *)_brk_ptr;
    h->magic = HDR_MAGIC;
    h->size  = sz;
    _brk_ptr += total;
    return (char *)h + HDR_SZ;
}

/* Lua allocator hook */
void *ascent_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    if (nsize == 0) {
        if (ptr) {
            hdr_t *h = (hdr_t *)((char *)ptr - HDR_SZ);
            if (h->magic == HDR_MAGIC) h->magic = HDR_FREE;
        }
        return NULL;
    }
    if (ptr == NULL)
        return lua_bump_alloc((unsigned)nsize);

    /* realloc */
    void *newp = lua_bump_alloc((unsigned)nsize);
    if (!newp) return NULL;
    size_t copy = osize < nsize ? osize : nsize;
    memcpy(newp, ptr, copy);
    hdr_t *h = (hdr_t *)((char *)ptr - HDR_SZ);
    if (h->magic == HDR_MAGIC) h->magic = HDR_FREE;
    return newp;
}

/* __wrap_lua_newstate: her zaman kendi allocator'ımızı kullan */
extern lua_State *__real_lua_newstate(lua_Alloc f, void *ud);

lua_State *__wrap_lua_newstate(lua_Alloc f, void *ud)
{
    (void)f; (void)ud;
    return __real_lua_newstate(ascent_lua_alloc, NULL);
}
COMPAT_EOF
info "ascentos_compat.c oluşturuldu."

# ── ascentos_lua_defs.h oluştur ───────────────────────────────
# gcc -include ile her Lua .c dosyasına zorla enjekte edilir.
# luaconf.h'taki patch, lprefix.h üzerinden geldiği için
# bazı dosyalarda geç görülebilir. Bu header en erken noktada
# l_system ve gerekli guard'ları sağlar.
info "ascentos_lua_defs.h oluşturuluyor..."
cat > ascentos_lua_defs.h << 'DEFS_EOF'
/* ascentos_lua_defs.h — gcc -include ile tüm Lua kaynaklarına eklenir */
#ifndef ASCENTOS_LUA_DEFS_H
#define ASCENTOS_LUA_DEFS_H

/* system() yok; l_system stub — loslib.c guard'ını tetikler */
#ifndef l_system
#define l_system(cmd)   ((cmd) == NULL ? 0 : -1)
#endif

/* LUA_USE_C89: readline, mkstemp, dlopen'sız mod */
#ifndef LUA_USE_C89
#define LUA_USE_C89 1
#endif

#endif /* ASCENTOS_LUA_DEFS_H */
DEFS_EOF
info "ascentos_lua_defs.h oluşturuldu."

# ── crt0.asm'yi derle ─────────────────────────────────────────
info "crt0.asm derleniyor..."
nasm -f elf64 -o crt0.o "${CRT0_ASM}"

# ── Lua kaynak dosyalarını listele ────────────────────────────
#
#  lua.c   → interpreter (main)
#  luac.c  → compiler   (ayrı binary; bu scriptte derlenmez)
#  Geri kalan tüm .c dosyaları → core kütüphane
# ──────────────────────────────────────────────────────────────
LUA_CORE_SRCS=""
for f in "${LUA_SRC_DIR}"/src/*.c; do
    base=$(basename "$f")
    # Sadece lua.c (interpreter main) al; luac.c'yi dışla
    [ "$base" = "luac.c" ] && continue
    LUA_CORE_SRCS="${LUA_CORE_SRCS} $f"
done

info "Derlenecek kaynak dosyaları:"
for f in $LUA_CORE_SRCS; do echo "  $(basename $f)"; done

# ── Derleme ───────────────────────────────────────────────────
info "Derleme başlıyor..."

# ascentos_compat.c ayrı derlenir (Lua header'larına ihtiyacı var)
${TARGET}-gcc \
    -c \
    -static \
    -ffreestanding \
    -fno-stack-protector \
    -mno-red-zone \
    -mcmodel=small \
    -std=c99 \
    -I"${LUA_SRC_DIR}/src" \
    -I"${MUSL_PREFIX}/include" \
    -isystem "${GCC_INCLUDE}" \
    -Wno-implicit-function-declaration \
    -o ascentos_compat.o \
    ascentos_compat.c

info "ascentos_compat.o derlendi."

# Ana derleme
${TARGET}-gcc \
    -static \
    -nostdlib \
    -ffreestanding \
    -fno-stack-protector \
    -mno-red-zone \
    -mcmodel=small \
    -ffunction-sections \
    -fdata-sections \
    -std=c99 \
    -DASCENTOS_PLATFORM \
    -DLUA_COMPAT_5_3 \
    -include "${PWD}/ascentos_lua_defs.h" \
    -I"${LUA_SRC_DIR}/src" \
    -I"${MUSL_PREFIX}/include" \
    -isystem "${GCC_INCLUDE}" \
    -Wno-implicit-function-declaration \
    -Wno-builtin-declaration-mismatch \
    -Wno-deprecated-declarations \
    -fno-builtin-malloc -fno-builtin-free \
    -L"${MUSL_PREFIX}/lib" \
    -T "${USER_LD}" \
    -Wl,--gc-sections \
    -Wl,--allow-multiple-definition \
    -Wl,--wrap=lua_newstate \
    -o "${OUTPUT_DIR}/lua.elf" \
    crt0.o \
    ascentos_compat.o \
    ${LUA_CORE_SRCS} \
    "${SYSCALLS_OBJ}" \
    -lc \
    "${LIBGCC}"

# ── Doğrulama ─────────────────────────────────────────────────
if [ -f "${OUTPUT_DIR}/lua.elf" ]; then
    SIZE=$(du -sh "${OUTPUT_DIR}/lua.elf" | cut -f1)
    info "Lua başarıyla derlendi: ${OUTPUT_DIR}/lua.elf (${SIZE})"
else
    error "lua.elf oluşturulamadı!"
fi

# ── Diske kopyalama (Ext2 — Makefile ile aynı yöntem) ────────
if [ -f "${DISK_IMG}" ]; then
    info "Diske kopyalanıyor: ${OUTPUT_DIR}/lua.elf -> ${DISK_IMG}:/bin/lua.elf"
    if command -v debugfs >/dev/null 2>&1; then
        info "  → debugfs kullanılıyor (root gerekmez)"
        debugfs -w "${DISK_IMG}" -R "write ${OUTPUT_DIR}/lua.elf bin/lua.elf" 2>/dev/null \
            && info "Kopyalama başarılı (debugfs)." \
            || warn "debugfs kopyalama başarısız!"
    elif command -v e2cp >/dev/null 2>&1; then
        info "  → e2cp kullanılıyor"
        e2cp "${OUTPUT_DIR}/lua.elf" "${DISK_IMG}:/bin/lua.elf" \
            && info "Kopyalama başarılı (e2cp)." \
            || warn "e2cp kopyalama başarısız!"
    else
        info "  → loop mount deneniyor (sudo gerekebilir)"
        MNT_TMP="/tmp/ascentos_mnt"
        mkdir -p "${MNT_TMP}"
        sudo mount -o loop "${DISK_IMG}" "${MNT_TMP}" \
            && sudo cp "${OUTPUT_DIR}/lua.elf" "${MNT_TMP}/bin/lua.elf" \
            && sudo umount "${MNT_TMP}" \
            && rmdir "${MNT_TMP}" \
            && info "Kopyalama başarılı (loop mount)." \
            || { warn "loop mount kopyalama başarısız!"; sudo umount "${MNT_TMP}" 2>/dev/null; rmdir "${MNT_TMP}" 2>/dev/null; }
    fi
else
    warn "Disk imajı bulunamadı: ${DISK_IMG}. Kopyalama atlandı."
    warn "  → Önce 'make disk.img' ile imajı oluşturun."
fi

# ── Özet ──────────────────────────────────────────────────────
echo ""
echo -e "${GRN}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GRN}║   Lua ${LUA_VERSION} AscentOS port BAŞARILI                 ║${NC}"
echo -e "${GRN}╠══════════════════════════════════════════════════════╣${NC}"
echo -e "${GRN}║${NC}  Çıktı          : ${OUTPUT_DIR}/lua.elf"
echo -e "${GRN}║${NC}  Boyut          : ${SIZE}"
echo -e "${GRN}║${NC}  Lua sürümü     : ${LUA_VERSION} (C89 uyumluluk modu)"
echo -e "${GRN}║${NC}  Kullanılan libc: musl ${MUSL_PREFIX}"
echo -e "${GRN}║${NC}  crt0           : ${CRT0_ASM}"
if [ -f "${DISK_IMG}" ]; then
    echo -e "${GRN}║${NC}  Diske kopyalandı: ${DISK_IMG}:/bin/lua.elf (Ext2)"
fi
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Bilinen kısıtlamalar:"
echo -e "${GRN}║${NC}  • os.execute() → system() yok; fork+execve ile kullanın"
echo -e "${GRN}║${NC}  • require() → dinamik linkleme yok (statik derleme)"
echo -e "${GRN}║${NC}  • io.popen()  → pipe() var ama shell yok; manuel fork gerek"
echo -e "${GRN}║${NC}  • os.tmpname()→ /tmp dizini gerekir (FAT32'de oluşturun)"
echo -e "${GRN}║${NC}  • math.*      → musl 1.2.5 tüm C99 math fonksiyonları OK"
echo -e "${GRN}║${NC}"
echo -e "${GRN}║${NC}  Kullanım: lua.elf script.lua"
echo -e "${GRN}║${NC}           lua.elf          (REPL modunda)"
echo -e "${GRN}╚══════════════════════════════════════════════════════╝${NC}"