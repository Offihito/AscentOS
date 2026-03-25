#ifndef SB16_H
#define SB16_H

#include <stdint.h>
#include <stdbool.h>

// I/O PORT MAP (base 0x220)
#define SB16_BASE            0x220
#define SB16_PORT_RESET      (SB16_BASE + 0x6)
#define SB16_PORT_READ       (SB16_BASE + 0xA)
#define SB16_PORT_WRITE      (SB16_BASE + 0xC)
#define SB16_PORT_READ_ST    (SB16_BASE + 0xE)
#define SB16_PORT_WRITE_ST   (SB16_BASE + 0xC)
#define SB16_PORT_ACK_8      (SB16_BASE + 0xE)
#define SB16_PORT_ACK_16     (SB16_BASE + 0xF)
#define SB16_PORT_MIXER_ADDR (SB16_BASE + 0x4)
#define SB16_PORT_MIXER_DATA (SB16_BASE + 0x5)

// DSP Commands
#define SB16_CMD_GET_VERSION      0xE1
#define SB16_CMD_SPEAKER_ON       0xD1
#define SB16_CMD_SPEAKER_OFF      0xD3
#define SB16_CMD_SET_SAMPLE_RATE  0x41
#define SB16_CMD_HALT_DMA_8       0xD0
#define SB16_CMD_RESUME_DMA_8     0xD4
#define SB16_CMD_HALT_DMA_16      0xD5
#define SB16_CMD_RESUME_DMA_16    0xD6
#define SB16_CMD_STOP_8           0xDA
#define SB16_CMD_STOP_16          0xD9
#define SB16_CMD_PLAY_8_SC        0xC0
#define SB16_CMD_PLAY_16_SC       0xB0
#define SB16_CMD_PLAY_8_AI        0xC6
#define SB16_CMD_PLAY_16_AI       0xB6

// MIXER
#define SB16_MIX_MASTER_LEFT   0x30
#define SB16_MIX_MASTER_RIGHT  0x31
#define SB16_MIX_PCM_LEFT      0x32
#define SB16_MIX_PCM_RIGHT     0x33
#define SB16_MIX_IRQ_SEL       0x80
#define SB16_MIX_DMA_SEL       0x81
#define SB16_MIX_IRQ_STATUS    0x82
#define SB16_MIX_OUTPUT_CTRL   0x3C

// ISA DMA PORT MAP
#define DMA8_MASK      0x0A
#define DMA8_MODE      0x0B
#define DMA8_CLEAR_FF  0x0C

#define DMA16_MASK     0xD4
#define DMA16_MODE     0xD6
#define DMA16_CLEAR_FF 0xD8

#define DMA_BUF_SIZE  8192
#define DMA_HALF      4096


// FORMAT
typedef enum {
    SB16_FMT_8BIT_MONO      = 0,
    SB16_FMT_8BIT_STEREO    = 1,
    SB16_FMT_16BIT_MONO     = 2,
    SB16_FMT_16BIT_STEREO   = 3,
} sb16_format_t;

typedef struct {
    bool          initialized;
    uint8_t       dsp_major;
    uint8_t       dsp_minor;
    uint8_t       irq;
    uint8_t       dma8;
    uint8_t       dma16;
    bool          playing;
    bool          auto_init;     
    sb16_format_t current_fmt;
    uint32_t      current_rate;
    volatile int  active_half;  
} sb16_state_t;

extern sb16_state_t g_sb16;
extern int          g_sb16_initialized;   


typedef int (*sb16_stream_cb_t)(uint8_t* buf, int bytes_needed, void* ctx);

// PUBLIC API
bool    sb16_init(void);
void    sb16_shutdown(void);
bool    sb16_detect(void);

void    sb16_dsp_write(uint8_t data);
uint8_t sb16_dsp_read(void);
int     sb16_dsp_reset(void);

void    sb16_mixer_write(uint8_t reg, uint8_t val);
uint8_t sb16_mixer_read(uint8_t reg);
void    sb16_set_volume(uint8_t vol);
void    sb16_set_pcm_volume(uint8_t vol);

bool    sb16_play_pcm(const void* buf, uint16_t len,
                      uint32_t rate_hz, sb16_format_t fmt);
void    sb16_stop(void);

bool    sb16_stream_start(uint32_t rate_hz, sb16_format_t fmt,
                          sb16_stream_cb_t cb, void* ctx);

void    sb16_stream_feed(const uint8_t* data, int len);
void    sb16_stream_stop(void);

void    sb16_irq_handler(void);

int     sb16_play_pcm_raw(const void* buf, unsigned short len,
                          unsigned int rate_hz, int fmt);

void    sb16_print_info(void);

void    sb16_test_tone(void);  
void    sb16_ding(void);       

#endif 