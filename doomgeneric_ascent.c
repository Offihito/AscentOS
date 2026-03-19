// doomgeneric_ascent.c — AscentOS Doom port (v12)
//
// v12: SYS_READ (fd=0) yerine SYS_KB_READ (410) kullaniliyor.
// SYS_KB_READ termios, ICANON, hlt hicbirini kullanmaz —
// dogrudan kb_ring_pop() cagirir. Kernel panic yok.
//
// keyboard_unified.c: ok tuslari 0xac-0xaf, ESC=27 olarak ring buffer'a
// yaziliyor (VT100 sequence yok). Burasi dogrudan okuyor.

#include "doomgeneric.h"
#include <stdint.h>

// ── Syscall numaralari ──────────────────────────────────────────
#define SYS_WRITE      1
#define SYS_EXIT      60
#define SYS_GETTICKS 404
#define SYS_FB_INFO  407
#define SYS_KB_RAW   408
#define SYS_FB_BLIT  409
#define SYS_KB_READ  410   // termios bypass — dogrudan kb_ring_pop

// ── Kernel struct'lari (tum field uint64_t) ─────────────────────
typedef struct { uint64_t addr,width,height,pitch,bpp; } fb_info_t;
typedef struct { uint64_t src_pixels,src_w,src_h,dst_x,dst_y,scale; } fb_blit_t;

// ── Syscall wrappers ────────────────────────────────────────────
static inline long _sc0(long n) {
    long r; __asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory"); return r;
}
static inline long _sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory"); return r;
}
static inline long _sc2(long n, long a, long b) {
    long r; __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory"); return r;
}
static inline long _sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}

// ── Debug print ─────────────────────────────────────────────────
static void _puts(const char *s) {
    int n = 0; while (s[n]) n++;
    if (n) _sc3(SYS_WRITE, 1, (long)s, n);
}
static void _putint(long v) {
    if (v < 0) { _puts("-"); v = -v; }
    if (v == 0) { _puts("0"); return; }
    char b[20]; int i = 19; b[i] = 0;
    while (v > 0) { b[--i] = '0' + (int)(v % 10); v /= 10; }
    _puts(b + i);
}

// ── Framebuffer ─────────────────────────────────────────────────
static uint32_t g_dst_x = 0, g_dst_y = 0, g_scale = 1, g_ready = 0;

static void fb_init(void) {
    fb_info_t info = {0,0,0,0,0};
    long r = _sc1(SYS_FB_INFO, (long)&info);
    _puts("[FB] r="); _putint(r);
    _puts(" w="); _putint((long)info.width);
    _puts(" h="); _putint((long)info.height);
    _puts("\n");
    if (r != 0 || !info.width || !info.height) {
        g_scale = 1; g_dst_x = 0; g_dst_y = 0; g_ready = 1; return;
    }
    uint32_t w = (uint32_t)info.width, h = (uint32_t)info.height;
    uint32_t sx = w / DOOMGENERIC_RESX, sy = h / DOOMGENERIC_RESY;
    uint32_t sc = sx < sy ? sx : sy;
    if (!sc) sc = 1; if (sc > 4) sc = 4;
    g_scale = sc;
    g_dst_x = (w - DOOMGENERIC_RESX * sc) / 2;
    g_dst_y = (h - DOOMGENERIC_RESY * sc) / 2;
    g_ready = 1;
    _puts("[FB] scale="); _putint(sc);
    _puts(" dst=("); _putint(g_dst_x); _puts(","); _putint(g_dst_y); _puts(")\n");
}

// ── Klavye ──────────────────────────────────────────────────────
//
// keyboard_unified.c ring buffer protokolü (kb_raw_mode=1 iken):
//   Press  : tek byte
//   Release: 0x80 + aynı byte
//
// Doom keycode'ları (doomdef.h):
//   KEY_RIGHTARROW=0xae  KEY_LEFTARROW=0xac
//   KEY_UPARROW=0xad     KEY_DOWNARROW=0xaf
//   KEY_RCTRL=0x9d (ateş)  KEY_RSHIFT=0xb6 (koş)
//   KEY_USE=' '=0x20 (kapı)  KEY_ESCAPE=27  KEY_ENTER=13

#define KQ_SIZE 256
typedef struct { unsigned char key; unsigned char pressed; } kev_t;
static kev_t kq[KQ_SIZE];
static int   kq_h = 0, kq_t = 0;

