// i_ascentos_sound_midiplay.c — AscentOS OSS /dev/dsp sound backend for
// doomgeneric Uses SB16 via standard OSS ioctls on /dev/dsp Uses midiplay
// library for MUS/MIDI music playback with OPL3 emulation

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

// midiplay library
#include "midiplay.h"

// ── OSS /dev/dsp ioctls ────────────────────────────────────────────────────
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006

#define AFMT_U8 0x00000008
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
  int position; // Output sample playback position
  int volume;   // 0-127
  int sep;      // 0-254 (stereo separation)
} channel_t;

static channel_t channels[MAX_CHANNELS];

// ── Output audio format ─────────────────────────────────────────────────────
#define OUTPUT_SAMPLERATE 48000
#define OUTPUT_CHANNELS 2
#define OUTPUT_BITS 16
#define OUTPUT_CHUNK_SAMPLES 1024
#define OUTPUT_CHUNK_BYTES                                                     \
  (OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS * (OUTPUT_BITS / 8))

// Ring buffer for non-blocking audio (holds ~200ms of audio)
#define RING_BUFFER_SIZE                                                       \
  (OUTPUT_SAMPLERATE * OUTPUT_CHANNELS * sizeof(int16_t) / 5)

static int32_t mix_buffer[OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS];
static uint8_t ring_buffer[RING_BUFFER_SIZE];
static int ring_read_pos = 0;
static int ring_write_pos = 0;
static int ring_data_len = 0;

// ── Music state ─────────────────────────────────────────────────────────────
static int music_initialized = 0;
static int music_playing = 0;
static int music_looping = 0;
static int music_volume = 127;
static char *genmidi_data = NULL;

// ── Initialize sound subsystem ─────────────────────────────────────────────
static boolean I_AscentOS_InitSound(boolean use_sfx_prefix) {
  (void)use_sfx_prefix;

  dsp_fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
  if (dsp_fd < 0) {
    printf("I_AscentOS_InitSound: Cannot open /dev/dsp (sound disabled)\n");
    return false;
  }

  // Configure /dev/dsp: 48000 Hz, 16-bit signed LE, stereo
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

  printf("I_AscentOS_InitSound: /dev/dsp opened (%d Hz, %d ch, %d-bit)\n", rate,
         num_ch, OUTPUT_BITS);

  memset(channels, 0, sizeof(channels));
  return true;
}

// ── Shutdown ───────────────────────────────────────────────────────────────
static void I_AscentOS_ShutdownSound(void) {
  if (dsp_fd >= 0) {
    close(dsp_fd);
    dsp_fd = -1;
  }
  if (genmidi_data) {
    Z_Free(genmidi_data);
    genmidi_data = NULL;
  }
}

// ── Get the lump index for a sound effect ──────────────────────────────────
static int I_AscentOS_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
  char namebuf[9];
  snprintf(namebuf, sizeof(namebuf), "ds%s", sfxinfo->name);
  return W_GetNumForName(namebuf);
}

