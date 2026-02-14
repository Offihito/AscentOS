// elf64.c - ELF-64 Loader for AscentOS
// FAT32 üzerindeki ET_EXEC / ET_DYN (PIE) x86-64 ELF ikililerini
// belleğe yükler; harici dinamik linker gerektirmez.

#include "elf64.h"
#include "disk64.h"       // fat32_read_file, fat32_file_size
#include "../apps/commands64.h"         // CommandOutput, output_add_line, renk sabitleri

// ============================================================
// Kernel memory helpers (kernel64.c'den extern edilmiş)
// ============================================================
extern void* memset64(void* dest, int c, uint64_t n);
extern void* memcpy64(void* dest, const void* src, uint64_t n);

// ============================================================
// İç yardımcılar
// ============================================================

// En basit sayfa hizalaması: addr'ı 4096'ya yukarı yuvarla
static inline uint64_t page_align_up(uint64_t addr) {
    return (addr + 0xFFF) & ~(uint64_t)0xFFF;
}

// Güvenli dosya boyutu üst sınırı (FAT32_MAX_FILE_BYTES yerine
// bellek güvenliği için 16 MB ile sınırlıyoruz)
#define ELF_MAX_LOAD_SIZE (16u * 1024u * 1024u)

// ============================================================
// İnsan okunabilir hata dizisi
// ============================================================
const char* elf64_strerror(int err) {
    switch (err) {
        case ELF_OK:           return "Success";
        case ELF_ERR_NULL:     return "Null pointer";
        case ELF_ERR_MAGIC:    return "Bad ELF magic";
        case ELF_ERR_CLASS:    return "Not ELF64 (need ELFCLASS64)";
        case ELF_ERR_ENDIAN:   return "Not little-endian";
        case ELF_ERR_TYPE:     return "Not ET_EXEC or ET_DYN";
        case ELF_ERR_MACHINE:  return "Not x86-64";
        case ELF_ERR_NOPHDR:   return "No PT_LOAD segments found";
        case ELF_ERR_PHENTSIZE:return "Bad program header entry size";
        case ELF_ERR_TOOBIG:   return "Segment exceeds buffer";
        case ELF_ERR_NOMEM:    return "Memory allocation failed";
        default:               return "Unknown error";
    }
}

// ============================================================
// elf64_validate  — sadece başlık denetimi yapar
// ============================================================
int elf64_validate(const uint8_t* buf, uint32_t buf_size) {
    if (!buf) return ELF_ERR_NULL;

    // Minimum boyut: ELF başlığı (64 byte)
    if (buf_size < sizeof(Elf64Header)) return ELF_ERR_MAGIC;

    const Elf64Header* hdr = (const Elf64Header*)buf;

    // Magic number
    if (hdr->e_ident[EI_MAG0] != ELF_MAGIC0 ||
        hdr->e_ident[EI_MAG1] != ELF_MAGIC1 ||
        hdr->e_ident[EI_MAG2] != ELF_MAGIC2 ||
        hdr->e_ident[EI_MAG3] != ELF_MAGIC3)
        return ELF_ERR_MAGIC;

    // Sınıf: 64-bit
    if (hdr->e_ident[EI_CLASS] != ELFCLASS64)
        return ELF_ERR_CLASS;

    // Byte order: little-endian
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return ELF_ERR_ENDIAN;

    // Tip: çalıştırılabilir ya da paylaşımlı (PIE)
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN)
        return ELF_ERR_TYPE;

    // Mimari: x86-64
    if (hdr->e_machine != EM_X86_64)
        return ELF_ERR_MACHINE;

    // Program header entry boyutu
    if (hdr->e_phentsize != sizeof(Elf64Phdr))
        return ELF_ERR_PHENTSIZE;

    return ELF_OK;
}

