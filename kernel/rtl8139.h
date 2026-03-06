// rtl8139.h — AscentOS RTL8139 Ağ Kartı Sürücüsü
// Aşama 1: PCI tarama, kart tespiti, TX/RX ring başlatma,
//           interrupt-driven paket alma, raw Ethernet gönderme.
//
// Kullanım:
//   rtl8139_init()          → PCI'da kartı bul, başlat
//   rtl8139_send(buf, len)  → Ham Ethernet çerçevesi gönder
//   rtl8139_poll()          → Ana döngüde çağır (RX işle)
//   rtl8139_get_mac(out)    → Kart MAC adresini oku (6 byte)
//   rtl8139_stats()         → Serial'e istatistik bas
//
// NOT: IRQ11 (PCI default) kernel'in IDT'sine eklenir.
//      interrupts64.asm'e isr_net stub eklenmeli (aşağıda açıklandı).

#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// RTL8139 Register offsetleri
// ============================================================================
#define RTL_MAC0        0x00    // MAC adresi 0-5
#define RTL_MAR0        0x08    // Multicast adresi 0-7
#define RTL_TSD0        0x10    // TX Status 0-3 (her biri 4 byte)
#define RTL_TSAD0       0x20    // TX Start Address 0-3
#define RTL_RBSTART     0x30    // RX Buffer Start Address
#define RTL_ERBCR       0x34    // Early RX Byte Count
#define RTL_ERSR        0x36    // Early RX Status
#define RTL_CR          0x37    // Komut Register
#define RTL_CAPR        0x38    // Current Address of Packet Read
#define RTL_CBR         0x3A    // Current Buffer Address (RX)
#define RTL_IMR         0x3C    // Interrupt Mask Register
#define RTL_ISR         0x3E    // Interrupt Status Register
#define RTL_TCR         0x40    // TX Config Register
#define RTL_RCR         0x44    // RX Config Register
#define RTL_TCTR        0x48    // Timer Counter (32-bit)
#define RTL_MPC         0x4C    // Missed Packet Counter
#define RTL_9346CR      0x50    // 93C46 Command Register (EEPROM)
#define RTL_CONFIG0     0x51
#define RTL_CONFIG1     0x52
#define RTL_BMCR        0x62    // Basic Mode Control (MII)
#define RTL_BMSR        0x64    // Basic Mode Status (MII)

// ============================================================================
// CR (Command Register) bitleri
// ============================================================================
#define CR_RST          0x10    // Software Reset
#define CR_RE           0x08    // Receiver Enable
#define CR_TE           0x04    // Transmitter Enable
#define CR_BUFE         0x01    // RX Buffer Empty

// ============================================================================
// ISR / IMR bitleri
// ============================================================================
#define ISR_ROK         0x0001  // RX OK
#define ISR_RER         0x0002  // RX Error
#define ISR_TOK         0x0004  // TX OK
#define ISR_TER         0x0008  // TX Error
#define ISR_RXOVW       0x0010  // RX Buffer Overflow
#define ISR_LINK        0x0020  // Link Change
#define ISR_FOVW        0x0040  // RX FIFO Overflow
#define ISR_LENCHG      0x2000  // Length Change
#define ISR_TIMEOUT     0x4000  // Timer Timeout
#define ISR_SERR        0x8000  // System Error

// ============================================================================
// RCR (RX Config) bitleri
// ============================================================================
#define RCR_AAP         (1<<0)  // Accept All Packets
#define RCR_APM         (1<<1)  // Accept Physical Match
#define RCR_AM          (1<<2)  // Accept Multicast
#define RCR_AB          (1<<3)  // Accept Broadcast
#define RCR_AR          (1<<4)  // Accept Runt (<64 byte)
#define RCR_AER         (1<<5)  // Accept Error
// RX buffer büyüklüğü: 8K+16 = 0 (varsayılan, güvenli)
#define RCR_RBLEN_8K    (0<<11)
#define RCR_RBLEN_16K   (1<<11)
#define RCR_RBLEN_32K   (2<<11)
#define RCR_RBLEN_64K   (3<<11)
// RX FIFO threshold: 256 byte
#define RCR_RXFTH_256   (4<<13)
// WRAP: ring buffer wrap etmesin (basit pointer hesabı)
#define RCR_WRAP        (1<<7)