static void kq_push(unsigned char key, int pressed) {
    int nx = (kq_t + 1) % KQ_SIZE;
    if (nx != kq_h) {
        kq[kq_t].key     = key;
        kq[kq_t].pressed = (unsigned char)pressed;
        kq_t = nx;
    }
}

// ring buffer byte → Doom keycode
// keyboard_unified.c'den gelen ham byte'ları dönüştür
static unsigned char byte_to_doom(unsigned char c) {
    // keyboard_unified.c bunları zaten doğru değerle gönderiyor
    if (c == 0x9d) return 0x9d;  // KEY_RCTRL  (ateş)
    if (c == 0xb6) return 0xb6;  // KEY_RSHIFT (koş)
    if (c == 0xad) return 0xad;  // KEY_UPARROW
    if (c == 0xaf) return 0xaf;  // KEY_DOWNARROW
    if (c == 0xae) return 0xae;  // KEY_RIGHTARROW
    if (c == 0xac) return 0xac;  // KEY_LEFTARROW
    if (c == 27)   return 27;    // KEY_ESCAPE
    if (c == 13)   return 13;    // KEY_ENTER
    if (c == '\n') return 13;
    // WASD fiziksel pozisyon — sc_to_char küçük harf döndürür
    if (c == 'w')  return 0xad;  // ileri
    if (c == 's')  return 0xaf;  // geri
    if (c == 'a')  return 0xac;  // sol
    if (c == 'd')  return 0xae;  // sağ
    // Büyük harf de olabilir (shift basılıyken)
    if (c == 'W')  return 0xad;
    if (c == 'S')  return 0xaf;
    if (c == 'A')  return 0xac;
    if (c == 'D')  return 0xae;
    // Space ve E → KEY_USE (kapı aç / kullan)
    if (c == ' ')  return ' ';   // 0x20
    if (c == 'e')  return ' ';   // 0x20
    if (c == 'E')  return ' ';   // 0x20
    // Diğer tuşlar (menü, silah değiştirme)
    if (c == '\t') return 9;
    if (c >= '0' && c <= '9') return c;
    if (c == ',')  return c;
    if (c == '.')  return c;
    if (c >= 'a' && c <= 'z') return c;
    return 0;
}

static void poll_keys(void) {
    unsigned char buf[32];
    long n = _sc3(SYS_KB_READ, (long)buf, 0, 32);
    if (n <= 0) return;
    long i = 0;
    while (i < n) {
        unsigned char b = buf[i++];
        if (b == 0x80) {
            // Release event: bir sonraki byte hangi tuş
            if (i >= n) break;
            unsigned char raw = buf[i++];
            unsigned char k = byte_to_doom(raw);
            if (k) kq_push(k, 0);
        } else {
            // Press event
            unsigned char k = byte_to_doom(b);
            if (k) kq_push(k, 1);
        }
    }
}

// ── Doom Generic API ────────────────────────────────────────────

// Doom g_game.c'deki key binding değişkenlerine erişim
// m_misc.c bunları doom.cfg'den override edebilir — DG_Init'ten sonra set et
extern int key_fire;
extern int key_use;
extern int key_speed;
extern int key_strafe;
extern int key_up;
extern int key_down;
extern int key_left;
extern int key_right;
extern int key_strafeleft;
extern int key_straferight;

void DG_Init(void) {
    _puts("[DG_Init] v12\n");
    fb_init();
    _sc1(SYS_KB_RAW, 1);

    // Key binding'leri zorla set et — doom.cfg override'ını engelle
    // Doom doomgeneric_Create() içinde M_LoadDefaults() çağırıyor,
    // bu DG_Init'ten önce olabilir. O yüzden burada tekrar set ediyoruz.
    key_fire        = 0x9d;  // KEY_RCTRL = Ctrl
    key_use         = 0x20;  // space = ' '
    key_speed       = 0xb6;  // KEY_RSHIFT = Shift (koş)
    key_up          = 0xad;  // KEY_UPARROW
    key_down        = 0xaf;  // KEY_DOWNARROW
    key_left        = 0xac;  // KEY_LEFTARROW
    key_right       = 0xae;  // KEY_RIGHTARROW
    key_strafeleft  = ',';
    key_straferight = '.';

    _puts("[DG_Init] key bindings set\n");
    _puts("[DG_Init] hazir ScreenBuffer=0x");
    {
        uint64_t a = (uint64_t)(uintptr_t)DG_ScreenBuffer;
        char hx[17]; hx[16]=0;
        const char *h = "0123456789abcdef";
        for (int i=0;i<16;i++) hx[i]=h[(a>>(60-i*4))&0xf];
        _puts(hx);
    }
    _puts("\n");
}

