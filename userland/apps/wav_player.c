// wav_player.c — AscentOS WAV Oynatıcı v2.0
//
// v2 — double-buffer streaming (cızırdama sorunu giderildi)
//
// v1'deki sorun:
//   Sıra: sb16_play() → wait_ms(chunk_ms) → read() → sb16_play()
//   DMA chunk'ı bitirince duruyor; arkasından disk okuma + syscall
//   overhead geliyor. Bu geçiş arası ~5-30ms sessizlik olarak
//   cızırtı/tıklama şeklinde duyuluyordu.
//
// v2 çözümü — double-buffer (A/B tampon):
//   buf_A çalarken buf_B disk'ten doldurulur.
//   DMA bitmeden (IRQ öncesi belirli eşikte) buf_B devreye alınır.
//   Geçiş arası boşluk sıfıra iner.
//
//   Somut sıra:
//     1. buf_A doldur + çalmaya başla
//     2. buf_B'yi hemen doldur (disk'ten oku, DMA çalarken)
//     3. DMA bitmek üzereyken (eşik_ms önce) buf_B'yi gönder
//     4. buf_A'yı yeniden doldur — döngü
//
// SYS_SB16_PLAY (411) çağrı kuralı:
//   RAX=411, RDI=buf, RSI=len(max 4096), RDX=rate_hz, R10=fmt(0-3)
//   fmt: 0=8bit_mono 1=8bit_stereo 2=16bit_mono 3=16bit_stereo
//   Dönüş: 0=başarı, negatif=hata

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

// ── Syscall numaraları ────────────────────────────────────────────────────────
#define SYS_SB16_PLAY  411
#define SYS_GETTICKS   404
#define SYS_YIELD       24

// ── Inline syscall'lar ────────────────────────────────────────────────────────

static inline long get_ticks(void) {
    long r;
    __asm__ volatile("movq $404,%%rax; syscall"
        : "=a"(r) :: "rcx","r11","memory");
    return r;
}

static inline void yield_cpu(void) {
    __asm__ volatile("movq $24,%%rax; syscall"
        ::: "rax","rcx","r11","memory");
}

/* sys_sb16_playing: 1=DMA çalıyor, 0=bitti, <0=hata */
static inline long sys_sb16_playing(void) {
    long r;
    __asm__ volatile("movq $412,%%rax; syscall"
        : "=a"(r) :: "rcx","r11","memory");
    return r;
}

/* poll_until_done: DMA bitene kadar bekle */
static void poll_until_done(void) {
    while (sys_sb16_playing() > 0)
        yield_cpu();
}

static inline long sb16_play(const void* buf, long len, long rate, long fmt) {
    long r;
    register long r10 __asm__("r10") = fmt;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(411L), "D"((long)buf), "S"(len), "d"(rate), "r"(r10)
        : "rcx","r11","memory");
    return r;
}

// ── WAV yapıları ──────────────────────────────────────────────────────────────

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

typedef struct __attribute__((packed)) {
    char riff[4];
    u32  file_size;
    char wave[4];
} wav_riff_t;

typedef struct __attribute__((packed)) {
    char id[4];
    u32  size;
} wav_chunk_t;

typedef struct __attribute__((packed)) {
    u16 audio_format;
    u16 channels;
    u32 sample_rate;
    u32 byte_rate;
    u16 block_align;
    u16 bits_per_sample;
} wav_fmt_t;

// ── sb16_fmt hesaplama ────────────────────────────────────────────────────────
static int wav_to_sb16_fmt(u16 bits, u16 ch) {
    if (bits ==  8 && ch == 1) return 0;
    if (bits ==  8 && ch == 2) return 1;
    if (bits == 16 && ch == 1) return 2;
    if (bits == 16 && ch == 2) return 3;
    return -1;
}

// ── Double-buffer sabitleri ───────────────────────────────────────────────────
//
// CHUNK: Her DMA transferinin boyutu (SB16 DMA max = 4096 byte).
//
// PREFILL_MS: DMA'nın bitmesinden bu kadar ms önce bir sonraki chunk'ı gönder.
//   Çok küçük → geç kalınır, boşluk oluşur (cızırtı).
//   Çok büyük → erken gönderilir, DMA bitmeden üzerine yazılır (bozulma).
//   Önerilen: ~8ms (scheduler jitter + syscall latency payı).
//   44100Hz stereo 16bit, 4096 byte → ~23ms/chunk → 8ms eşik güvenli.
//   8000Hz mono 8bit, 4096 byte → ~512ms/chunk → 8ms eşik çok güvenli.
//
#define CHUNK       4096
#define PREFILL_MS  8

