// i_ascentos_sound.c — AscentOS OSS /dev/dsp sound backend for doomgeneric
// Uses SB16 via standard OSS ioctls on /dev/dsp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

// ── OSS /dev/dsp ioctls ────────────────────────────────────────────────────
#define SNDCTL_DSP_SPEED    0xC0045002
#define SNDCTL_DSP_STEREO   0xC0045003
#define SNDCTL_DSP_SETFMT   0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006

#define AFMT_U8     0x00000008
#define AFMT_S16_LE 0x00000010

// ── Sound device ────────────────────────────────────────────────────────────
static int dsp_fd = -1;

// Doom uses 11025 Hz mono 8-bit for sound effects
#define DOOM_SAMPLERATE 11025

// ── Sound channel state ────────────────────────────────────────────────────
#define MAX_CHANNELS 8

typedef struct {
    // Sound data
    uint8_t *data;
    int length;
    int sample_rate;

    // Playback state
    int playing;
    int position;
    int volume;      // 0-127
    int sep;         // 0-254 (stereo separation)

    // Resampled output buffer (8-bit mono → 16-bit stereo for /dev/dsp)
    int16_t *resampled;
    int resampled_len;
} channel_t;

static channel_t channels[MAX_CHANNELS];

// ── Helper: write a chunk to /dev/dsp, blocking until DMA completes ────────
static void dsp_write_chunk(const void *data, int len) {
    if (dsp_fd < 0 || len <= 0) return;
    write(dsp_fd, data, len);
}

// ── Resample and mix all active channels into a single output buffer ────────
// We output at 22050 Hz stereo 16-bit (SB16 native) and let the SB16 play it.
// This gives us enough bandwidth for 11025 Hz mono sound effects.

#define OUTPUT_SAMPLERATE 22050
#define OUTPUT_CHANNELS   2
#define OUTPUT_BITS       16
#define OUTPUT_CHUNK_SAMPLES 1024
#define OUTPUT_CHUNK_BYTES (OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS * (OUTPUT_BITS / 8))

// Ring buffer for non-blocking audio (holds ~200ms of audio)
#define RING_BUFFER_SIZE (OUTPUT_SAMPLERATE * OUTPUT_CHANNELS * sizeof(int16_t) / 5)

static int16_t mix_buffer[OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS];
static uint8_t ring_buffer[RING_BUFFER_SIZE];
static int ring_read_pos = 0;
static int ring_write_pos = 0;
static int ring_data_len = 0;

// ── Initialize sound subsystem ─────────────────────────────────────────────
static boolean I_AscentOS_InitSound(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;

    dsp_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (dsp_fd < 0) {
        printf("I_AscentOS_InitSound: Cannot open /dev/dsp (sound disabled)\n");
        return false;
    }

    // Configure /dev/dsp: 22050 Hz, 16-bit signed LE, stereo
    int rate = OUTPUT_SAMPLERATE;
    if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &rate) < 0) {
        printf("I_AscentOS_InitSound: SNDCTL_DSP_SPEED failed\n");
    }

    int fmt = AFMT_S16_LE;
    if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &fmt) < 0) {
        printf("I_AscentOS_InitSound: SNDCTL_DSP_SETFMT failed\n");
    }

    int num_ch = OUTPUT_CHANNELS;
    if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &num_ch) < 0) {
        printf("I_AscentOS_InitSound: SNDCTL_DSP_CHANNELS failed\n");
    }

    printf("I_AscentOS_InitSound: /dev/dsp opened (%d Hz, %d ch, %d-bit)\n",
           rate, num_ch, OUTPUT_BITS);

    memset(channels, 0, sizeof(channels));
    return true;
}

// ── Shutdown ───────────────────────────────────────────────────────────────
static void I_AscentOS_ShutdownSound(void) {
    if (dsp_fd >= 0) {
        close(dsp_fd);
        dsp_fd = -1;
    }
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (channels[i].resampled) {
            free(channels[i].resampled);
            channels[i].resampled = NULL;
        }
    }
}