// ── Add data to ring buffer ────────────────────────────────────────────────
static void ring_buffer_write(const uint8_t *data, int len) {
  if (len <= 0)
    return;

  // Don't overflow the ring buffer
  int available = RING_BUFFER_SIZE - ring_data_len;
  if (len > available)
    len = available;
  if (len <= 0)
    return;

  // Write in two parts if wrapping around
  int first_part = RING_BUFFER_SIZE - ring_write_pos;
  if (first_part > len)
    first_part = len;

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

// ── Peek data from ring buffer without consuming ───────────────────────────
static int ring_buffer_peek(uint8_t *data, int max_len) {
  if (ring_data_len <= 0)
    return 0;

  int len = (ring_data_len < max_len) ? ring_data_len : max_len;
  if (len <= 0)
    return 0;

  int first_part = RING_BUFFER_SIZE - ring_read_pos;
  if (first_part > len)
    first_part = len;

  memcpy(data, ring_buffer + ring_read_pos, first_part);

  if (first_part < len) {
    int second_part = len - first_part;
    memcpy(data + first_part, ring_buffer, second_part);
  }

  return len;
}

// ── Advance ring buffer read pointer ───────────────────────────────────────
static void ring_buffer_advance(int len) {
  if (len <= 0)
    return;
  if (len > ring_data_len)
    len = ring_data_len;
  ring_read_pos = (ring_read_pos + len) % RING_BUFFER_SIZE;
  ring_data_len -= len;
}

// ── Generate music samples using midiplay ──────────────────────────────────
static void generate_music_samples(int16_t *out_samples,
                                   int num_stereo_frames) {
  if (!music_playing || !music_initialized)
    return;

  for (int i = 0; i < num_stereo_frames; i++) {
    int sample[2];
    Midiplay_Output(sample);
    // Clamp to 16-bit
    if (sample[0] > 32767)
      sample[0] = 32767;
    if (sample[0] < -32768)
      sample[0] = -32768;
    if (sample[1] > 32767)
      sample[1] = 32767;
    if (sample[1] < -32768)
      sample[1] = -32768;
    out_samples[i * 2] = (int16_t)sample[0];
    out_samples[i * 2 + 1] = (int16_t)sample[1];
  }
}

// ── Update (mix and push audio) ────────────────────────────────────────────
static void I_AscentOS_UpdateSound(void) {
  if (dsp_fd < 0)
    return;

  // First, try to drain the ring buffer to the device (non-blocking)
  if (ring_data_len > 0) {
    uint8_t temp_buf[RING_BUFFER_SIZE];
    int to_write = ring_buffer_peek(temp_buf, RING_BUFFER_SIZE);
    if (to_write > 0) {
      ssize_t written = write(dsp_fd, temp_buf, to_write);
      if (written > 0) {
        ring_buffer_advance((int)written);
      }
    }
  }

  // Only mix new audio if the ring buffer has enough space for a full chunk
  if (RING_BUFFER_SIZE - ring_data_len < OUTPUT_CHUNK_BYTES) {
    return;
  }

  // Mix active channels into mix_buffer
  memset(mix_buffer, 0, sizeof(mix_buffer));

  int active_channels = 0;
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!channels[i].playing)
      continue;
    active_channels++;

    int pos = channels[i].position; // current output sample index

    // Convert parameters to scale
    double vol_scale = channels[i].volume / 127.0;
    double left_scale = vol_scale * (1.0 - (double)channels[i].sep / 254.0);
    double right_scale = vol_scale * ((double)channels[i].sep / 254.0);
    int l_fact = (int)(left_scale * 256);
    int r_fact = (int)(right_scale * 256);

    int samples_to_mix = OUTPUT_CHUNK_SAMPLES;
    uint32_t s_rate = channels[i].sample_rate;

    uint32_t src_pos = ((uint64_t)pos * s_rate) / OUTPUT_SAMPLERATE;
    uint32_t src_frac = ((uint64_t)pos * s_rate) % OUTPUT_SAMPLERATE;

    for (int s = 0; s < samples_to_mix; s++) {
      if (src_pos >= (uint32_t)channels[i].length) {
        channels[i].playing = 0;
        break;
      }

      int raw_sample = ((int)channels[i].data[src_pos] - 128); // -128 to 127

      mix_buffer[s * 2] += (int32_t)(raw_sample * l_fact);
      mix_buffer[s * 2 + 1] += (int32_t)(raw_sample * r_fact);

      src_frac += s_rate;
      while (src_frac >= OUTPUT_SAMPLERATE) {
        src_frac -= OUTPUT_SAMPLERATE;
        src_pos++;
      }
      channels[i].position++;
    }
  }

  // Mix in music samples from midiplay
  if (music_playing && music_initialized) {
    int16_t music_samples[OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS];
    generate_music_samples(music_samples, OUTPUT_CHUNK_SAMPLES);

    for (int i = 0; i < OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS; i++) {
      mix_buffer[i] += music_samples[i];
    }
  }

  // Always push mixed audio to ring buffer (even if no active channels -
  // outputs silence)
  int16_t final_buffer[OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS];
  for (int i = 0; i < OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS; i++) {
    int32_t val = mix_buffer[i];
    if (val > 32767)
      val = 32767;
    if (val < -32768)
      val = -32768;
    final_buffer[i] = (int16_t)val;
  }
  ring_buffer_write((uint8_t *)final_buffer, OUTPUT_CHUNK_BYTES);
}

// ── Update sound params (volume/separation) ────────────────────────────────
static void I_AscentOS_UpdateSoundParams(int channel, int vol, int sep) {
  if (channel < 0 || channel >= MAX_CHANNELS)
    return;
  channels[channel].volume = vol;
  channels[channel].sep = sep;
}

