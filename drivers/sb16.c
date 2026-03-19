// sb16.c — Sound Blaster 16 Driver for AscentOS 64-bit  [v7 + diagnostic]
//
// Diagnostic eklendi: sb16_play_pcm ve sb16_irq_handler başlarında
// serial log — IRQ geliyor mu, rate doğru mu, playing flag doğru mu?
//
// Diagnostic logları görmek için QEMU'yu şöyle başlat:
//   qemu-system-x86_64 ... -serial stdio
// veya
//   qemu-system-x86_64 ... -serial file:serial.log
//
// Beklenen log (sağlıklı):
//   [SB16_DIAG] play #1 rate=44100 len=4096 fmt=16b_stereo before=0
//   [SB16_DIAG] IRQ #1
//   [SB16_DIAG] play #2 rate=44100 len=4096 fmt=16b_stereo before=0
//   [SB16_DIAG] IRQ #2
//   ...
//
// Sorun göstergesi:
//   IRQ satırı yok           → IRQ gelmiyor
//   before=1 hiç yok         → hlt döngüsü çalışmıyor
//   rate yanlış              → wav_player rate hatası
//   play sayısı çok az/çok   → chunk boyutu hatası

#include "sb16.h"

extern void serial_print(const char* s);

// ── Port erişim ───────────────────────────────────────────────────
static inline void _outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t _inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void _io_delay(void) { _outb(0x80, 0); }

// ── Gecikme ───────────────────────────────────────────────────────
static inline uint64_t _rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#define CYCLES_PER_US 3800ULL  /* QEMU ~3.8GHz */
static void _delay_us(uint32_t us) {
    uint64_t end = _rdtsc() + (uint64_t)us * CYCLES_PER_US;
    while (_rdtsc() < end) __asm__ volatile ("pause");
}

// ── Diagnostic yardımcı ───────────────────────────────────────────
static void _diag_u32(uint32_t n) {
    char buf[12]; int i = 0; char tmp[12];
    if (!n) { serial_print("0"); return; }
    while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    for (int k = i-1; k >= 0; k--) buf[j++] = tmp[k];
    buf[j] = 0;
    serial_print(buf);
}

// ── Global durum ──────────────────────────────────────────────────
sb16_state_t g_sb16 = { .initialized = false };
int          g_sb16_initialized = 0;
volatile int g_sb16_playing     = 0;
volatile uint32_t g_sb16_irq_count = 0;
/* DMA polling icin export - syscall.c case 411 okur */
uint8_t g_sb16_dma16_ch  = 5;  /* varsayilan ch5, init'te guncellenir */
uint8_t g_sb16_dma8_ch   = 1;  /* varsayilan ch1 */
uint8_t g_sb16_is_16bit  = 0;  /* mevcut transfer 16bit mi */

static uint8_t* _dma_ptr  = (void*)0;
static uint32_t _dma_phys = 0;

#define DMA_BUF_SIZE 8192
static uint8_t _dma_buf[DMA_BUF_SIZE]
    __attribute__((section(".dma_low"), aligned(4096)));

// ── DSP ───────────────────────────────────────────────────────────
void sb16_dsp_write(uint8_t data) {
    int t = 100000;
    while ((_inb(SB16_PORT_WRITE_ST) & 0x80) && --t) _io_delay();
    _outb(SB16_PORT_WRITE, data);
}
uint8_t sb16_dsp_read(void) {
    int t = 100000;
    while (!(_inb(SB16_PORT_READ_ST) & 0x80) && --t) _io_delay();
    return _inb(SB16_PORT_READ);
}
int sb16_dsp_reset(void) {
    _outb(SB16_PORT_RESET, 1); _delay_us(10);
    _outb(SB16_PORT_RESET, 0); _delay_us(10);
    for (int i = 0; i < 2000; i++) {
        if ((_inb(SB16_PORT_READ_ST) & 0x80) && _inb(SB16_PORT_READ) == 0xAA)
            return 0;
        _delay_us(100);
    }
    return -1;
}