// ── Get the lump index for a sound effect ──────────────────────────────────
static int I_AscentOS_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[9];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfxinfo->name);
    return W_GetNumForName(namebuf);
}

// ── Resample a sound from its native rate to the output rate ────────────────
// Input: 8-bit unsigned mono at `in_rate`
// Output: 16-bit signed stereo at OUTPUT_SAMPLERATE
static void resample_sound(channel_t *ch) {
    if (ch->data == NULL || ch->length <= 0) {
        ch->resampled = NULL;
        ch->resampled_len = 0;
        return;
    }

    // Calculate output length
    double ratio = (double)OUTPUT_SAMPLERATE / (double)ch->sample_rate;
    int out_samples = (int)(ch->length * ratio);
    if (out_samples <= 0) {
        ch->resampled = NULL;
        ch->resampled_len = 0;
        return;
    }

    // Allocate stereo 16-bit output
    ch->resampled = (int16_t *)malloc(out_samples * OUTPUT_CHANNELS * sizeof(int16_t));
    if (!ch->resampled) {
        ch->resampled_len = 0;
        return;
    }
    ch->resampled_len = out_samples * OUTPUT_CHANNELS * sizeof(int16_t);

    // Volume scale (0-127 → 0.0-1.0)
    double vol_scale = ch->volume / 127.0;

    // Stereo separation: sep 0 = full left, 128 = center, 254 = full right
    double left_scale  = vol_scale * (1.0 - (double)ch->sep / 254.0);
    double right_scale = vol_scale * ((double)ch->sep / 254.0);

    // Nearest-neighbor resample with 8-bit unsigned → 16-bit signed stereo
    for (int i = 0; i < out_samples; i++) {
        int src_idx = (int)(i / ratio);
        if (src_idx >= ch->length) src_idx = ch->length - 1;

        // Convert unsigned 8-bit (0-255, center=128) to signed 16-bit
        int16_t sample = (int16_t)((ch->data[src_idx] - 128) * 256);

        ch->resampled[i * 2]     = (int16_t)(sample * left_scale);   // left
        ch->resampled[i * 2 + 1] = (int16_t)(sample * right_scale);  // right
    }
}

// ── Add data to ring buffer ────────────────────────────────────────────────
static void ring_buffer_write(const uint8_t *data, int len) {
    if (len <= 0) return;
    
    // Don't overflow the ring buffer
    int available = RING_BUFFER_SIZE - ring_data_len;
    if (len > available) len = available;
    if (len <= 0) return;
    
    // Write in two parts if wrapping around
    int first_part = RING_BUFFER_SIZE - ring_write_pos;
    if (first_part > len) first_part = len;
    
    memcpy(ring_buffer + ring_write_pos, data, first_part);
    ring_write_pos = (ring_write_pos + first_part) % RING_BUFFER_SIZE;
    ring_data_len += first_part;
    
    if (first_part < len) {
        int second_part = len - first_part;
        memcpy(ring_buffer + ring_write_pos, data + first_part, second_part);
        ring_write_pos = (ring_write_pos + second_part) % RING_BUFFER_SIZE;
        ring_data_len += second_part;
    }
}

// ── Read data from ring buffer ──────────────────────────────────────────────
static int ring_buffer_read(uint8_t *data, int max_len) {
    if (ring_data_len <= 0) return 0;
    
    int len = (ring_data_len < max_len) ? ring_data_len : max_len;
    if (len <= 0) return 0;
    
    // Read in two parts if wrapping around
    int first_part = RING_BUFFER_SIZE - ring_read_pos;
    if (first_part > len) first_part = len;
    
    memcpy(data, ring_buffer + ring_read_pos, first_part);
    ring_read_pos = (ring_read_pos + first_part) % RING_BUFFER_SIZE;
    ring_data_len -= first_part;
    
    if (first_part < len) {
        int second_part = len - first_part;
        memcpy(data + first_part, ring_buffer + ring_read_pos, second_part);
        ring_read_pos = (ring_read_pos + second_part) % RING_BUFFER_SIZE;
        ring_data_len -= second_part;
    }
    
    return len;
}

