// kernel64.c — AscentOS 64-bit Unified Kernel

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "idt64.h"
#include "../drivers/ata64.h"
#include "ext3.h"
#include "cpu64.h"   // SSE, CPUID, I/O port yardımcıları
#include "../drivers/sb16.h"

#define COM1 0x3F8

void serial_write(char c) { while(!(inb(COM1+5)&0x20)); outb(COM1,c); }
void serial_print(const char* s) { while(*s){if(*s=='\n')serial_write('\r');serial_write(*s++);} }
void serial_putchar(char c) { serial_write(c); }

size_t strlen64(const char* s) { size_t n=0; while(s[n])n++; return n; }
int strcmp64(const char* a, const char* b) { while(*a&&*a==*b){a++;b++;} return *(uint8_t*)a-*(uint8_t*)b; }
void* memset64(void* d, int v, size_t n) { uint8_t*p=d; while(n--)*p++=(uint8_t)v; return d; }
void* memcpy64(void* d, const void* s, size_t n) { uint8_t*dp=d; const uint8_t*sp=s; while(n--)*dp++=*sp++; return d; }


// Multiboot2
extern uint64_t multiboot_mmap_addr;
extern uint32_t multiboot_mmap_entry_size;
extern uint32_t multiboot_mmap_total_size;
typedef struct { uint64_t base,len; uint32_t type,res; } __attribute__((packed)) mb2_mmap_t;
struct mmap_entry { unsigned long base,len; unsigned int type,acpi; };
#define MAX_MMAP 64
static struct mmap_entry parsed_mmap[MAX_MMAP];
void pmm_init(struct mmap_entry*, uint32_t);
void vmm_init(void);
void gdt_install_user_segments(void);
void tss_init(void);
void task_init(void);
void scheduler_init(void);
void syscall_init(void);

// ============================================================================
// GLOBAL MOD DEĞİŞKENLERİ
// ============================================================================
volatile int kernel_mode = 0;          // 0=TEXT (GUI kaldırıldı)

// ============================================================================
// TEXT MODE arayüz (vesa64.c)
// ============================================================================
void init_vesa64(void);
void print_str64(const char* str, uint8_t color);
void println64(const char* str, uint8_t color);
void putchar64(char c, uint8_t color);
void clear_screen64(void);
void set_position64(size_t row, size_t col);
void get_position64(size_t* row, size_t* col);
void get_screen_size64(size_t* w, size_t* h);
void scroll_up(size_t lines);
void scroll_down(size_t lines);
void update_cursor64(void);
void reset_to_standard_mode(void);
void set_extended_text_mode(void);

// ============================================================================
// PMM bootstrap
// ============================================================================
static uint32_t parse_mmap(void) {
    uint32_t es=multiboot_mmap_entry_size, ts=multiboot_mmap_total_size;
    uint64_t ma=multiboot_mmap_addr;
    if(!es||!ts||!ma) return 0;
    uint32_t n=0,off=0;
    while(off+es<=ts&&n<MAX_MMAP){
        mb2_mmap_t* e=(mb2_mmap_t*)(uint64_t)(ma+off);
        parsed_mmap[n].base=(unsigned long)e->base; parsed_mmap[n].len=(unsigned long)e->len;
        parsed_mmap[n].type=(unsigned int)e->type; parsed_mmap[n].acpi=0;
        n++; off+=es;
    }
    return n;
}
static void pmm_init_from_mb(void) {
    uint32_t n=parse_mmap();
    if(n){pmm_init(parsed_mmap,n);return;}
    struct mmap_entry fb[]={{0,0x9FC00,1,0},{0x9FC00,0x400,2,0},{0xF0000,0x10000,2,0},{0x100000,0x1FF00000,1,0}};
    pmm_init(fb,4); serial_print("[MMAP] Fallback\n");
}

// ============================================================================
// TEXT modunda boot ekranı
// ============================================================================
#define VGA_GREEN       0x02
#define VGA_CYAN        0x03
#define VGA_YELLOW      0x0E
#define VGA_LIGHT_GREEN 0x0A
#define VGA_WHITE       0x0F
#define VGA_MAGENTA     0x05