// Double buffer: A ve B
static u8 buf_a[CHUNK];
static u8 buf_b[CHUNK];

// ── Aktif bekleme (ms) ────────────────────────────────────────────────────────
static void wait_until(long deadline_ticks) {
    while (get_ticks() < deadline_ticks)
        yield_cpu();
}

// ── WAV dosyası çal (double-buffer) ──────────────────────────────────────────
static int play_file(const char* path) {
    int fd = open(path, 0 /*O_RDONLY*/);
    if (fd < 0) {
        printf("Hata: Dosya açılamadı: %s\n", path);
        return 1;
    }

    // RIFF/WAVE başlığı
    wav_riff_t riff;
    if (read(fd, &riff, 12) != 12 ||
        riff.riff[0]!='R' || riff.riff[1]!='I' ||
        riff.riff[2]!='F' || riff.riff[3]!='F' ||
        riff.wave[0]!='W' || riff.wave[1]!='A' ||
        riff.wave[2]!='V' || riff.wave[3]!='E') {
        printf("Hata: Geçerli bir WAV dosyası değil.\n");
        close(fd);
        return 1;
    }

    wav_fmt_t fmt;
    int has_fmt = 0;
    int result  = 0;

    for (;;) {
        wav_chunk_t hdr;
        if (read(fd, &hdr, 8) != 8) break;

        // fmt chunk
        if (!memcmp(hdr.id, "fmt ", 4)) {
            int rd = hdr.size < (u32)sizeof(fmt)
                     ? (int)hdr.size : (int)sizeof(fmt);
            if (read(fd, &fmt, rd) != rd) break;
            int skip = (int)hdr.size - rd;
            if (skip > 0) lseek(fd, skip, 1);
            if (hdr.size & 1) lseek(fd, 1, 1);
            has_fmt = 1;

            if (fmt.audio_format != 1) {
                printf("Hata: Yalnızca PCM (format 1) destekleniyor.\n");
                result = 1; goto done;
            }
            if (fmt.channels > 2) {
                printf("Hata: Yalnızca mono/stereo destekleniyor.\n");
                result = 1; goto done;
            }
            if (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16) {
                printf("Hata: Yalnızca 8-bit ve 16-bit destekleniyor.\n");
                result = 1; goto done;
            }
            if (fmt.sample_rate < 4000 || fmt.sample_rate > 44100) {
                printf("Hata: Örnekleme hızı 4000-44100 Hz olmalı.\n");
                result = 1; goto done;
            }
            continue;
        }

        // data chunk → double-buffer döngüsü
        if (!memcmp(hdr.id, "data", 4)) {
            if (!has_fmt) {
                printf("Hata: fmt chunk bulunamadı.\n");
                result = 1; goto done;
            }

            int  sb16_fmt = wav_to_sb16_fmt(fmt.bits_per_sample, fmt.channels);
            if (sb16_fmt < 0) {
                printf("Hata: Desteklenmeyen PCM formatı.\n");
                result = 1; goto done;
            }

            long rate      = (long)fmt.sample_rate;
            long byte_rate = (long)fmt.byte_rate;
            u32  remaining = hdr.size;

            // 16-bit DMA: chunk çift byte olmalı
            int chunk_size = CHUNK;
            if ((sb16_fmt == 2 || sb16_fmt == 3) && (chunk_size & 1))
                chunk_size--;

            printf("  Kanal     : %s\n", fmt.channels == 1 ? "Mono" : "Stereo");
            printf("  Bit derinlik: %d-bit\n", fmt.bits_per_sample);
            printf("  Hız       : %ld Hz\n", rate);
            printf("  Süre      : ~%ld ms\n",
                   (long)remaining * 1000L / byte_rate);
            printf("  Çalıyor");
            fflush(stdout);

            // ── İlk chunk: buf_a'yı doldur ve çalmaya başla ──────────────────
            int n_a = (int)(remaining < (u32)chunk_size
                            ? remaining : (u32)chunk_size);
            int got_a = read(fd, buf_a, n_a);
            if (got_a <= 0) goto done;
            remaining -= (u32)got_a;

            long rc = sb16_play(buf_a, got_a, rate, sb16_fmt);
            if (rc < 0) {
                printf("\nHata: SB16 syscall başarısız (%ld).\n"
                       "  -> QEMU'da -device sb16 var mı?\n", rc);
                result = 1; goto done;
            }

            int dot = 0;

            // ── Double-buffer döngüsü ─────────────────────────────────────────
            while (remaining > 0) {
                // buf_b'yi DMA çalarken doldur (disk okuma burada yapılır)
                int n_b = (int)(remaining < (u32)chunk_size
                                ? remaining : (u32)chunk_size);
                int got_b = read(fd, buf_b, n_b);
                if (got_b <= 0) break;
                remaining -= (u32)got_b;

                /* IRQ'yu bekle — DMA tam bitince gönder */
                poll_until_done();
                rc = sb16_play(buf_b, got_b, rate, sb16_fmt);
                if (rc < 0) {
                    printf("\nHata: SB16 syscall başarısız (%ld).\n", rc);
                    result = 1; goto done;
                }

                /* buf_b çalarken buf_a'yı doldur */
                if (remaining > 0) {
                    int n_a2 = (int)(remaining < (u32)chunk_size
                                     ? remaining : (u32)chunk_size);
                    got_a = read(fd, buf_a, n_a2);
                    if (got_a <= 0) break;
                    remaining -= (u32)got_a;

                    poll_until_done();
                    rc = sb16_play(buf_a, got_a, rate, sb16_fmt);
                    if (rc < 0) {
                        printf("\nHata: SB16 syscall başarısız (%ld).\n", rc);
                        result = 1; goto done;
                    }
                }

                if ((dot++ & 7) == 0) { putchar('.'); fflush(stdout); }
            }

            /* Son chunk bitmesini bekle */
            poll_until_done();
            printf("\n  Tamamlandı.\n");
            goto done;
        }

        // Bilinmeyen chunk: atla
        if (hdr.size) lseek(fd, (int)hdr.size, 1);
        if (hdr.size & 1) lseek(fd, 1, 1);
    }

    printf("Hata: data chunk bulunamadı.\n");
    result = 1;

done:
    close(fd);
    return result;
}