// ── Start a sound ──────────────────────────────────────────────────────────
static int I_AscentOS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol,
                                 int sep) {
  if (channel < 0 || channel >= MAX_CHANNELS)
    return -1;

  // Load the sound lump
  int lumpnum = I_AscentOS_GetSfxLumpNum(sfxinfo);
  if (lumpnum < 0)
    return -1;

  // Get lump data (skip first 8 bytes = Doom sound header)
  int lumpsize = W_LumpLength(lumpnum);
  if (lumpsize <= 8)
    return -1;

  uint8_t *lumpdata = (uint8_t *)W_CacheLumpNum(lumpnum, PU_STATIC);

  // Parse Doom sound header: offset 2 = sample rate (uint16), offset 4 = data
  // length (uint32)
  uint16_t sample_rate = lumpdata[2] | (lumpdata[3] << 8);
  if (sample_rate == 0)
    sample_rate = DOOM_SAMPLERATE;

  uint32_t data_len = lumpdata[4] | (lumpdata[5] << 8) | (lumpdata[6] << 16) |
                      (lumpdata[7] << 24);
  if (data_len == 0 || data_len > (uint32_t)(lumpsize - 8))
    data_len = lumpsize - 8;

  channels[channel].data = lumpdata + 8;
  channels[channel].length = (int)data_len;
  channels[channel].sample_rate = sample_rate;
  channels[channel].volume = vol;
  channels[channel].sep = sep;
  channels[channel].position = 0;
  channels[channel].playing = 1;

  return channel;
}

// ── Stop a sound ───────────────────────────────────────────────────────────
static void I_AscentOS_StopSound(int channel) {
  if (channel < 0 || channel >= MAX_CHANNELS)
    return;
  channels[channel].playing = 0;
}