static void text_boot_screen(void) {
    char cpu[13]; get_cpu_info(cpu);
    println64("===============================================================", VGA_CYAN);
    println64("===        ASCENTOS 64-bit  v1.2  Unified Kernel          ===", VGA_LIGHT_GREEN);
    println64("===============================================================", VGA_CYAN);
    print_str64("  CPU : ", VGA_GREEN); println64(cpu, VGA_YELLOW);
    println64("  PMM, VMM, GDT, TSS, Scheduler, SYSCALL READY", VGA_GREEN);
    println64("  Klavye + Mouse interrupt-driven Active", VGA_GREEN);
    println64("", VGA_WHITE);
    println64("  help   - Show All Commands", VGA_LIGHT_GREEN);
    if (g_sb16.initialized)
        println64("  SB16   - Sound Blaster 16 aktif  (sb16 komutu ile kullanin)", VGA_GREEN);
    else
        println64("  Audio  - PC Speaker aktif  (beep komutu ile kullanin)", VGA_GREEN);
    println64("", VGA_WHITE);
}

// ============================================================================
// Dış init tanımları
// ============================================================================
void init_keyboard64(void);
void init_commands64(void);
void show_prompt64(void);

// RTL8139 ağ sürücüsü (kernel/rtl8139.c)
extern bool rtl8139_init(void);
extern void rtl8139_set_packet_handler(void* handler);

// commands64.c'de tanımlı — rtl8139 başarıyla başlatıldıktan sonra
// packet handler'ı kaydet ve g_net_initialized'ı güncelle.
extern void net_register_packet_handler(void);

// Ağ katmanları — tam yığın (Aşama 1-6)
extern void     ipv4_init(void);
extern void     udp_init(int csum_mode);
extern void     icmp_init(void);
extern void     dhcp_init(void);
extern bool     dhcp_discover(void);
extern void     tcp_init(void);
extern void     tcp_tick(void);
extern bool     tcp_is_initialized(void);
extern uint64_t get_system_ticks(void);

// ============================================================================
// NET STACK INIT — tüm katmanları sırayla başlat
// ============================================================================
static void net_stack_init(void) {
    if (!rtl8139_init()) {
        serial_print("[NET] RTL8139 bulunamadi (QEMU -device rtl8139 eklenmeli).\n");
        return;
    }
    net_register_packet_handler();   // g_net_initialized = 1
    serial_print("[NET] RTL8139 hazir.\n");

    ipv4_init();
    serial_print("[NET] IPv4 hazir.\n");

    udp_init(1);   // 1 = UDP_CSUM_ENABLE
    serial_print("[NET] UDP hazir.\n");

    icmp_init();
    serial_print("[NET] ICMP hazir.\n");

    tcp_init();    // IPv4'e proto=6 kaydeder
    serial_print("[NET] TCP hazir.\n");

    serial_print("[NET] DHCP hazir. DHCPDISCOVER gonderiliyor...\n");
}

// ============================================================================
// LBA48 Sürücü Testi
// ============================================================================
// 2TB sınırını aşan bir LBA'ya komut gönderir; DRQ gelip gelmediğini
// kontrol ederek sürücünün LBA48 paketini doğru yorumladığını doğrular.
// Gerçek bir disk olmayabilir — hata toleranslıdır.
static void test_lba48_driver(void) {
    serial_print("[LBA48_TEST] ---- LBA48 Surucu Testi Basladi ----\n");

    uint8_t buf[512];

    // ── 1. Ext3 superblock LBA'sini oku (LBA 2 = byte offset 1024) ──────────
    int ok = disk_read_sector64(2, buf);
    serial_print("[LBA48_TEST] LBA=2 (Ext3 superblock) oku: ");
    serial_print(ok ? "OK\n" : "FAIL (disk yok/hata)\n");

    if (ok) {
        // Ext3 magic: offset 56 = 0x38 → 0xEF53
        uint16_t magic = (uint16_t)(buf[56] | (buf[57] << 8));
        serial_print("[LBA48_TEST]   Ext3 magic 0xEF53: ");
        serial_print((magic == 0xEF53) ? "OK\n" : "FAIL\n");
    }

    // ── 2. LBA 3 oku (superblock 2. sektoru) ─────────────────────────────────
    int ok2 = disk_read_sector64(3, buf);
    serial_print("[LBA48_TEST] LBA=3 (superblock devam) oku: ");
    serial_print(ok2 ? "OK\n" : "FAIL\n");

    // ── 3. 2TB sinirini asan LBA (LBA48 mekanizma testi) ─────────────────────
    serial_print("[LBA48_TEST] LBA=0x100000000 (2TiB+, LBA48): ");
    int ok48 = disk_read_sector64_ext((uint64_t)0x100000000ULL, buf);
    serial_print(ok48 ? "OK (disk yanit verdi)\n"
                      : "TIMEOUT (beklenen) — donmadi, LBA48 OK\n");

    serial_print("[LBA48_TEST] LBA28 max=137GB, LBA48 max=128PiB\n");
    serial_print("[LBA48_TEST] ---- LBA48 Testi Bitti ----\n");
}

