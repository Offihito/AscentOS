// elf64.c - ELF-64 Loader for AscentOS
// Ext3 üzerindeki ET_EXEC / ET_DYN (PIE) x86-64 ELF ikililerini
// belleğe yükler; harici dinamik linker gerektirmez.

#include "elf64.h"
#include "ext3.h"   // ext3_read_file, ext3_file_size
#include "../commands/commands64.h"         // CommandOutput, output_add_line, renk sabitleri

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

// Güvenli dosya boyutu üst sınırı (bellek güvenliği için 64 MB ile sınırlı)
#define ELF_MAX_LOAD_SIZE (64u * 1024u * 1024u)  // 64 MB — PMM destekli yükleme için yeterli

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

    // User-space sayfa tablosu girdilerini temizle (PML4[0..255]).
    // Önceki task'ın map'leri geçersiz/kirli fiziksel frame'lere işaret eder.
    // Bu olmadan vmm_get_physical_address eski bozuk adresleri döndürür
    // ve memset64 yanlış frame'e yazar → BSS sıfırlanmaz → #UD panic.
    {
        extern void vmm_flush_tlb_all(void);
        // CR3'ü doğrudan oku (vmm_read_cr3 static inline, extern erişilemiyor)
        // Sanal adres = phys + (0xFFFFFFFF80000000 - 0x100000)
        #define KERNEL_VMA_OFF  (0xFFFFFFFF80000000ULL - 0x100000ULL)
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        uint64_t* pml4 = (uint64_t*)(cr3 + KERNEL_VMA_OFF);
        for (int _i = 0; _i < 256; _i++) {
            pml4[_i] = 0;
        }
        vmm_flush_tlb_all();
    }

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
    // Her segment için önce fiziksel sayfaları tahsis et, sonra yaz.
    // Bu olmadan memset64/memcpy64 sayfa tablosunda karşılığı olmayan
    // adreslere yazar → sessiz başarısızlık → BSS sıfırlanmaz → ikinci
    // çalışmada kirli veriler kalır → RIP=0x2D (#UD).
    {
        extern void* pmm_alloc_frame(void);
        extern int   vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
        const uint64_t USER_RW = 0x7ULL; // PRESENT|WRITE|USER
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

        // Segment için fiziksel sayfaları tahsis et ve map et.
        // Sonra sanal adres üzerinden sıfırla + dosyadan kopyala.
        // PML4[0..255] az önce temizlendi ve vmm_map_page yeni sayfa tablosu
        // oluşturacak — bu sayede sanal adres geçerli hale gelir.
        {
            extern uint64_t vmm_get_physical_address(uint64_t virt);
            uint64_t pg     = dest_va & ~(uint64_t)0xFFF;
            uint64_t pg_end = (dest_va + ph->p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
            while (pg < pg_end) {
                uint64_t phys = vmm_get_physical_address(pg);
                if (!phys) {
                    void* frame = pmm_alloc_frame();
                    if (!frame) break;
                    phys = (uint64_t)frame;
                }
                // Her durumda yeniden map et (USER flag + TLB flush)
                vmm_map_page(pg, phys, USER_RW);
                pg += 0x1000;
            }
        }

        // Hedef belleği sıfırla (BSS + kirli frame temizliği için).
        // vmm_map_page sonrası sanal adres geçerli — doğrudan yaz.
        memset64((void*)dest_va, 0, ph->p_memsz);
        // DEBUG: verify memset worked at 0xC06840 offset if in range
        if (dest_va <= 0xC06840ULL && (dest_va + ph->p_memsz) > 0xC06960ULL) {
            volatile uint8_t* check = (volatile uint8_t*)0xC06960ULL;
            extern void serial_print(const char*);
            serial_print("[ELF] CHECK 0xC06960=");
            {
                const char* h = "0123456789ABCDEF";
                char buf[4]={'0','x',h[(*check)>>4],h[(*check)&0xF]};
                buf[3]='\0'; // hack: only 1 byte
                // print as hex byte
                char b2[3]={h[(*check)>>4],h[(*check)&0xF],'\0'};
                serial_print(b2);
            }
            serial_print("\n");
        }

        // Dosya baytlarını kopyala (filesz <= memsz)
        if (ph->p_filesz > 0) {
            memcpy64((void*)dest_va,
                     buf + ph->p_offset,
                     ph->p_filesz);
        }
    }
    }

    // Çıkış bilgilerini doldur
    out->load_base    = base;
    out->load_min     = min_vaddr;
    out->load_max     = max_vaddr;
    out->segment_count = seg_count;
    out->entry        = base + hdr->e_entry;

    // Sayfa tahsisi ve izin güncellemesi artık yukarıdaki
    // segment döngüsünde yapılıyor (pmm_alloc_frame + vmm_map_page).

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

