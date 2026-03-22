#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pci.h"

// ============================================================================
// RTL8139 Register offsets
// ============================================================================
#define RTL_MAC0        0x00    
#define RTL_MAR0        0x08  
#define RTL_TSD0        0x10    
#define RTL_TSAD0       0x20   
#define RTL_RBSTART     0x30    
#define RTL_ERBCR       0x34    
#define RTL_ERSR        0x36    
#define RTL_CR          0x37    
#define RTL_CAPR        0x38    
#define RTL_CBR         0x3A    
#define RTL_IMR         0x3C    
#define RTL_ISR         0x3E    
#define RTL_TCR         0x40    
#define RTL_RCR         0x44    
#define RTL_TCTR        0x48    
#define RTL_MPC         0x4C    
#define RTL_9346CR      0x50    
#define RTL_CONFIG0     0x51
#define RTL_CONFIG1     0x52
#define RTL_BMCR        0x62   
#define RTL_BMSR        0x64    


#define CR_RST          0x10   
#define CR_RE           0x08    
#define CR_TE           0x04    
#define CR_BUFE         0x01   

// ============================================================================
// ISR / IMR bytes
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
// RCR (RX Config) bytes
// ============================================================================
#define RCR_AAP         (1<<0)  
#define RCR_APM         (1<<1)  
#define RCR_AM          (1<<2)  
#define RCR_AB          (1<<3)  
#define RCR_AR          (1<<4)  
#define RCR_AER         (1<<5)  

#define RCR_RBLEN_8K    (0<<11)
#define RCR_RBLEN_16K   (1<<11)
#define RCR_RBLEN_32K   (2<<11)
#define RCR_RBLEN_64K   (3<<11)

#define RCR_RXFTH_256   (4<<13)

#define RCR_WRAP        (1<<7)

#define TCR_IFG_NORMAL  (3<<24) 
#define TCR_MXDMA_2048  (7<<8)  

#define RTL_TX_SLOTS    4
#define RTL_TX_BUF_SIZE 2048 

#define RTL_RX_BUF_SIZE (8192 + 16 + 1500)

#define RTL_VENDOR_ID   0x10EC
#define RTL_DEVICE_ID   0x8139


#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HLEN + ETH_MTU)

typedef void (*packet_handler_t)(const uint8_t* buf, uint16_t len);

// ============================================================================
// RTL8139 Driver Statistic
// ============================================================================
typedef struct {
    uint16_t    io_base;            
    uint8_t     mac[ETH_ALEN];      
    uint8_t     irq;                

    uint8_t     rx_buf[RTL_RX_BUF_SIZE] __attribute__((aligned(4)));
    uint16_t    rx_ptr;          

    uint8_t     tx_buf[RTL_TX_SLOTS][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
    uint8_t     tx_slot;            

    packet_handler_t on_packet;

    // Statistics
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

bool rtl8139_init(void);

bool rtl8139_send(const uint8_t* data, uint16_t len);

void rtl8139_irq_handler(void);

void rtl8139_poll(void);

void rtl8139_get_mac(uint8_t out[ETH_ALEN]);

bool rtl8139_link_is_up(void);

void rtl8139_stats(void);

void rtl8139_set_packet_handler(packet_handler_t handler);

void rtl8139_dump_regs(void);

#endif