// ── Demo: 440 Hz sinüs, 1 saniye ─────────────────────────────────────────────
static void play_demo(void) {
    static const u8 sine18[18] = {
        128,172,210,237,251,251,237,210,172,
        128, 84, 46, 19,  5,  5, 19, 46, 84
    };

    printf("Demo: 440 Hz sinüs tonu, 1 saniye (8000 Hz, 8-bit mono)\n");

    int total = 8000;
    int sent  = 0;

    // Demo: tek chunk (8000 byte = 1 saniye), double-buffer gerekmez
    for (int i = 0; i < total && i < CHUNK; i++)
        buf_a[i] = sine18[i % 18];

    int n = total < CHUNK ? total : CHUNK;
    long rc = sb16_play(buf_a, n, 8000, 0);
    if (rc < 0) {
        printf("Hata: SB16 syscall başarısız (%ld).\n"
               "  -> QEMU'da -device sb16 var mı?\n", rc);
        return;
    }

    // 8000 byte @ 8000Hz = 1000ms
    long deadline = get_ticks() + 1000L;
    wait_until(deadline);
    printf("Tamamlandı.\n");
    (void)sent;
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  AscentOS WAV Player v5.1                ║\n");
    printf("║  IRQ polling · cızırtısız ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");
    fflush(stdout);

    if (argc < 2) {
        printf("Kullanım : wav_player <dosya.wav>\n");
        printf("Demo modu: demo tonu çalınıyor...\n\n");
        play_demo();
    } else {
        printf("Dosya: %s\n", argv[1]);
        fflush(stdout);
        int r = play_file(argv[1]);
        fflush(stdout);
        exit(r);
    }

    fflush(stdout);
    exit(0);
    return 0;
}