// ── Update (mix and push audio) ────────────────────────────────────────────
static void I_AscentOS_UpdateSound(void) {
    if (dsp_fd < 0) return;

    // First, try to drain the ring buffer to the device (non-blocking)
    if (ring_data_len > 0) {
        uint8_t temp_buf[OUTPUT_CHUNK_BYTES];
        int to_write = ring_buffer_read(temp_buf, OUTPUT_CHUNK_BYTES);
        if (to_write > 0) {
            ssize_t written = write(dsp_fd, temp_buf, to_write);
            (void)written;  // Ignore partial writes - data stays in ring buffer
        }
    }

    // Mix active channels into mix_buffer
    memset(mix_buffer, 0, sizeof(mix_buffer));

    int active_channels = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!channels[i].playing) continue;
        active_channels++;

        int16_t *src = channels[i].resampled;
        int src_bytes = channels[i].resampled_len;
        int pos = channels[i].position;

        // Mix this channel into the mix buffer
        int samples_to_mix = OUTPUT_CHUNK_SAMPLES;
        int remaining = (src_bytes - pos) / (int)sizeof(int16_t);
        if (remaining < samples_to_mix * OUTPUT_CHANNELS) {
            // Sound will finish this chunk
            samples_to_mix = remaining / OUTPUT_CHANNELS;
            if (samples_to_mix <= 0) {
                channels[i].playing = 0;
                continue;
            }
        }

        for (int s = 0; s < samples_to_mix; s++) {
            mix_buffer[s * 2]     += src[(pos / 2) + s * 2];
            mix_buffer[s * 2 + 1] += src[(pos / 2) + s * 2 + 1];
        }

        channels[i].position += samples_to_mix * OUTPUT_CHANNELS * sizeof(int16_t);

        if (channels[i].position >= src_bytes) {
            channels[i].playing = 0;
        }
    }

    // Always push mixed audio to ring buffer (even if no active channels - outputs silence)
    ring_buffer_write((uint8_t *)mix_buffer, OUTPUT_CHUNK_BYTES);
}

// ── Update sound params (volume/separation) ────────────────────────────────
static void I_AscentOS_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= MAX_CHANNELS) return;
    channels[channel].volume = vol;
    channels[channel].sep = sep;

    // Re-resample with new volume/sep if the sound is playing
    if (channels[channel].playing) {
        int saved_pos = channels[channel].position;
        resample_sound(&channels[channel]);
        channels[channel].position = saved_pos;
    }
}

// ── Start a sound ──────────────────────────────────────────────────────────
static int I_AscentOS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (channel < 0 || channel >= MAX_CHANNELS) return -1;

    // Free old resampled data
    if (channels[channel].resampled) {
        free(channels[channel].resampled);
        channels[channel].resampled = NULL;
    }

    // Load the sound lump
    int lumpnum = I_AscentOS_GetSfxLumpNum(sfxinfo);
    if (lumpnum < 0) return -1;

    // Get lump data (skip first 8 bytes = Doom sound header)
    int lumpsize = W_LumpLength(lumpnum);
    if (lumpsize <= 8) return -1;

    uint8_t *lumpdata = (uint8_t *)W_CacheLumpNum(lumpnum, PU_STATIC);

    // Parse Doom sound header: offset 2 = sample rate (uint16), offset 4 = data length (uint32)
    uint16_t sample_rate = lumpdata[2] | (lumpdata[3] << 8);
    if (sample_rate == 0) sample_rate = DOOM_SAMPLERATE;

    uint32_t data_len = lumpdata[4] | (lumpdata[5] << 8) |
                        (lumpdata[6] << 16) | (lumpdata[7] << 24);
    if (data_len == 0 || data_len > (uint32_t)(lumpsize - 8))
        data_len = lumpsize - 8;

    channels[channel].data = lumpdata + 8;
    channels[channel].length = (int)data_len;
    channels[channel].sample_rate = sample_rate;
    channels[channel].volume = vol;
    channels[channel].sep = sep;
    channels[channel].position = 0;
    channels[channel].playing = 1;

    // Resample to output format
    resample_sound(&channels[channel]);

    return channel;
}