// ── Mixer ─────────────────────────────────────────────────────────
void sb16_mixer_write(uint8_t reg, uint8_t val) {
    _outb(SB16_PORT_MIXER_ADDR, reg); _io_delay();
    _outb(SB16_PORT_MIXER_DATA, val);
}
uint8_t sb16_mixer_read(uint8_t reg) {
    _outb(SB16_PORT_MIXER_ADDR, reg); _io_delay();
    return _inb(SB16_PORT_MIXER_DATA);
}
void sb16_set_volume(uint8_t vol) {
    uint8_t v = vol & 0xF8;
    sb16_mixer_write(SB16_MIX_MASTER_LEFT,  v);
    sb16_mixer_write(SB16_MIX_MASTER_RIGHT, v);
}
void sb16_set_pcm_volume(uint8_t vol) {
    uint8_t v = vol & 0xF8;
    sb16_mixer_write(SB16_MIX_PCM_LEFT,  v);
    sb16_mixer_write(SB16_MIX_PCM_RIGHT, v);
}

// ── DMA ───────────────────────────────────────────────────────────
static void _dma8_program(uint8_t channel, uint32_t phys_addr, uint16_t count) {
    uint8_t  ch        = channel & 0x03;
    uint16_t offset    = (uint16_t)(phys_addr & 0xFFFF);
    uint8_t  page      = (uint8_t)((phys_addr >> 16) & 0xFF);
    uint8_t  addr_port = (uint8_t)(ch * 2);
    uint8_t  cnt_port  = (uint8_t)(ch * 2 + 1);
    static const uint8_t PAGE8[4] = { 0x87, 0x83, 0x81, 0x82 };
    _outb(DMA8_MASK,     0x04 | ch);
    _outb(DMA8_CLEAR_FF, 0x00);
    _outb(DMA8_MODE,     0x48 | ch);
    _outb(addr_port, (uint8_t)(offset & 0xFF));
    _outb(addr_port, (uint8_t)(offset >> 8));
    _outb(PAGE8[ch],  page);
    uint16_t cnt1 = count - 1;
    _outb(cnt_port, (uint8_t)(cnt1 & 0xFF));
    _outb(cnt_port, (uint8_t)(cnt1 >> 8));
    _outb(DMA8_MASK, ch);
}

static void _dma16_program(uint8_t channel, uint32_t phys_addr, uint16_t count) {
    phys_addr &= ~1u;
    uint8_t  ch          = (channel - 4) & 0x03;
    uint16_t word_offset = (uint16_t)((phys_addr >> 1) & 0xFFFF);
    uint8_t  page        = (uint8_t)((phys_addr >> 17) & 0x7F);
    uint8_t  addr_port   = (uint8_t)(0xC0 + ch * 4);
    uint8_t  cnt_port    = (uint8_t)(0xC2 + ch * 4);
    static const uint8_t PAGE16[4] = { 0x8F, 0x8B, 0x89, 0x8A };
    _outb(DMA16_MASK,     0x04 | ch);
    _outb(DMA16_CLEAR_FF, 0x00);
    _outb(DMA16_MODE,     0x48 | ch);
    _outb(addr_port,  (uint8_t)(word_offset & 0xFF));
    _outb(addr_port,  (uint8_t)(word_offset >> 8));
    _outb(PAGE16[ch], page);
    uint16_t wcnt1 = (uint16_t)((count / 2) - 1);
    _outb(cnt_port, (uint8_t)(wcnt1 & 0xFF));
    _outb(cnt_port, (uint8_t)(wcnt1 >> 8));
    _outb(DMA16_MASK, ch);
}

// ── Init / Shutdown ───────────────────────────────────────────────
extern void* pmm_alloc_page_low(void) __attribute__((weak));
bool sb16_detect(void) { return sb16_dsp_reset() == 0; }