void DG_DrawFrame(void) {
    if (!DG_ScreenBuffer || !g_ready) return;
    static fb_blit_t req;
    req.src_pixels = (uint64_t)(uintptr_t)DG_ScreenBuffer;
    req.src_w  = DOOMGENERIC_RESX;
    req.src_h  = DOOMGENERIC_RESY;
    req.dst_x  = g_dst_x;
    req.dst_y  = g_dst_y;
    req.scale  = g_scale;
    __asm__ volatile(""::: "memory");
    static int ff = 1;
    if (ff) { ff = 0; _puts("[DG_DrawFrame] ilk cizim\n"); }
    _sc1(SYS_FB_BLIT, (long)&req);
}

void DG_SleepMs(uint32_t ms) { (void)ms; }

uint32_t DG_GetTicksMs(void) {
    static uint64_t t0 = 0;
    long now = _sc0(SYS_GETTICKS);
    if (!t0) t0 = (uint64_t)(unsigned long)now;
    return (uint32_t)(((uint64_t)(unsigned long)now - t0) & 0xFFFFFFFFu);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    poll_keys();
    if (kq_h == kq_t) return 0;
    *key     = kq[kq_h].key;
    *pressed = kq[kq_h].pressed;
    kq_h = (kq_h + 1) % KQ_SIZE;
    return 1;
}

void DG_SetWindowTitle(const char *t) { (void)t; }

void I_Error(const char *e, ...) {
    _puts("[CRASH] "); _puts(e); _puts("\n");
    _sc1(SYS_EXIT, 1);
    __builtin_unreachable();
}


// ═══════════════════════════════════════════════════════════════
// SES SİSTEMİ — SB16 Software Mixer
// i_sound.h imzalarına tam uyumlu (doomgeneric repo versiyonu)
// ═══════════════════════════════════════════════════════════════

#include "i_sound.h"
#include "sounds.h"

#define SYS_SB16_PLAY    411
#define SYS_SB16_PLAYING 412
#define SB16_FMT_8BIT_MONO 0
#define SFX_RATE     11025
#define MIX_CHANNELS 8
#define MIX_BUF_SIZE 2048

static inline long _sb16_play(const void* buf, long len, long rate, long fmt) {
    long r;
    register long r10 __asm__("r10") = fmt;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_SB16_PLAY), "D"((long)buf), "S"(len), "d"(rate), "r"(r10)
        : "rcx","r11","memory");
    return r;
}
static inline long _sb16_playing(void) {
    long r;
    __asm__ volatile("movq $412,%%rax; syscall"
        : "=a"(r) :: "rcx","r11","memory");
    return r;
}

typedef struct {
    const unsigned char* data;
    int pos, len, vol, active, handle;
} mix_chan_t;

static mix_chan_t g_chan[MIX_CHANNELS];
static unsigned char g_mix_buf[MIX_BUF_SIZE];
static int g_snd_init = 0;
static int g_handle_seq = 1;

static void snd_mix_and_send(int n) {
    if (!n) return;
    if (n > MIX_BUF_SIZE) n = MIX_BUF_SIZE;
    for (int i = 0; i < n; i++) g_mix_buf[i] = 128;
    for (int ch = 0; ch < MIX_CHANNELS; ch++) {
        mix_chan_t *c = &g_chan[ch];
        if (!c->active) continue;
        for (int i = 0; i < n && c->pos < c->len; i++, c->pos++) {
            int s = ((int)c->data[c->pos] - 128) * c->vol >> 7;
            int o = (int)g_mix_buf[i] - 128 + s;
            if (o >  127) o =  127;
            if (o < -128) o = -128;
            g_mix_buf[i] = (unsigned char)(o + 128);
        }
        if (c->pos >= c->len) c->active = 0;
    }
    while (_sb16_playing() > 0) {}
    _sb16_play(g_mix_buf, n, SFX_RATE, SB16_FMT_8BIT_MONO);
}

/* void I_InitSound(boolean use_sfx_prefix) */
void I_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    for (int i = 0; i < MIX_CHANNELS; i++) g_chan[i].active = 0;
    g_snd_init = 1;
    _puts("[SND] I_InitSound ok\n");
}

void I_ShutdownSound(void) { g_snd_init = 0; }

void I_UpdateSound(void) {
    if (!g_snd_init) return;
    snd_mix_and_send(315); /* ~35Hz tick @ 11025Hz = 315 sample */
}

