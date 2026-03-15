// rtl8139.c — AscentOS RTL8139 Ağ Kartı Sürücüsü
// Aşama 1: PCI tarama + kart başlatma + TX/RX (interrupt-driven)
//
// Derleme:
//   Makefile'a ekle:
//     rtl8139.o: kernel/rtl8139.c kernel/rtl8139.h
//         $(CC) $(CFLAGS) -c kernel/rtl8139.c -o rtl8139.o
//   KERNEL_OBJS listesine rtl8139.o ekle.
//
// interrupts64.asm'e eklenecek stub (açıklaması aşağıda):
//   isr_net: aynı isr_keyboard kalıbında, rtl8139_irq_handler çağırır,
//            PIC EOI: slave (0xA0) + master (0x20) gönderir (IRQ11 = slave).

#include "rtl8139.h"

// ============================================================================
// Kernel yardımcıları (kernel64.c'de tanımlı)
// ============================================================================
extern void serial_print(const char*);
extern void serial_write(char);

// ============================================================================
// I/O port işlemleri
// ============================================================================
static inline void     outb(uint16_t p, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void     outw(uint16_t p, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void     outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  inb (uint16_t p){ uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw (uint16_t p){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t inl (uint16_t p){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

// ============================================================================
// Küçük yardımcılar (stdib/string yok, kernel freestanding)
// ============================================================================
static void memcpy_drv(void* d, const void* s, uint32_t n){
    uint8_t* dp=(uint8_t*)d; const uint8_t* sp=(const uint8_t*)s;
    while(n--) *dp++=*sp++;
}

// ============================================================================
// Serial hex/int yardımcıları (debug çıktısı için)
// ============================================================================
static void serial_hex8(uint8_t v){
    const char* h="0123456789ABCDEF";
    serial_write(h[(v>>4)&0xF]); serial_write(h[v&0xF]);
}
static void serial_hex16(uint16_t v){
    serial_write('0'); serial_write('x');
    serial_hex8((v>>8)&0xFF); serial_hex8(v&0xFF);
}
static void serial_hex32(uint32_t v){
    serial_write('0'); serial_write('x');
    serial_hex8((v>>24)&0xFF); serial_hex8((v>>16)&0xFF);
    serial_hex8((v>>8)&0xFF);  serial_hex8(v&0xFF);
}
static void serial_dec(uint32_t v){
    if(v==0){ serial_write('0'); return; }
    char buf[12]; int i=0;
    while(v){ buf[i++]='0'+(v%10); v/=10; }
    while(i--) serial_write(buf[i]);
}

// ============================================================================
// Global sürücü nesnesi
// ============================================================================
static RTL8139 g_rtl;

// ============================================================================
// PCI yardımcıları
// ============================================================================

// PCI konfig alanı okuma (32-bit)
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    uint32_t addr = (1u<<31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

// PCI konfig alanı yazma (32-bit)
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val){
    uint32_t addr = (1u<<31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

// PCI Bus Master + I/O Space enable
static void pci_enable_busmaster(uint8_t bus, uint8_t dev, uint8_t fn){
    uint32_t cmd = pci_read32(bus, dev, fn, 0x04);
    cmd |= (1<<0) | (1<<2); // I/O Space | Bus Master
    pci_write32(bus, dev, fn, 0x04, cmd);
}

// RTL8139'u PCI bus'ta ara: 0..255 bus, 0..31 cihaz
// Bulunursa bus/dev/fn doldurulur, true döner.
static bool pci_find_rtl8139(uint8_t* out_bus, uint8_t* out_dev, uint8_t* out_fn,
                               uint16_t* out_iobase, uint8_t* out_irq){
    for(uint16_t bus=0; bus<256; bus++){
        for(uint8_t dev=0; dev<32; dev++){
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, 0x00);
            if(id == 0xFFFFFFFF) continue;           // Cihaz yok
            uint16_t vid = id & 0xFFFF;
            uint16_t did = (id >> 16) & 0xFFFF;
            if(vid == RTL_VENDOR_ID && did == RTL_DEVICE_ID){
                *out_bus = (uint8_t)bus;
                *out_dev = dev;
                *out_fn  = 0;
                // BAR0 = I/O port tabanı (bit0=1 → I/O space)
                uint32_t bar0 = pci_read32((uint8_t)bus, dev, 0, 0x10);
                *out_iobase = (uint16_t)(bar0 & 0xFFFC);
                // IRQ
                uint32_t irq_line = pci_read32((uint8_t)bus, dev, 0, 0x3C);
                *out_irq = (uint8_t)(irq_line & 0xFF);
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// Küçük gecikme (I/O port üzerinden, PIT gerektirmez)
// ============================================================================
static void io_delay(void){
    // Port 0x80 = POST kod portu, genelde mevcuttur ve güvenlidir
    for(int i=0; i<100; i++) outb(0x80, 0);
}

// ============================================================================
// RTL8139 yazmaç erişimleri
// ============================================================================
static inline uint8_t  rtl_r8 (uint16_t reg){ return inb (g_rtl.io_base + reg); }
static inline uint16_t rtl_r16(uint16_t reg){ return inw (g_rtl.io_base + reg); }
static inline uint32_t rtl_r32(uint16_t reg){ return inl (g_rtl.io_base + reg); }
static inline void     rtl_w8 (uint16_t reg, uint8_t  v){ outb(g_rtl.io_base + reg, v); }
static inline void     rtl_w16(uint16_t reg, uint16_t v){ outw(g_rtl.io_base + reg, v); }
static inline void     rtl_w32(uint16_t reg, uint32_t v){ outl(g_rtl.io_base + reg, v); }

// ============================================================================
// Yazılımsal reset ve hazır bekleme
// ============================================================================
static bool rtl_reset(void){
    rtl_w8(RTL_CR, CR_RST);
    // RST biti 0'a düşene kadar bekle (max ~10ms)
    for(int i=0; i<10000; i++){
        io_delay();
        if(!(rtl_r8(RTL_CR) & CR_RST)) return true;
    }
    serial_print("[RTL8139] HATA: Reset timeout!\n");
    return false;
}

// ============================================================================
// MAC adresini oku (reset sonrası EEPROM otomatik yüklenir)
// ============================================================================
static void rtl_read_mac(void){
    for(int i=0; i<ETH_ALEN; i++)
        g_rtl.mac[i] = rtl_r8(RTL_MAC0 + i);
}

// ============================================================================
// Polling modu bayrağı (IRQ yoksa ana döngüde çağır)
// ============================================================================
static bool g_polling_mode = false;

// ============================================================================
// RX paketi işle — IRQ handler ve poll() ikisi de bu fonksiyonu çağırır
// ============================================================================
static void rtl_process_rx(void){
    // CR_BUFE: RX tampon boş → işlenecek paket yok
    while(!(rtl_r8(RTL_CR) & CR_BUFE)){

        // Paket başlığı: [status:2][uzunluk:2][veri:len]
        // rx_ptr, DWORD hizalı olmalı
        uint16_t offset = g_rtl.rx_ptr;
        uint8_t* pkt    = g_rtl.rx_buf + offset;

        uint16_t status = *(uint16_t*)(pkt + 0);
        uint16_t plen   = *(uint16_t*)(pkt + 2); // CRC dahil (4 byte)

        // ROK kontrolü
        if(!(status & 0x0001)){
            // Hatalı paket: overflow recovery
            serial_print("[RTL8139] RX status hatasi, overflow recovery\n");
            rtl_w8(RTL_CR, CR_TE | CR_RE);
            rtl_w32(RTL_RCR,
                RCR_APM | RCR_AB | RCR_AM |
                RCR_RBLEN_8K | RCR_RXFTH_256 | RCR_WRAP);
            g_rtl.rx_ptr = 0;
            rtl_w16(RTL_CAPR, (uint16_t)(g_rtl.rx_ptr - 16));
            g_rtl.rx_errors++;
            return;
        }

        // Gerçek veri uzunluğu (başlık 4 byte, CRC son 4 byte)
        if(plen > 4){
            uint16_t data_len = plen - 4;
            uint8_t* data     = pkt + 4; // Ethernet çerçevesi başlangıcı

            if(g_rtl.on_packet && data_len >= ETH_HLEN)
                g_rtl.on_packet(data, data_len);

            g_rtl.rx_count++;
        }

        // RX pointer ilerlet (4-byte hizalı)
        g_rtl.rx_ptr = (uint16_t)((g_rtl.rx_ptr + plen + 4 + 3) & ~3);
        // 8K buffer'a wrap
        g_rtl.rx_ptr %= 8192;

        // Donanıma CAPR (Current Address of Packet Read) bildir
        // Donanım 0x10 offset ile okur; -16 trick overflow koruması sağlar
        rtl_w16(RTL_CAPR, (uint16_t)(g_rtl.rx_ptr - 16));
    }
}

// ============================================================================
// IRQ handler (isr_net stub'ından çağrılır)
// ============================================================================
void rtl8139_irq_handler(void){
    uint16_t isr = rtl_r16(RTL_ISR);
    if(!isr) return;  // Bizim IRQ'muz değil

    // ISR'yi temizle (yazarak sıfırla)
    rtl_w16(RTL_ISR, isr);

    if(isr & ISR_ROK){
        rtl_process_rx();
    }
    if(isr & ISR_RER){
        g_rtl.rx_errors++;
        serial_print("[RTL8139] RX hata\n");
    }
    if(isr & ISR_TOK){
        g_rtl.tx_count++;
    }
    if(isr & ISR_TER){
        g_rtl.tx_errors++;
        serial_print("[RTL8139] TX hata\n");
    }
    if(isr & ISR_RXOVW){
        g_rtl.rx_overflow++;
        serial_print("[RTL8139] RX overflow!\n");
    }
    if(isr & ISR_LINK){
        // Bağlantı durumu değişti
        uint16_t bmsr = rtl_r16(RTL_BMSR);
        g_rtl.link_up = (bmsr & (1<<2)) ? true : false;
        serial_print("[RTL8139] Link ");
        serial_print(g_rtl.link_up ? "UP\n" : "DOWN\n");
    }
}

// ============================================================================
// Polling modu (IRQ yoksa ana döngüde çağır)
// ============================================================================
void rtl8139_poll(void){
    if(!g_rtl.initialized) return;
    rtl_process_rx();
}

// ============================================================================
// RTL8139 başlatma
// ============================================================================
bool rtl8139_init(void){
    serial_print("[RTL8139] PCI taranıyor...\n");

    uint8_t  bus, dev, fn, irq;
    uint16_t iobase;

    if(!pci_find_rtl8139(&bus, &dev, &fn, &iobase, &irq)){
        serial_print("[RTL8139] Kart bulunamadı!\n");
        return false;
    }

    serial_print("[RTL8139] Bulundu — bus=");
    serial_dec(bus);
    serial_print(" dev=");
    serial_dec(dev);
    serial_print(" io=");
    serial_hex16(iobase);
    serial_print(" irq=");
    serial_dec(irq);
    serial_write('\n');

    // Sürücü durumunu sıfırla
    for(int i=0; i<sizeof(RTL8139); i++) ((uint8_t*)&g_rtl)[i]=0;

    g_rtl.io_base = iobase;
    g_rtl.irq     = irq;

    // PCI Bus Master + I/O Space enable
    pci_enable_busmaster(bus, dev, fn);

    // PM state → D0 (güç tasarrufu modundan çıkar)
    rtl_w8(RTL_CONFIG1, 0x00);
    io_delay();

    // Yazılım reset
    if(!rtl_reset()){
        serial_print("[RTL8139] Başlatma başarısız (reset)\n");
        return false;
    }

    // MAC adresini oku
    rtl_read_mac();
    serial_print("[RTL8139] MAC: ");
    for(int i=0; i<ETH_ALEN; i++){
        serial_hex8(g_rtl.mac[i]);
        if(i<ETH_ALEN-1) serial_write(':');
    }
    serial_write('\n');

    // IMR = 0 (tüm kesmeleri geçici kapat, yapılandırma sırasında)
    rtl_w16(RTL_IMR, 0);
    rtl_w16(RTL_ISR, 0xFFFF); // Bekleyen ISR'yi temizle

    // TX+RX'i geçici kapat (RBSTART ve RCR yazılmadan önce)
    rtl_w8(RTL_CR, 0x00);
    io_delay();

    // RX tamponu başlangıç adresini bildir (fiziksel adres = sanal adres, no paging)
    uint64_t rx_phys = (uint64_t)(uintptr_t)g_rtl.rx_buf;
    if(rx_phys >> 32){
        serial_print("[RTL8139] HATA: rx_buf 4GB ustunde, DMA calismaz!\n");
        return false;
    }
    rtl_w32(RTL_RBSTART, (uint32_t)rx_phys);

    // TX tamponlarının adreslerini kaydet (4 slot)
    for(int i=0; i<RTL_TX_SLOTS; i++){
        uint32_t tx_phys = (uint32_t)(uintptr_t)g_rtl.tx_buf[i];
        rtl_w32(RTL_TSAD0 + i*4, tx_phys);
    }

    // TX Config: normal IFG, 2048 byte DMA burst, CRC otomatik
    rtl_w32(RTL_TCR, TCR_IFG_NORMAL | TCR_MXDMA_2048);

    // RX Config: tüm paketleri kabul et (AAP), unicast + broadcast + multicast,
    // 8K ring, FIFO 256B, wrap. AAP olmadan QEMU NAT reply'ları filtrelenebilir.
    rtl_w32(RTL_RCR,
        RCR_AAP | RCR_APM | RCR_AB | RCR_AM |
        RCR_RBLEN_8K | RCR_RXFTH_256 | RCR_WRAP);

    // RX pointer sıfırla
    g_rtl.rx_ptr = 0;
    rtl_w16(RTL_CAPR, (uint16_t)(0 - 16)); // = 0xFFF0

    // TX+RX etkinleştir (tüm config tamamlandıktan sonra)
    rtl_w8(RTL_CR, CR_TE | CR_RE);
    io_delay();

    // IDT gate IRQ11 için keyboard_unified.c → init_interrupts64() içinde
    // idt_set(43, isr_net, 0x08, 0x8E) ile kuruldu.
    // PIC IRQ11 maskesi de init_interrupts64() → irq_enable(11) ile açıldı.
    // Burada sadece kart IMR'sini (RTL8139 donanım kesmelerini) etkinleştiriyoruz.
    g_polling_mode = false;

    // IMR: ROK + RER + TOK + TER + RXOVW + LINK
    rtl_w16(RTL_IMR,
        ISR_ROK | ISR_RER | ISR_TOK | ISR_TER |
        ISR_RXOVW | ISR_LINK);

    // Bağlantı durumunu kontrol et
    uint16_t bmsr = rtl_r16(RTL_BMSR);
    g_rtl.link_up = (bmsr & (1<<2)) ? true : false;

    g_rtl.initialized = true;

    serial_print("[RTL8139] Hazır — Link: ");
    serial_print(g_rtl.link_up ? "UP\n" : "DOWN (kablo takılı değil?)\n");
    return true;
}

// ============================================================================
// Ham Ethernet çerçevesi gönder
// ============================================================================
bool rtl8139_send(const uint8_t* data, uint16_t len){
    if(!g_rtl.initialized) return false;
    if(len < ETH_HLEN || len > ETH_FRAME_MAX){
        serial_print("[RTL8139] TX: geçersiz uzunluk\n");
        return false;
    }

    // TX slot seç (round-robin)
    uint8_t slot = g_rtl.tx_slot;

    // Önceki TX tamamlandı mı? TSD'nin OWN biti 1 = donanım bitti
    uint32_t tsd = inl(g_rtl.io_base + RTL_TSD0 + slot * 4);
    if(!(tsd & (1<<13))){ // OWN bit = 13
        // TX meşgul, bir sonraki slot'u dene
        slot = (slot + 1) % RTL_TX_SLOTS;
        tsd  = inl(g_rtl.io_base + RTL_TSD0 + slot * 4);
        if(!(tsd & (1<<13))){
            serial_print("[RTL8139] TX: tüm slotlar meşgul!\n");
            return false;
        }
    }

    // Veriyi TX tamponuna kopyala
    memcpy_drv(g_rtl.tx_buf[slot], data, len);

    // TSD'ye uzunluk yaz → donanım gönderir
    // Bit 13 (OWN) = 0 yazarak donanıma ver
    // Early TX threshold = 0 (FIFO dolu olunca başla)
    outl(g_rtl.io_base + RTL_TSD0 + slot * 4, (uint32_t)len & 0x1FFF);

    g_rtl.tx_slot = (slot + 1) % RTL_TX_SLOTS;

    return true;
}

// ============================================================================
// Bilgi fonksiyonları
// ============================================================================
void rtl8139_get_mac(uint8_t out[ETH_ALEN]){
    memcpy_drv(out, g_rtl.mac, ETH_ALEN);
}

bool rtl8139_link_is_up(void){
    return g_rtl.link_up;
}

uint32_t rtl8139_get_rx_count(void){ return g_rtl.rx_count; }
uint32_t rtl8139_get_tx_count(void){ return g_rtl.tx_count; }

void rtl8139_set_packet_handler(packet_handler_t handler){
    g_rtl.on_packet = handler;
}

void rtl8139_stats(void){
    serial_print("\n[RTL8139] İstatistikler:\n");
    serial_print("  RX paket: "); serial_dec(g_rtl.rx_count);   serial_write('\n');
    serial_print("  TX paket: "); serial_dec(g_rtl.tx_count);   serial_write('\n');
    serial_print("  RX hata:  "); serial_dec(g_rtl.rx_errors);  serial_write('\n');
    serial_print("  TX hata:  "); serial_dec(g_rtl.tx_errors);  serial_write('\n');
    serial_print("  RX taşma: "); serial_dec(g_rtl.rx_overflow);serial_write('\n');
    serial_print("  Link:     "); serial_print(g_rtl.link_up ? "UP\n" : "DOWN\n");
}

void rtl8139_dump_regs(void){
    if(!g_rtl.initialized) return;
    serial_print("[RTL8139] Yazmaç Dökümü:\n");
    serial_print("  CR  = "); serial_hex8(rtl_r8(RTL_CR));   serial_write('\n');
    serial_print("  ISR = "); serial_hex16(rtl_r16(RTL_ISR));serial_write('\n');
    serial_print("  IMR = "); serial_hex16(rtl_r16(RTL_IMR));serial_write('\n');
    serial_print("  RCR = "); serial_hex32(rtl_r32(RTL_RCR));serial_write('\n');
    serial_print("  TCR = "); serial_hex32(rtl_r32(RTL_TCR));serial_write('\n');
    serial_print("  CAPR= "); serial_hex16(rtl_r16(RTL_CAPR));serial_write('\n');
    serial_print("  CBR = "); serial_hex16(rtl_r16(RTL_CBR)); serial_write('\n');
}