bool sb16_init(void) {
    _dma_ptr  = _dma_buf;
    _dma_phys = (uint32_t)(uint64_t)(void*)_dma_buf;

    if (_dma_phys == 0 || _dma_phys >= 0x01000000) {
        if (pmm_alloc_page_low) {
            void* low = pmm_alloc_page_low();
            if (low) {
                _dma_ptr  = (uint8_t*)low;
                _dma_phys = (uint32_t)(uint64_t)low;
                serial_print("[SB16] DMA buf: PMM low-page.\n");
            }
        }
        if (_dma_phys == 0 || _dma_phys >= 0x01000000)
            serial_print("[SB16] UYARI: DMA buf >16MB!\n");
    }

    if (sb16_dsp_reset() != 0) return false;

    sb16_dsp_write(SB16_CMD_GET_VERSION);
    g_sb16.dsp_major = sb16_dsp_read();
    g_sb16.dsp_minor = sb16_dsp_read();
    if (g_sb16.dsp_major < 4) return false;

    sb16_mixer_write(0x00, 0x00);
    _delay_us(100);

    uint8_t irq_reg = sb16_mixer_read(SB16_MIX_IRQ_SEL);
    g_sb16.irq = (irq_reg & 0x01) ? 2  :
                 (irq_reg & 0x02) ? 5  :
                 (irq_reg & 0x04) ? 7  :
                 (irq_reg & 0x08) ? 10 : 5;

    uint8_t dma_reg = sb16_mixer_read(SB16_MIX_DMA_SEL);
    g_sb16.dma8  = (dma_reg & 0x02) ? 1 : (dma_reg & 0x08) ? 3 : 1;
    g_sb16.dma16 = (dma_reg & 0x20) ? 5 : (dma_reg & 0x40) ? 6 :
                   (dma_reg & 0x80) ? 7 : 5;

    sb16_set_volume(255);
    sb16_set_pcm_volume(255);
    sb16_mixer_write(SB16_MIX_OUTPUT_CTRL, 0x1F);

    g_sb16.current_fmt  = (sb16_format_t)-1;
    g_sb16.current_rate = 0;
    g_sb16.initialized  = true;
    g_sb16.playing      = false;
    g_sb16_initialized  = 1;
    g_sb16_playing      = 0;
    g_sb16_dma16_ch     = g_sb16.dma16;
    g_sb16_dma8_ch      = g_sb16.dma8;

    sb16_dsp_write(SB16_CMD_SPEAKER_ON);

    serial_print("[SB16] Sound Blaster 16 hazir.\n");
    return true;
}

void sb16_shutdown(void) {
    if (!g_sb16.initialized) return;
    sb16_stop();
    sb16_dsp_write(SB16_CMD_SPEAKER_OFF);
    g_sb16.initialized  = false;
    g_sb16_initialized  = 0;
    g_sb16_playing      = 0;
}