/* int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) */
int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    (void)sep;
    if (!g_snd_init || !sfxinfo) return -1;

    /* W_CacheLumpName ile SFX lump'ını yükle.
     * Doom SFX isimleri "ds" prefix'li: "dspistol", "dsshotgn" vs.
     * PU_CACHE=1: lump cache'e alınır, sonra serbest bırakılır. */
    extern int   W_GetNumForName(const char *name);
    extern void* W_CacheLumpNum(int lump, int tag);

    /* sfxinfo->name: "pistol", "shotgn" gibi — "ds" prefix ekle */
    char lumpname[16];
    lumpname[0]='d'; lumpname[1]='s';
    int i = 0;
    while (sfxinfo->name[i] && i < 13) {
        lumpname[2+i] = sfxinfo->name[i]; i++;
    }
    lumpname[2+i] = 0;

    int lumpnum = W_GetNumForName(lumpname);
    if (lumpnum < 0) return -1;

    unsigned char *p = (unsigned char*)W_CacheLumpNum(lumpnum, 1 /*PU_CACHE*/);
    if (!p) return -1;

    /* Doom SFX lump formatı: 8 byte header + PCM
     * [0-1]=0x0003 (type), [2-3]=rate (LE u16), [4-7]=length (LE u32) */
    int len = (int)(p[4] | (p[5]<<8) | (p[6]<<16) | (p[7]<<24));
    if (len <= 0 || len > 65536) return -1;
    const unsigned char *pcm = p + 8;

    int ch = channel % MIX_CHANNELS;
    g_chan[ch].data   = pcm;
    g_chan[ch].pos    = 0;
    g_chan[ch].len    = len;
    g_chan[ch].vol    = vol; /* 0-127 */
    g_chan[ch].active = 1;
    g_chan[ch].handle = g_handle_seq;
    return g_handle_seq++;
}

void I_StopSound(int handle) {
    for (int i = 0; i < MIX_CHANNELS; i++)
        if (g_chan[i].handle == handle) { g_chan[i].active = 0; break; }
}

/* boolean I_SoundIsPlaying(int channel) */
boolean I_SoundIsPlaying(int channel) {
    int ch = channel % MIX_CHANNELS;
    return g_chan[ch].active ? true : false;
}

/* void I_UpdateSoundParams(int channel, int vol, int sep) */
void I_UpdateSoundParams(int channel, int vol, int sep) {
    (void)sep;
    int ch = channel % MIX_CHANNELS;
    if (g_chan[ch].active)
        g_chan[ch].vol = (vol * 127) / 127;
}

/* Müzik stub'ları — i_sound.h imzalarına uygun */
void  I_InitMusic(void)                        {}
void  I_ShutdownMusic(void)                    {}
void  I_SetMusicVolume(int vol)                { (void)vol; }
void  I_PauseSong(void)                        {}
void  I_ResumeSong(void)                       {}
void *I_RegisterSong(void *data, int len)      { (void)data; (void)len; return data; }
void  I_PlaySong(void *handle, boolean loop)   { (void)handle; (void)loop; }
void  I_StopSong(void)                         {}
void  I_UnRegisterSong(void *handle)           { (void)handle; }


/* ── Eksik sembol stub'ları ─────────────────────────────────────
 * Linker'ın istediği ama kullanmadığımız fonksiyonlar.        */
void     I_BindSoundVariables(void)                        {}
void     I_PrecacheSounds(sfxinfo_t *sounds, int num)      { (void)sounds; (void)num; }
int      I_GetSfxLumpNum(sfxinfo_t *sfxinfo)               { (void)sfxinfo; return -1; }
boolean  I_MusicIsPlaying(void)                            { return false; }
int      snd_musicdevice = 0;
int      snd_sfxdevice   = 3;
void     StatCopy(void *ep)                                { (void)ep; }
void     StatDump(void)                                    {}

int main(int argc, char **argv) {
    _puts("[DOOM v12] main\n");
    doomgeneric_Create(argc, argv);
    // doomgeneric_Create içinde M_LoadDefaults() doom.cfg'yi okur ve
    // key binding'leri override edebilir. Burada tekrar set ediyoruz.
    key_fire        = 0x9d;  // Ctrl
    key_use         = 0x20;  // Space
    key_speed       = 0xb6;  // Shift
    key_up          = 0xad;
    key_down        = 0xaf;
    key_left        = 0xac;
    key_right       = 0xae;
    key_strafeleft  = ',';
    key_straferight = '.';
    _puts("[DOOM v12] loop\n");
    for (;;) doomgeneric_Tick();
    return 0;
}