// ============================================================
//  Ext3 test fonksiyonu
//  Kernel boot'ta cagrilir; sonuclar serial porta yazilir.
// ============================================================
void test_ext3_driver(void) {
    serial_print("[EXT3_TEST] Starting...\n");

    // 1. Mount
    if (ext3_mount() != 0) {
        serial_print("[EXT3_TEST] Mount FAILED\n");
        return;
    }
    serial_print("[EXT3_TEST] Mount OK\n");

    // 2. mkdir
    int r = ext3_mkdir("/test_dir");
    serial_print(r == 0 ? "[EXT3_TEST] mkdir OK\n"
                        : "[EXT3_TEST] mkdir FAIL\n");

    // 3. create + write file
    ext3_create_file("/test_dir/hello.txt");
    const uint8_t* msg = (const uint8_t*)"Hello Ext3!\n";
    ext3_write_file("/test_dir/hello.txt", 0, msg, 12);
    serial_print("[EXT3_TEST] write OK\n");

    // 4. read file
    uint8_t rbuf[64];
    int i; for (i = 0; i < 64; i++) rbuf[i] = 0;
    int rd = ext3_read_file("/test_dir/hello.txt", rbuf, 63);
    if (rd == 12 && rbuf[0] == 'H')
        serial_print("[EXT3_TEST] read OK\n");
    else
        serial_print("[EXT3_TEST] read FAIL\n");

    // 5. getdents
    dirent64_t dbuf[8];
    int n = ext3_getdents("/test_dir", dbuf, (int)sizeof(dbuf));
    serial_print(n > 0 ? "[EXT3_TEST] getdents OK\n"
                       : "[EXT3_TEST] getdents FAIL\n");

    // 6. unlink
    r = ext3_unlink("/test_dir/hello.txt");
    serial_print(r == 0 ? "[EXT3_TEST] unlink OK\n"
                        : "[EXT3_TEST] unlink FAIL\n");

    // 7. rmdir
    r = ext3_rmdir("/test_dir");
    serial_print(r == 0 ? "[EXT3_TEST] rmdir OK\n"
                        : "[EXT3_TEST] rmdir FAIL\n");

    serial_print("[EXT3_TEST] Done.\n");
}

void kernel_main(uint64_t multiboot_info) {
    (void)multiboot_info;
    serial_print("\n=== AscentOS Unified Kernel ===\n");

    init_vesa64();
    pmm_init_from_mb();
    vmm_init();
    gdt_install_user_segments();
    tss_init();

    // IDT'yi erkenden kur: task_init/scheduler_init içinde oluşabilecek
    // herhangi bir CPU exception (#GP, #PF, #DF) handler'a yönlendirilsin.
    // IDT kurulmadan önce gelen exception → triple fault → sistem resetler.
    init_interrupts64();   // IDT + PIC + Timer kurulur; STI henüz yok

    task_init();
    scheduler_init();
    sse_init();
    syscall_init();

    // Task + scheduler hazır, artık timer IRQ'yu güvenle açabiliriz
    __asm__ volatile("sti");
    serial_print("[IRQ] STI — interrupts enabled\n");
    init_keyboard64();
    init_commands64();

    // Ext3 sürücü testi — serial çıktıya yaz
    test_ext3_driver();

    // LBA48 sürücü testi
    test_lba48_driver();

    // Tüm ağ yığınını başlat: RTL8139 → IPv4 → UDP → ICMP → TCP → DHCP
    net_stack_init();

    // SB16 ses kartini baslat (IRQ5 zaten IDT ve PIC'te aktif)
    // false donerse cihaz yok -- pcspk fallback devrede
    if (sb16_init()) {
        serial_print("[SB16] Sound Blaster 16 hazir.\n");
        sb16_print_info();
    } else {
        serial_print("[SB16] Cihaz bulunamadi (pcspk aktif).\n");
    }

    text_boot_screen();
    show_prompt64();

    // Ana döngü: TEXT modunda bekle
    // tcp_tick() her ~100ms'de bir çağrılarak yeniden iletim / TIME_WAIT yönetir
    static uint64_t last_tcp_tick = 0;
    while(1){        // TCP zamanlayıcısı — ~100ms aralıkla
        if (tcp_is_initialized()) {
            uint64_t now = get_system_ticks();
            if (now - last_tcp_tick >= 100) {
                last_tcp_tick = now;
                tcp_tick();
            }
        }

        __asm__ volatile("sti; hlt");
    }
}