// ── Çalma ─────────────────────────────────────────────────────────
bool sb16_play_pcm(const void* buf, uint16_t len,
                   uint32_t rate_hz, sb16_format_t fmt) {
    if (!g_sb16.initialized || !buf || len == 0) return false;



    if (rate_hz < 4000)  rate_hz = 4000;
    if (rate_hz > 48000) rate_hz = 48000;

    bool is_16bit  = (fmt == SB16_FMT_16BIT_MONO ||
                      fmt == SB16_FMT_16BIT_STEREO);
    bool is_stereo = (fmt == SB16_FMT_8BIT_STEREO ||
                      fmt == SB16_FMT_16BIT_STEREO);

    if (len > DMA_BUF_SIZE) len = (uint16_t)DMA_BUF_SIZE;
    if (is_16bit && (len & 1)) len--;

    // ── DIAGNOSTIC: play çağrısı logla ───────────────────────────
    {
        static uint32_t _play_cnt = 0;
        _play_cnt++;
        if (_play_cnt <= 5) {
            serial_print("[SB16_DIAG] play #");
            _diag_u32(_play_cnt);
            serial_print(" rate=");
            _diag_u32(rate_hz);
            serial_print(" len=");
            _diag_u32(len);
            serial_print(is_16bit  ? " 16bit" : " 8bit");
            serial_print(is_stereo ? "_stereo" : "_mono");
            serial_print(" before=");
            serial_print(g_sb16_playing ? "1\n" : "0\n");
        }
    }

    if (buf != (const void*)_dma_ptr) {
        const uint8_t* s = (const uint8_t*)buf;
        for (uint16_t i = 0; i < len; i++) _dma_ptr[i] = s[i];
    }

    uint32_t phys = _dma_phys;
    if (phys == 0 || phys >= 0x01000000) return false;

    bool same_config = (g_sb16.current_fmt  == fmt &&
                        g_sb16.current_rate == rate_hz);

    if (!same_config) {
        /* Format veya rate değişti — tam yeniden yapılandır.
         * Önce mevcut DMA'yı durdur, sonra rate'i ayarla.
         * _delay_us() yok: sb16_dsp_write() zaten DSP busy-wait yapar. */
        if (g_sb16_playing) {
            sb16_dsp_write(is_16bit ? SB16_CMD_STOP_16 : SB16_CMD_STOP_8);
        }
        sb16_dsp_write(SB16_CMD_SET_SAMPLE_RATE);
        sb16_dsp_write((uint8_t)(rate_hz >> 8));
        sb16_dsp_write((uint8_t)(rate_hz & 0xFF));
        g_sb16.current_fmt  = fmt;
        g_sb16.current_rate = rate_hz;
        serial_print("[SB16] config: ");
        serial_print(is_16bit  ? "16bit " : "8bit ");
        serial_print(is_stereo ? "stereo\n" : "mono\n");
    }
    /* same_config=true: SET_SAMPLE_RATE gönderme.
     * DSP single-cycle bitişinde halt durumunda — yeni 0xB0/0xC0
     * play komutu DSP'yi direkt çalıştırır, rate yeniden set gerekmez.
     * SET_SAMPLE_RATE her chunk'ta gönderilirse DSP PLL yeniden kilitlenir
     * → ilk birkaç chunk'ta IRQ gecikmesi → cızırtı. */

    /* Guard yok: wav_player syscall 412 ile g_sb16_playing poll eder,
     * DMA bitmeden bu fonksiyon çağrılmaz. */

    if (is_16bit) _dma16_program(g_sb16.dma16, phys, len);
    else          _dma8_program (g_sb16.dma8,  phys, len);

    /*
     * SB16 DSP mode byte (16-bit komutlar 0xB0/0xB6 için):
     *   bit 5 (0x20): stereo (1=stereo, 0=mono)
     *   bit 4 (0x10): signed (1=signed, 0=unsigned)
     *
     * 16-bit PCM WAV her zaman signed'dır → bit4=1 zorunlu.
     * 8-bit WAV unsigned'dır → bit4=0.
     *
     * Önceki hata: her iki bit de koşullu set ediliyordu;
     * 16-bit stereo için 0x30 yerine 0x10 gidiyordu (stereo biti eksik).
     */
    uint8_t mode_byte;
    if (is_16bit) {
        mode_byte = 0x10;                    /* signed, mono */
        if (is_stereo) mode_byte |= 0x20;   /* → 0x30 stereo */
    } else {
        mode_byte = 0x00;                    /* unsigned, mono */
        if (is_stereo) mode_byte |= 0x20;   /* → 0x20 stereo */
    }

    /* SB16 DSP sc = sample_count - 1
     * 16-bit mono:   1 sample = 2 byte → sc = len/2 - 1
     * 16-bit stereo: 1 sample = 4 byte → sc = len/4 - 1
     * 8-bit mono:    1 sample = 1 byte → sc = len - 1
     * 8-bit stereo:  1 sample = 2 byte → sc = len/2 - 1 */
    uint16_t sc;
    if      (!is_16bit && !is_stereo) sc = (uint16_t)(len - 1);
    else if (!is_16bit &&  is_stereo) sc = (uint16_t)(len / 2 - 1);
    else if ( is_16bit && !is_stereo) sc = (uint16_t)(len / 2 - 1);
    else                              sc = (uint16_t)(len / 4 - 1);

    sb16_dsp_write(is_16bit ? SB16_CMD_PLAY_16_SC : SB16_CMD_PLAY_8_SC);
    sb16_dsp_write(mode_byte);
    sb16_dsp_write((uint8_t)(sc & 0xFF));
    sb16_dsp_write((uint8_t)(sc >> 8));

    g_sb16.playing  = true;
    g_sb16_playing  = 1;
    g_sb16_is_16bit = is_16bit ? 1 : 0;
    return true;
}

int sb16_play_pcm_raw(const void* buf, unsigned short len,
                      unsigned int rate_hz, int fmt) {
    if (!g_sb16.initialized) return -1;
    if (fmt < 0 || fmt > 3)  return -1;
    return sb16_play_pcm(buf, len, rate_hz, (sb16_format_t)fmt) ? 0 : -1;
}

void sb16_stop(void) {
    if (!g_sb16.initialized) return;
    bool b16 = (g_sb16.current_fmt == SB16_FMT_16BIT_MONO ||
                g_sb16.current_fmt == SB16_FMT_16BIT_STEREO);
    sb16_dsp_write(b16 ? SB16_CMD_STOP_16 : SB16_CMD_STOP_8);
    g_sb16.playing = false;
    g_sb16_playing = 0;
}