// ── Stop a sound ───────────────────────────────────────────────────────────
static void I_AscentOS_StopSound(int channel) {
    if (channel < 0 || channel >= MAX_CHANNELS) return;
    channels[channel].playing = 0;
}

// ── Check if sound is playing ──────────────────────────────────────────────
static boolean I_AscentOS_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= MAX_CHANNELS) return false;
    return channels[channel].playing;
}

// ── Cache sounds (no-op, we load on demand) ────────────────────────────────
static void I_AscentOS_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds;
    (void)num_sounds;
}

// ── Symbols required by i_sound.c when FEATURE_SOUND is defined ────────────
int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

// ── Supported sound devices (SB16 via OSS /dev/dsp) ────────────────────────
static snddevice_t ascentos_sound_devices[] = {
    SNDDEVICE_SB,
};

// ── The sound module descriptor ────────────────────────────────────────────
sound_module_t DG_sound_module = {
    .sound_devices    = ascentos_sound_devices,
    .num_sound_devices = 1,
    .Init             = I_AscentOS_InitSound,
    .Shutdown         = I_AscentOS_ShutdownSound,
    .GetSfxLumpNum    = I_AscentOS_GetSfxLumpNum,
    .Update           = I_AscentOS_UpdateSound,
    .UpdateSoundParams = I_AscentOS_UpdateSoundParams,
    .StartSound       = I_AscentOS_StartSound,
    .StopSound        = I_AscentOS_StopSound,
    .SoundIsPlaying   = I_AscentOS_SoundIsPlaying,
    .CacheSounds      = I_AscentOS_CacheSounds,
};

// ── Music stubs (no MIDI support on SB16 yet) ─────────────────────────────
// We provide a minimal music_module that does nothing so the engine doesn't crash.

static boolean I_AscentOS_InitMusic(void)  { return false; }
static void I_AscentOS_ShutdownMusic(void) { }
static void I_AscentOS_SetMusicVolume(int volume) { (void)volume; }
static void I_AscentOS_PauseMusic(void)    { }
static void I_AscentOS_ResumeMusic(void)   { }
static void *I_AscentOS_RegisterSong(void *data, int len) { (void)data; (void)len; return NULL; }
static void I_AscentOS_UnRegisterSong(void *handle) { (void)handle; }
static void I_AscentOS_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
static void I_AscentOS_StopSong(void)      { }
static boolean I_AscentOS_MusicIsPlaying(void) { return false; }
static void I_AscentOS_PollMusic(void)     { }

music_module_t DG_music_module = {
    .sound_devices     = NULL,
    .num_sound_devices = 0,
    .Init              = I_AscentOS_InitMusic,
    .Shutdown          = I_AscentOS_ShutdownMusic,
    .SetMusicVolume    = I_AscentOS_SetMusicVolume,
    .PauseMusic        = I_AscentOS_PauseMusic,
    .ResumeMusic       = I_AscentOS_ResumeMusic,
    .RegisterSong      = I_AscentOS_RegisterSong,
    .UnRegisterSong    = I_AscentOS_UnRegisterSong,
    .PlaySong          = I_AscentOS_PlaySong,
    .StopSong          = I_AscentOS_StopSong,
    .MusicIsPlaying    = I_AscentOS_MusicIsPlaying,
    .Poll              = I_AscentOS_PollMusic,
};