// ============================================================
// elf64_load  — PT_LOAD segmentleri belleğe kopyalar
// ============================================================
int elf64_load(const uint8_t* buf, uint32_t buf_size,
               uint64_t load_base, ElfImage* out) {
    if (!buf || !out) return ELF_ERR_NULL;

    int rc = elf64_validate(buf, buf_size);
    if (rc != ELF_OK) return rc;

    const Elf64Header* hdr = (const Elf64Header*)buf;

    // Program header tablosunun dosya içindeki sınır denetimi
    uint64_t phdr_end = (uint64_t)hdr->e_phoff +
                        (uint64_t)hdr->e_phnum * sizeof(Elf64Phdr);
    if (phdr_end > buf_size) return ELF_ERR_MAGIC;

    // Yükleme tabanını belirle
    // ET_EXEC: ELF p_vaddr'leri mutlaktır, load_base ihmal edilir
    // ET_DYN : p_vaddr'ler load_base'e göre kaydırılır
    uint64_t base = (hdr->e_type == ET_DYN) ? load_base : 0;

    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;
    int      seg_count = 0;

    // İki geçiş: 1) sınırları bul  2) kopyala
    // Geçiş 1: vaddr aralığını hesapla
    for (int i = 0; i < hdr->e_phnum; i++) {
        const Elf64Phdr* ph = (const Elf64Phdr*)
            (buf + hdr->e_phoff + i * sizeof(Elf64Phdr));

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)       continue;

        uint64_t seg_start = base + ph->p_vaddr;
        uint64_t seg_end   = seg_start + ph->p_memsz;

        if (seg_start < min_vaddr) min_vaddr = seg_start;
        if (seg_end   > max_vaddr) max_vaddr = seg_end;
        seg_count++;
    }

    if (seg_count == 0) return ELF_ERR_NOPHDR;

    // Toplam bellek boyutu güvenlik sınırı
    if ((max_vaddr - min_vaddr) > ELF_MAX_LOAD_SIZE) return ELF_ERR_TOOBIG;

    // Geçiş 2: Segmentleri yükle
    for (int i = 0; i < hdr->e_phnum; i++) {
        const Elf64Phdr* ph = (const Elf64Phdr*)
            (buf + hdr->e_phoff + i * sizeof(Elf64Phdr));

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)       continue;

        uint64_t dest_va = base + ph->p_vaddr;

        // Dosya görüntüsü sınır denetimi
        if (ph->p_filesz > 0) {
            if (ph->p_offset + ph->p_filesz > buf_size)
                return ELF_ERR_TOOBIG;
        }

        // Hedef belleği sıfırla (BSS desteği için memsz boyutunda)
        memset64((void*)dest_va, 0, ph->p_memsz);

        // Dosya baytlarını kopyala (filesz <= memsz)
        if (ph->p_filesz > 0) {
            memcpy64((void*)dest_va,
                     buf + ph->p_offset,
                     ph->p_filesz);
        }
    }

    // Çıkış bilgilerini doldur
    out->load_base    = base;
    out->load_min     = min_vaddr;
    out->load_max     = max_vaddr;
    out->segment_count = seg_count;
    out->entry        = base + hdr->e_entry;

    return ELF_OK;
}

// ============================================================
// elf64_dump_header  — tanı amaçlı başlık bilgisi yazdırır
// ============================================================

// Küçük bir uint64 → hex yardımcısı (kernel64.c'deki versiyondan bağımsız)
static void u64_to_hex_str(uint64_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = hex[(val >> (60 - i * 4)) & 0xF];
    out[18] = '\0';
}

static void u32_to_dec_str(uint32_t val, char* out) {
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (val) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i--) out[j++] = tmp[i + 1]; // Not: i-- sonrası i == -1 olmaz, else için düzelt
    /* Yukarıdaki reverse döngü düzeltme: */
    // Tekrar doğru yaz:
    out[j] = '\0';
}

// Basitleştirilmiş int→string (reverse trick)
static void fmt_u32(uint32_t v, char* buf) {
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return; }
    char tmp[12]; int n=0;
    while(v){ tmp[n++]='0'+(v%10); v/=10; }
    for(int i=0;i<n;i++) buf[i]=tmp[n-1-i];
    buf[n]='\0';
}

void elf64_dump_header(const uint8_t* buf, void* cmd_output) {
    CommandOutput* out = (CommandOutput*)cmd_output;
    if (!buf || !out) return;

    if (buf[EI_MAG0] != ELF_MAGIC0) {
        output_add_line(out, "  [ELF] Not a valid ELF file", VGA_RED);
        return;
    }

    const Elf64Header* hdr = (const Elf64Header*)buf;
    char line[128];
    char tmp[24];

    // Tip
    output_add_line(out, "  [ELF] Header Info:", VGA_CYAN);

    // Sınıf
    str_cpy(line, "    Class     : ");
    str_concat(line, hdr->e_ident[EI_CLASS] == ELFCLASS64 ? "ELF64" : "ELF32 (unsupported)");
    output_add_line(out, line, VGA_WHITE);

    // Byte order
    str_cpy(line, "    Endian    : ");
    str_concat(line, hdr->e_ident[EI_DATA] == ELFDATA2LSB ? "Little-endian" : "Big-endian (unsupported)");
    output_add_line(out, line, VGA_WHITE);

    // Tip
    str_cpy(line, "    Type      : ");
    if      (hdr->e_type == ET_EXEC) str_concat(line, "ET_EXEC (Executable)");
    else if (hdr->e_type == ET_DYN)  str_concat(line, "ET_DYN (Shared/PIE)");
    else if (hdr->e_type == ET_REL)  str_concat(line, "ET_REL (Relocatable)");
    else                              str_concat(line, "Unknown");
    output_add_line(out, line, VGA_WHITE);

    // Mimari
    str_cpy(line, "    Machine   : ");
    str_concat(line, hdr->e_machine == EM_X86_64 ? "x86-64" : "Unknown");
    output_add_line(out, line, VGA_WHITE);

    // Entry point
    u64_to_hex_str(hdr->e_entry, tmp);
    str_cpy(line, "    Entry     : ");
    str_concat(line, tmp);
    output_add_line(out, line, VGA_YELLOW);

    // Program header sayısı
    fmt_u32(hdr->e_phnum, tmp);
    str_cpy(line, "    PHDRs     : ");
    str_concat(line, tmp);
    output_add_line(out, line, VGA_WHITE);

    // Section header sayısı
    fmt_u32(hdr->e_shnum, tmp);
    str_cpy(line, "    SHDRs     : ");
    str_concat(line, tmp);
    output_add_line(out, line, VGA_WHITE);
}