// ============================================================================
// TCR (TX Config) bitleri
// ============================================================================
#define TCR_IFG_NORMAL  (3<<24) // Interframe Gap 9.6µs
#define TCR_MXDMA_2048  (7<<8)  // Max DMA burst 2048 byte

// ============================================================================
// TX Slot sayısı (RTL8139 donanım sabiti: 4 adet)
// ============================================================================
#define RTL_TX_SLOTS    4
#define RTL_TX_BUF_SIZE 2048   // Her TX tamponu (DWORD hizalı)

// ============================================================================
// RX ring tamponu (8K + 16 byte overflow guard + CAPR header)
// ============================================================================
#define RTL_RX_BUF_SIZE (8192 + 16 + 1500)

// ============================================================================
// PCI sabitleri (RTL8139)
// ============================================================================
#define RTL_VENDOR_ID   0x10EC
#define RTL_DEVICE_ID   0x8139

// PCI config adresi
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

// ============================================================================
// Ethernet sabitleri
// ============================================================================
#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HLEN + ETH_MTU)

// ============================================================================
// Paket alındığında çağrılacak callback tipi
// packet_handler_t(buf, len) şeklinde kaydedilir.
// ============================================================================
typedef void (*packet_handler_t)(const uint8_t* buf, uint16_t len);

// ============================================================================
// RTL8139 sürücü durumu (tek instance, hobi OS için yeterli)
// ============================================================================
typedef struct {
    uint16_t    io_base;            // I/O port tabanı (PCI BAR0)
    uint8_t     mac[ETH_ALEN];      // Kart MAC adresi
    uint8_t     irq;                // PCI IRQ numarası

    // RX ring tamponu (fiziksel = sanal, paging yok)
    uint8_t     rx_buf[RTL_RX_BUF_SIZE] __attribute__((aligned(4)));
    uint16_t    rx_ptr;             // CAPR gösteren yazılım pointer'ı

    // TX tamponları (4 slot döngüsel)
    uint8_t     tx_buf[RTL_TX_SLOTS][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
    uint8_t     tx_slot;            // Bir sonraki boş TX slot (0-3)

    // Alınan paket callback'i
    packet_handler_t on_packet;

    // İstatistikler
    uint32_t    rx_count;
    uint32_t    tx_count;
    uint32_t    rx_errors;
    uint32_t    tx_errors;
    uint32_t    rx_overflow;

    bool        link_up;
    bool        initialized;
} RTL8139;

// ============================================================================
// Public API
// ============================================================================

// PCI taraması → kart bul → donanımı başlat → IRQ kur
// Başarılı ise true döner, kart yoksa false.
bool rtl8139_init(void);

// Ham Ethernet çerçevesi gönder (Ethernet başlığı dahil)
// len: 14..1514 byte arası
// Başarılı ise true, TX tamponu doluysa/yanlış uzunluksa false döner.
bool rtl8139_send(const uint8_t* data, uint16_t len);

// IRQ handler'dan çağrılan RX/TX işleme fonksiyonu
// interrupts64.asm'deki isr_net, bu fonksiyonu çağırır.
void rtl8139_irq_handler(void);

// Ana döngüde polling modu için (opsiyonel, IRQ devre dışıysa kullan)
void rtl8139_poll(void);

// MAC adresini 6-byte tampon 'out'a kopyalar
void rtl8139_get_mac(uint8_t out[ETH_ALEN]);

// Bağlantı durumu
bool rtl8139_link_is_up(void);

// Seri porta istatistik yaz
void rtl8139_stats(void);

// Paket alma callback'i kaydet
// Her gelen çerçeve için handler(buf, len) çağrılır
void rtl8139_set_packet_handler(packet_handler_t handler);

// ============================================================================
// Dahili yardımcılar (test/debug için erişilebilir)
// ============================================================================
void rtl8139_dump_regs(void);

#endif // RTL8139_H