// ── IRQ handler ───────────────────────────────────────────────────
void sb16_irq_handler(void) {
    if (!g_sb16.initialized) return;

    // ── DIAGNOSTIC: IRQ geldi ─────────────────────────────────────
    {
        static uint32_t _irq_cnt = 0;
        _irq_cnt++;
        if (_irq_cnt <= 10) {
            serial_print("[SB16_DIAG] IRQ #");
            _diag_u32(_irq_cnt);
            serial_print("\n");
        }
    }

    bool b16 = (g_sb16.current_fmt == SB16_FMT_16BIT_MONO ||
                g_sb16.current_fmt == SB16_FMT_16BIT_STEREO);

    // 1. DSP ACK — IRQ hattını serbest bırak
    if (b16) _inb(SB16_PORT_ACK_16);
    else     _inb(SB16_PORT_ACK_8);

    // 2. Mixer IRQ status (ACK'tan sonra)
    (void)sb16_mixer_read(SB16_MIX_IRQ_STATUS);

    // 3. PIC EOI
    if (g_sb16.irq >= 8) _outb(0xA0, 0x20);
    _outb(0x20, 0x20);

    // 4. Flag'leri düşür
    g_sb16.playing = false;
    g_sb16_playing = 0;
    g_sb16_irq_count++;  // syscall.c beklemesini uyandır
}

// ── Streaming stub ────────────────────────────────────────────────
bool sb16_stream_start(uint32_t rate_hz, sb16_format_t fmt,
                       sb16_stream_cb_t cb, void* ctx) {
    (void)cb; (void)ctx;
    return sb16_play_pcm(_dma_ptr, DMA_BUF_SIZE, rate_hz, fmt);
}
void sb16_stream_feed(const uint8_t* data, int len) {
    if (!data || len <= 0) return;
    int n = len < DMA_BUF_SIZE ? len : DMA_BUF_SIZE;
    for (int i = 0; i < n; i++) _dma_ptr[i] = data[i];
}
void sb16_stream_stop(void) { sb16_stop(); }

// ── Debug ─────────────────────────────────────────────────────────
static void _u32hex(uint32_t n, char* b) {
    const char* h = "0123456789ABCDEF";
    b[0]='0'; b[1]='x';
    for (int i=7;i>=0;i--) { b[2+i]=h[n&0xF]; n>>=4; }
    b[10]=0;
}
void sb16_print_info(void) {
    char buf[16];
    serial_print("[SB16] DSP    : ");
    _diag_u32(g_sb16.dsp_major); serial_print(".");
    _diag_u32(g_sb16.dsp_minor); serial_print("\n");
    serial_print("[SB16] IRQ    : "); _diag_u32(g_sb16.irq);  serial_print("\n");
    serial_print("[SB16] DMA8   : ch"); _diag_u32(g_sb16.dma8);  serial_print("\n");
    serial_print("[SB16] DMA16  : ch"); _diag_u32(g_sb16.dma16); serial_print("\n");
    serial_print("[SB16] DMAphys: "); _u32hex(_dma_phys, buf); serial_print(buf);
    serial_print(_dma_phys && _dma_phys < 0x1000000 ? " [OK]\n" : " [!!!>16MB]\n");
    serial_print("[SB16] Status : ");
    serial_print(g_sb16.initialized ? "HAZIR\n" : "INIT YOK\n");
}

// ── Efektler ──────────────────────────────────────────────────────
static const uint8_t _sine[18] = {
    128,172,210,237,251,251,237,210,172,
    128, 84, 46, 19,  5,  5, 19, 46, 84
};
void sb16_test_tone(void) {
    if (!g_sb16.initialized) return;
    int n = 2000;
    for (int i = 0; i < n; i++) _dma_ptr[i] = _sine[i % 18];
    sb16_play_pcm(_dma_ptr, (uint16_t)n, 8000, SB16_FMT_8BIT_MONO);
}
void sb16_ding(void) {
    if (!g_sb16.initialized) return;
    int n = 2205;
    for (int i = 0; i < n; i++) {
        int amp = 127 - (127 * i / n);
        int idx = (int)(((uint32_t)i * 18U * 1000U) / 22050U) % 18;
        int s   = (int)_sine[idx] - 128;
        _dma_ptr[i] = (uint8_t)(128 + (s * amp) / 127);
    }
    sb16_play_pcm(_dma_ptr, (uint16_t)n, 22050, SB16_FMT_8BIT_MONO);
}