// ============================================================
// elf64_exec_from_fat32
// FAT32'den dosyayı okur, doğrular, yükler, rapor verir.
// ============================================================
int elf64_exec_from_fat32(const char* fat83_name,
                          uint64_t    load_base,
                          ElfImage*   out,
                          void*       cmd_output) {
    CommandOutput* cout = (CommandOutput*)cmd_output;

    if (!fat83_name || !out || !cout) return ELF_ERR_NULL;

    // --- 1. Dosya boyutunu öğren ---
    uint32_t fsize = fat32_file_size(fat83_name);
    if (fsize == 0) {
        output_add_line(cout, "  [ELF] File not found on FAT32", VGA_RED);
        return ELF_ERR_NULL;
    }
    if (fsize > ELF_MAX_LOAD_SIZE) {
        output_add_line(cout, "  [ELF] File too large (>16 MB)", VGA_RED);
        return ELF_ERR_TOOBIG;
    }

    // --- 2. Dosyayı oku ---
    // Basit statik tampon (kernel yığını sınırlı olduğundan PMM kullan)
    // Şimdilik 1 MB'a kadar olan ikilileri destekleyen statik tampon.
    // Gerçek çekirdek: pmm_alloc_pages ile dinamik tahsis yapılmalı.
    static uint8_t elf_read_buf[1u * 1024u * 1024u]; // 1 MB

    if (fsize > sizeof(elf_read_buf)) {
        output_add_line(cout, "  [ELF] File too large for read buffer", VGA_RED);
        return ELF_ERR_TOOBIG;
    }

    int n = fat32_read_file(fat83_name, elf_read_buf, fsize);
    if (n <= 0) {
        output_add_line(cout, "  [ELF] FAT32 read failed", VGA_RED);
        return ELF_ERR_NULL;
    }

    // --- 3. Başlık dökümü ---
    elf64_dump_header(elf_read_buf, cout);

    // --- 4. Doğrula ---
    int rc = elf64_validate(elf_read_buf, (uint32_t)n);
    if (rc != ELF_OK) {
        char line[96];
        str_cpy(line, "  [ELF] Validation error: ");
        str_concat(line, elf64_strerror(rc));
        output_add_line(cout, line, VGA_RED);
        return rc;
    }

    // --- 5. Yükle ---
    rc = elf64_load(elf_read_buf, (uint32_t)n, load_base, out);
    if (rc != ELF_OK) {
        char line[96];
        str_cpy(line, "  [ELF] Load error: ");
        str_concat(line, elf64_strerror(rc));
        output_add_line(cout, line, VGA_RED);
        return rc;
    }

    // --- 6. Başarı raporu ---
    char line[128];
    char tmp[24];

    output_add_line(cout, "  [ELF] Load successful!", VGA_GREEN);

    u64_to_hex_str(out->entry, tmp);
    str_cpy(line, "    Entry point  : "); str_concat(line, tmp);
    output_add_line(cout, line, VGA_YELLOW);

    u64_to_hex_str(out->load_min, tmp);
    str_cpy(line, "    Load min VA  : "); str_concat(line, tmp);
    output_add_line(cout, line, VGA_WHITE);

    u64_to_hex_str(out->load_max, tmp);
    str_cpy(line, "    Load max VA  : "); str_concat(line, tmp);
    output_add_line(cout, line, VGA_WHITE);

    fmt_u32((uint32_t)out->segment_count, tmp);
    str_cpy(line, "    Segments     : "); str_concat(line, tmp);
    output_add_line(cout, line, VGA_WHITE);

    // Yüklenen boyut (KB)
    uint32_t loaded_kb = (uint32_t)((out->load_max - out->load_min) / 1024);
    fmt_u32(loaded_kb, tmp);
    str_cpy(line, "    Mapped size  : "); str_concat(line, tmp);
    str_concat(line, " KB");
    output_add_line(cout, line, VGA_WHITE);

    // NOT: Gerçek çalıştırma için çekirdeğin yeni bir task oluşturması,
    // VMM ile kullanıcı-alanı sayfa tablosunu kurması ve
    // out->entry'ye bir iret/sysret ile geçiş yapması gerekir.
    output_add_empty_line(cout);
    output_add_line(cout, "  [ELF] Note: call task_create_from_elf() to run", VGA_DARK_GRAY);

    return ELF_OK;
}