// ── Check if sound is playing ──────────────────────────────────────────────
static boolean I_AscentOS_SoundIsPlaying(int channel) {
  if (channel < 0 || channel >= MAX_CHANNELS)
    return false;
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
int snd_musicdevice = 0;

// Forward declarations for music functions (defined below)
boolean I_AscentOS_InitMusic(void);
void I_AscentOS_ShutdownMusic(void);
void I_AscentOS_SetMusicVolume(int volume);
void I_AscentOS_PauseMusic(void);
void I_AscentOS_ResumeMusic(void);
void *I_AscentOS_RegisterSong(void *data, int len);
void I_AscentOS_UnRegisterSong(void *handle);
void I_AscentOS_PlaySong(void *handle, boolean looping);
void I_AscentOS_StopSong(void);
boolean I_AscentOS_MusicIsPlaying(void);

void I_InitSound(boolean use_sfx_prefix) {
  I_AscentOS_InitSound(use_sfx_prefix);
}
void I_ShutdownSound(void) { I_AscentOS_ShutdownSound(); }
int I_GetSfxLumpNum(sfxinfo_t *sfx) { return I_AscentOS_GetSfxLumpNum(sfx); }
void I_UpdateSound(void) { I_AscentOS_UpdateSound(); }
void I_UpdateSoundParams(int ch, int vol, int sep) {
  I_AscentOS_UpdateSoundParams(ch, vol, sep);
}
int I_StartSound(sfxinfo_t *sfx, int ch, int vol, int sep) {
  return I_AscentOS_StartSound(sfx, ch, vol, sep);
}
void I_StopSound(int ch) { I_AscentOS_StopSound(ch); }
boolean I_SoundIsPlaying(int ch) { return I_AscentOS_SoundIsPlaying(ch); }
void I_PrecacheSounds(sfxinfo_t *s, int n) { I_AscentOS_CacheSounds(s, n); }
void I_InitMusic(void) { I_AscentOS_InitMusic(); }
void I_ShutdownMusic(void) { I_AscentOS_ShutdownMusic(); }
void I_SetMusicVolume(int vol) { I_AscentOS_SetMusicVolume(vol); }
void I_PauseSong(void) { I_AscentOS_PauseMusic(); }
void I_ResumeSong(void) { I_AscentOS_ResumeMusic(); }
void *I_RegisterSong(void *data, int len) {
  return I_AscentOS_RegisterSong(data, len);
}
void I_UnRegisterSong(void *h) { I_AscentOS_UnRegisterSong(h); }
void I_PlaySong(void *h, boolean loop) { I_AscentOS_PlaySong(h, loop); }
void I_StopSong(void) { I_AscentOS_StopSong(); }
boolean I_MusicIsPlaying(void) { return I_AscentOS_MusicIsPlaying(); }
void I_BindSoundVariables(void) {}

// ── Supported sound devices (SB16 via OSS /dev/dsp) ────────────────────────
static snddevice_t ascentos_sound_devices[] = {
    SNDDEVICE_SB,
};

// ── The sound module descriptor ────────────────────────────────────────────
sound_module_t DG_sound_module = {
    .sound_devices = ascentos_sound_devices,
    .num_sound_devices = 1,
    .Init = I_AscentOS_InitSound,
    .Shutdown = I_AscentOS_ShutdownSound,
    .GetSfxLumpNum = I_AscentOS_GetSfxLumpNum,
    .Update = I_AscentOS_UpdateSound,
    .UpdateSoundParams = I_AscentOS_UpdateSoundParams,
    .StartSound = I_AscentOS_StartSound,
    .StopSound = I_AscentOS_StopSound,
    .SoundIsPlaying = I_AscentOS_SoundIsPlaying,
    .CacheSounds = I_AscentOS_CacheSounds,
};

// ── Music API implementation using midiplay ────────────────────────────────

// Load GENMIDI lump (OPL instrument definitions)
static int load_genmidi(void) {
  if (genmidi_data)
    return 0; // Already loaded

  int lumpnum = W_GetNumForName("GENMIDI");
  if (lumpnum < 0) {
    printf("I_AscentOS_InitMusic: GENMIDI lump not found\n");
    return -1;
  }

  int lumpsize = W_LumpLength(lumpnum);
  if (lumpsize < GENMIDI_SIZE) {
    printf("I_AscentOS_InitMusic: GENMIDI lump too small (%d < %d)\n", lumpsize,
           GENMIDI_SIZE);
    return -1;
  }

  genmidi_data = (char *)W_CacheLumpNum(lumpnum, PU_STATIC);
  return 0;
}

boolean I_AscentOS_InitMusic(void) {
  // Load GENMIDI instruments
  if (load_genmidi() < 0) {
    return false;
  }

  // Initialize midiplay with our output sample rate
  if (Midiplay_Init(OUTPUT_SAMPLERATE, genmidi_data) != 0) {
    printf("I_AscentOS_InitMusic: Midiplay_Init failed\n");
    return false;
  }

  Midiplay_SetVolume(music_volume);
  music_initialized = 1;
  printf("I_AscentOS_InitMusic: midiplay initialized at %d Hz\n",
         OUTPUT_SAMPLERATE);
  return true;
}

void I_AscentOS_ShutdownMusic(void) {
  if (music_initialized) {
    Midiplay_Play(0);
    music_initialized = 0;
    music_playing = 0;
  }
}

void I_AscentOS_SetMusicVolume(int volume) {
  music_volume = volume;
  if (music_initialized) {
    Midiplay_SetVolume(volume);
  }
}

void I_AscentOS_PauseMusic(void) {
  if (music_initialized) {
    Midiplay_Play(0);
    music_playing = 0;
  }
}

void I_AscentOS_ResumeMusic(void) {
  if (music_initialized && Midiplay_IsPlaying()) {
    Midiplay_Play(1);
    music_playing = 1;
  }
}

void *I_AscentOS_RegisterSong(void *data, int len) {
  if (!music_initialized) {
    return NULL;
  }

  // Load the MUS/MIDI data into midiplay
  if (Midiplay_Load(data, len) != 0) {
    printf("I_AscentOS_RegisterSong: Midiplay_Load failed\n");
    return NULL;
  }

  // Set looping mode
  Midiplay_Loop(music_looping);

  // Return a non-null handle to indicate success
  return (void *)1;
}

void I_AscentOS_UnRegisterSong(void *handle) {
  (void)handle;
  // Nothing to do - midiplay handles this internally
}

void I_AscentOS_PlaySong(void *handle, boolean looping) {
  if (!music_initialized || !handle) {
    return;
  }

  music_looping = looping;
  Midiplay_Loop(looping ? 1 : 0);
  Midiplay_Play(1);
  music_playing = 1;
}

void I_AscentOS_StopSong(void) {
  if (music_initialized) {
    Midiplay_Play(0);
    music_playing = 0;
  }
}

boolean I_AscentOS_MusicIsPlaying(void) {
  if (!music_initialized) {
    return false;
  }
  return Midiplay_IsPlaying() ? true : false;
}