/* u32_to_dec_str kaldırıldı — fmt_u32 kullanılıyor */

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
// elf64_exec_from_ext3
// Ext3'den dosyayı okur, doğrular, yükler, rapor verir.
// path: tam ext3 yolu, örn. "/bin/hello.elf"
// ============================================================
int elf64_exec_from_ext3(const char* path,
                         uint64_t    load_base,
                         ElfImage*   out,
                         void*       cmd_output) {
    CommandOutput* cout = (CommandOutput*)cmd_output;

    if (!path || !out || !cout) return ELF_ERR_NULL;

    // --- 1. Dosya boyutunu öğren ---
    uint32_t fsize = ext3_file_size(path);
    if (fsize == 0) {
        output_add_line(cout, "  [ELF] File not found on ext3", VGA_RED);
        return ELF_ERR_NULL;
    }
    if (fsize > ELF_MAX_LOAD_SIZE) {
        output_add_line(cout, "  [ELF] File too large (>64 MB)", VGA_RED);
        return ELF_ERR_TOOBIG;
    }

    // --- 2. Dosyayı oku ---
    // Heap'ten al: static buffer kernel .bss'te sabit adres kaplar ve
    // yüklenen ELF segment'leriyle fiziksel adres çakışmasına yol açar.
    #define ELF_READ_BUF_MAX (16u * 1024u * 1024u)
    if (fsize > ELF_READ_BUF_MAX) {
        char sz_line[96];
        str_cpy(sz_line, "  [ELF] File too large (");
        char sz_tmp[16]; fmt_u32(fsize / 1024, sz_tmp);
        str_concat(sz_line, sz_tmp);
        str_concat(sz_line, " KB > 16384 KB)");
        output_add_line(cout, sz_line, VGA_RED);
        return ELF_ERR_TOOBIG;
    }
    extern void* kmalloc(uint64_t size);
    extern void  kfree(void* ptr);
    uint8_t* elf_read_buf = (uint8_t*)kmalloc((uint64_t)fsize);
    if (!elf_read_buf) {
        output_add_line(cout, "  [ELF] kmalloc failed for read buffer", VGA_RED);
        return ELF_ERR_NOMEM;
    }

    int n = ext3_read_file(path, elf_read_buf, fsize);

    // --- DEBUG: kaç byte okundu? ---
    {
        char dbg[96]; char t1[16]; char t2[16];
        fmt_u32(fsize, t1); fmt_u32((uint32_t)(n < 0 ? 0 : n), t2);
        str_cpy(dbg, "  [ELF] ext3 fsize="); str_concat(dbg, t1);
        str_concat(dbg, " read="); str_concat(dbg, t2);
        output_add_line(cout, dbg, VGA_YELLOW);
    }

    if (n <= 0) {
        output_add_line(cout, "  [ELF] ext3 read failed", VGA_RED);
        kfree(elf_read_buf);
        return ELF_ERR_NULL;
    }

    // Okunan byte fsize'dan az gelirse buf_size olarak fsize kullan
    // (ext3 driver kısa okursa segment offset kontrolü yanlış patlıyor)
    // Sparse bloklar sifir dolu gelir; effective_size = fsize
    uint32_t effective_size = fsize;
    (void)n;

    // --- 3. Başlık dökümü ---
    elf64_dump_header(elf_read_buf, cout);

    // --- 4. Doğrula ---
    // buf_size olarak fsize kullan: segment offset'leri dosya boyutuna göre kontrol edilmeli
    int rc = elf64_validate(elf_read_buf, fsize);
    if (rc != ELF_OK) {
        char line[96];
        str_cpy(line, "  [ELF] Validation error: ");
        str_concat(line, elf64_strerror(rc));
        output_add_line(cout, line, VGA_RED);
        kfree(elf_read_buf);
        return rc;
    }

    // --- 5. Yükle ---
    rc = elf64_load(elf_read_buf, fsize, load_base, out);
    if (rc != ELF_OK) {
        char line[96];
        str_cpy(line, "  [ELF] Load error: ");
        str_concat(line, elf64_strerror(rc));
        output_add_line(cout, line, VGA_RED);
        kfree(elf_read_buf);
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

    kfree(elf_read_buf);
    return ELF_OK;
}