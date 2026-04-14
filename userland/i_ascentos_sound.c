// i_ascentos_sound.c — AscentOS OSS /dev/dsp sound backend for doomgeneric
// Uses SB16 via standard OSS ioctls on /dev/dsp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <math.h>

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
    int position;    // Output sample playback position
    int volume;      // 0-127
    int sep;         // 0-254 (stereo separation)
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
#define OUTPUT_CHUNK_SAMPLES 630
#define OUTPUT_CHUNK_BYTES (OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS * (OUTPUT_BITS / 8))

// Ring buffer for non-blocking audio (holds ~200ms of audio)
#define RING_BUFFER_SIZE (OUTPUT_SAMPLERATE * OUTPUT_CHANNELS * sizeof(int16_t) / 5)

static int32_t mix_buffer[OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS];
static uint8_t ring_buffer[RING_BUFFER_SIZE];
static int ring_read_pos = 0;
static int ring_write_pos = 0;
static int ring_data_len = 0;

// Forward declaration for music update (defined later)
static void update_music(void);

// Music state struct (needed early for I_AscentOS_UpdateSound)
#define NUM_MUS_CHANNELS 16
#define SAMPLE_BUFFER_SIZE (OUTPUT_SAMPLERATE / 10)

typedef struct {
    int active;
    int instrument;
    int volume;
    int pan;
    int pitch_bend;
    struct {
        int active;
        int note;
        int velocity;
        int64_t start_tick;
        int64_t end_tick;   // -1 = not released yet
        double phase;        // phase accumulator for continuous waveform
        double phase_inc;   // phase increment per sample
        int64_t samples_generated; // samples generated since note start
        int in_release;     // 1 if note is in release phase
        double release_envelope; // current release envelope value
    } notes[8];
} mus_channel_t;

static struct {
    uint8_t *mus_data;
    int mus_len;
    uint8_t *score_ptr;
    uint8_t *score_end;
    int playing;
    int looping;
    int volume;
    int64_t ticks;
    int64_t next_event_tick;
    mus_channel_t channels[NUM_MUS_CHANNELS];
    int16_t *sample_buffer;
    int sample_count;
    int sample_pos;
} music;

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
}

// ── Get the lump index for a sound effect ──────────────────────────────────
static int I_AscentOS_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[9];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfxinfo->name);
    return W_GetNumForName(namebuf);
}

// Dynamic resampling is now done directly during mixing.

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

        int pos = channels[i].position; // current output sample index

        // Convert parameters to scale
        double vol_scale = channels[i].volume / 127.0;
        double left_scale  = vol_scale * (1.0 - (double)channels[i].sep / 254.0);
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
            
            mix_buffer[s * 2]     += (int32_t)(raw_sample * l_fact);
            mix_buffer[s * 2 + 1] += (int32_t)(raw_sample * r_fact);
            
            src_frac += s_rate;
            while (src_frac >= OUTPUT_SAMPLERATE) {
                src_frac -= OUTPUT_SAMPLERATE;
                src_pos++;
            }
            channels[i].position++;
        }
    }

    // Mix in music samples
    if (music.playing && music.sample_buffer) {
        // Poll music to generate more samples if needed
        update_music();
        
        // Mix music samples into mix_buffer
        int music_samples = music.sample_count - music.sample_pos;
        if (music_samples > OUTPUT_CHUNK_SAMPLES) {
            music_samples = OUTPUT_CHUNK_SAMPLES;
        }
        
        if (music_samples > 0) {
            int16_t *music_src = &music.sample_buffer[music.sample_pos * OUTPUT_CHANNELS];
            for (int s = 0; s < music_samples; s++) {
                mix_buffer[s * 2]     += music_src[s * 2];
                mix_buffer[s * 2 + 1] += music_src[s * 2 + 1];
            }
            music.sample_pos += music_samples;
            
            // Reset buffer when exhausted
            if (music.sample_pos >= music.sample_count) {
                music.sample_pos = 0;
                music.sample_count = 0;
            }
        }
    }

    // Always push mixed audio to ring buffer (even if no active channels - outputs silence)
    int16_t final_buffer[OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS];
    for (int i = 0; i < OUTPUT_CHUNK_SAMPLES * OUTPUT_CHANNELS; i++) {
        int32_t val = mix_buffer[i];
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        final_buffer[i] = (int16_t)val;
    }
    ring_buffer_write((uint8_t *)final_buffer, OUTPUT_CHUNK_BYTES);
}

// ── Update sound params (volume/separation) ────────────────────────────────
static void I_AscentOS_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= MAX_CHANNELS) return;
    channels[channel].volume = vol;
    channels[channel].sep = sep;
}

// ── Start a sound ──────────────────────────────────────────────────────────
static int I_AscentOS_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (channel < 0 || channel >= MAX_CHANNELS) return -1;

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

void I_InitSound(boolean use_sfx_prefix)    { I_AscentOS_InitSound(use_sfx_prefix); }
void I_ShutdownSound(void)                  { I_AscentOS_ShutdownSound(); }
int  I_GetSfxLumpNum(sfxinfo_t *sfx)        { return I_AscentOS_GetSfxLumpNum(sfx); }
void I_UpdateSound(void)                    { I_AscentOS_UpdateSound(); }
void I_UpdateSoundParams(int ch, int vol, int sep) { I_AscentOS_UpdateSoundParams(ch, vol, sep); }
int  I_StartSound(sfxinfo_t *sfx, int ch, int vol, int sep) { return I_AscentOS_StartSound(sfx, ch, vol, sep); }
void I_StopSound(int ch)                    { I_AscentOS_StopSound(ch); }
boolean I_SoundIsPlaying(int ch)            { return I_AscentOS_SoundIsPlaying(ch); }
void I_PrecacheSounds(sfxinfo_t *s, int n)  { I_AscentOS_CacheSounds(s, n); }
void I_InitMusic(void)                      { I_AscentOS_InitMusic(); }
void I_ShutdownMusic(void)                  { I_AscentOS_ShutdownMusic(); }
void I_SetMusicVolume(int vol)              { I_AscentOS_SetMusicVolume(vol); }
void I_PauseSong(void)                      { I_AscentOS_PauseMusic(); }
void I_ResumeSong(void)                     { I_AscentOS_ResumeMusic(); }
void *I_RegisterSong(void *data, int len)   { return I_AscentOS_RegisterSong(data, len); }
void I_UnRegisterSong(void *h)              { I_AscentOS_UnRegisterSong(h); }
void I_PlaySong(void *h, boolean loop)      { I_AscentOS_PlaySong(h, loop); }
void I_StopSong(void)                       { I_AscentOS_StopSong(); }
boolean I_MusicIsPlaying(void)              { return I_AscentOS_MusicIsPlaying(); }
void I_BindSoundVariables(void)             { }

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

// ── MUS Music System (software synthesis) ──────────────────────────────────
// Doom uses MUS format, a simplified MIDI. We synthesize in software and mix
// with the SFX output.

// MUS header structure
typedef struct __attribute__((packed)) {
    char    id[4];          // "MUS"\x1a
    uint16_t score_len;
    uint16_t score_start;
    uint16_t channels;      // primary channels (0-15, but 15 = percussion)
    uint16_t sec_channels;
    uint16_t instrument_count;
    uint16_t padding;
} mus_header_t;

// MUS event types (high nibble of first byte)
#define MUS_EV_RELEASE_NOTE    0x00
#define MUS_EV_PLAY_NOTE       0x10
#define MUS_EV_PITCH_BEND      0x20
#define MUS_EV_SYS_EVENT       0x30
#define MUS_EV_CONTROLLER      0x40
#define MUS_EV_END_OF_MEASURE  0x50
#define MUS_EV_FINISH_PLAY     0x60
#define MUS_EV_UNUSED          0x70

// MUS controllers
#define MUS_CTRL_INSTRUMENT    0
#define MUS_CTRL_BANK          1
#define MUS_CTRL_MODULATION    2
#define MUS_CTRL_VOLUME        3
#define MUS_CTRL_PAN           4
#define MUS_CTRL_EXPRESSION    5
#define MUS_CTRL_REVERB        6
#define MUS_CTRL_CHORUS        7
#define MUS_CTRL_SUSTAIN       8
#define MUS_CTRL_SOFT_PEDAL    9

// Precomputed tables for performance
#define SINE_TABLE_SIZE 1024
static double fast_sine_table[SINE_TABLE_SIZE];
static double fast_freq_table[128];
static int tables_initialized = 0;

static unsigned int synth_seed = 1;
static inline int fast_rand(void) {
    synth_seed = synth_seed * 1103515245 + 12345;
    return (unsigned int)(synth_seed / 65536) % 32768;
}

static inline double fast_sin(double phase) {
    if (!tables_initialized) {
        for (int i = 0; i < SINE_TABLE_SIZE; i++) {
            fast_sine_table[i] = sin(2.0 * M_PI * ((double)i / SINE_TABLE_SIZE));
        }
        for (int i = 0; i < 128; i++) {
            fast_freq_table[i] = 440.0 * pow(2.0, (i - 69) / 12.0);
        }
        tables_initialized = 1;
    }
    double p = phase - (int)phase;
    if (p < 0.0) p += 1.0;
    int idx = (int)(p * SINE_TABLE_SIZE);
    if (idx >= SINE_TABLE_SIZE) idx = SINE_TABLE_SIZE - 1;
    return fast_sine_table[idx];
}

// Generate one sample for a note with continuous phase tracking
static int16_t synthesize_one_sample(mus_channel_t *ch, int note_idx) {
    if (!ch->notes[note_idx].active || ch->volume == 0) return 0;
    
    int note = ch->notes[note_idx].note;
    int velocity = ch->notes[note_idx].velocity;
    double phase = ch->notes[note_idx].phase;
    int64_t note_age = ch->notes[note_idx].samples_generated;
    
    // ADSR envelope
    double envelope = 1.0;
    int64_t attack_samples = OUTPUT_SAMPLERATE / 20;   // 50ms attack
    int64_t decay_samples = OUTPUT_SAMPLERATE / 5;     // 200ms decay
    double sustain_level = 0.7;
    double release_time = OUTPUT_SAMPLERATE / 4;       // 250ms release
    
    // Envelope: in_release is set immediately by the event parser (MUS_EV_RELEASE_NOTE),
    // so we only need two branches here.
    if (ch->notes[note_idx].in_release) {
        // Fade out over release_time samples
        ch->notes[note_idx].release_envelope -= 1.0 / release_time;
        if (ch->notes[note_idx].release_envelope <= 0.0) {
            ch->notes[note_idx].active = 0;
            ch->notes[note_idx].in_release = 0;
            return 0;
        }
        envelope = ch->notes[note_idx].release_envelope;
    } else {
        // Normal ADSR sustain phase
        if (note_age < attack_samples) {
            envelope = (double)note_age / attack_samples;
        } else if (note_age < attack_samples + decay_samples) {
            envelope = 1.0 - (1.0 - sustain_level) * (note_age - attack_samples) / decay_samples;
        } else {
            envelope = sustain_level;
        }
    }
    
    // Generate waveform based on instrument type
    double sample = 0.0;
    int inst_cat = ch->instrument / 8;
    
    switch (inst_cat) {
        case 0:  // Piano - triangle wave
            sample = (phase < 0.5) ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase);
            break;
        case 1:  // Chromatic percussion - sine + harmonics
            sample = fast_sin(phase) + 0.3 * fast_sin(phase * 2.0);
            break;
        case 2:  // Organ - sine with harmonics
            sample = fast_sin(phase) + 0.5 * fast_sin(phase * 2.0) 
                   + 0.25 * fast_sin(phase * 3.0) + 0.125 * fast_sin(phase * 4.0);
            break;
        case 3:  // Guitar - sawtooth-ish
            sample = 2.0 * phase - 1.0;
            break;
        case 4:  // Bass - sine with slight distortion
            sample = fast_sin(phase);
            if (sample > 0.5) sample = 0.5;
            if (sample < -0.5) sample = -0.5;
            break;
        case 5:  // Strings - soft sawtooth
            sample = (2.0 * phase - 1.0) * 0.7;
            break;
        case 6:  // Ensemble - multiple sines
            sample = fast_sin(phase) * 0.6 + fast_sin(phase * 1.01) * 0.4;
            break;
        case 7:  // Brass - square-ish
            sample = (phase < 0.5) ? 0.7 : -0.7;
            break;
        case 8:  // Reed - soft square
            sample = (phase < 0.5) ? 0.5 : -0.5;
            break;
        case 9:  // Pipe - pure sine
            sample = fast_sin(phase);
            break;
        case 10: // Synth lead - varied
            sample = fast_sin(phase) + 0.3 * (2.0 * phase - 1.0);
            break;
        case 11: // Synth pad - soft harmonics
            sample = fast_sin(phase) * 0.5 + fast_sin(phase * 2.0) * 0.3 
                   + fast_sin(phase * 3.0) * 0.2;
            break;
        case 12: // Synth effects - noise-ish
            sample = fast_sin(phase) + 0.2 * ((double)fast_rand() / 32768.0 - 0.5);
            break;
        case 13: // Ethnic - triangle with vibrato
            sample = (phase < 0.5) ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase);
            break;
        case 14: // Percussive - noise burst
            if (note_age < OUTPUT_SAMPLERATE / 100) {
                sample = ((double)fast_rand() / 32768.0 - 0.5) * 2.0;
            } else {
                sample = 0;
            }
            break;
        case 15: // Sound effects / drums
            if (ch->instrument == 128) {  // drum channel
                if (note < 40) {  // bass drum
                    sample = fast_sin(phase * 2.0) * (1.0 - (note_age % 1000) / 1000.0);
                } else if (note < 50) {  // snare
                    sample = ((double)fast_rand() / 32768.0 - 0.5) * (1.0 - (note_age % 500) / 500.0);
                } else {  // hi-hat
                    sample = ((double)fast_rand() / 32768.0 - 0.5) * 0.5;
                }
            } else {
                sample = fast_sin(phase);
            }
            break;
        default:
            sample = fast_sin(phase);
    }
    
    // Advance phase for next sample
    ch->notes[note_idx].phase += ch->notes[note_idx].phase_inc;
    if (ch->notes[note_idx].phase >= 1.0) {
        ch->notes[note_idx].phase -= 1.0;
    }
    ch->notes[note_idx].samples_generated++;
    
    // Apply velocity, channel volume, and envelope
    double amp = (velocity / 127.0) * (ch->volume / 127.0) * envelope;
    
    return (int16_t)(sample * amp * 32767.0 * 0.2);
}

// Synthesize samples for all channels (stereo interleaved output)
static void synthesize_samples(int16_t *out_samples, int num_samples) {
    // Output is stereo interleaved, num_samples is total samples (L+R pairs)
    int num_stereo_frames = num_samples / 2;
    
    for (int frame = 0; frame < num_stereo_frames; frame++) {
        int32_t left = 0, right = 0;
        
        for (int c = 0; c < NUM_MUS_CHANNELS; c++) {
            mus_channel_t *ch = &music.channels[c];
            if (!ch->active) continue;
            
            double left_scale = (ch->pan < 64) ? 1.0 : (127.0 - ch->pan) / 64.0;
            double right_scale = (ch->pan > 64) ? 1.0 : (double)ch->pan / 64.0;
            
            for (int n = 0; n < 8; n++) {
                if (!ch->notes[n].active) continue;
                
                int16_t sample = synthesize_one_sample(ch, n);
                
                left += (int16_t)(sample * left_scale);
                right += (int16_t)(sample * right_scale);
            }
        }
        
        // Clamp to prevent overflow
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        
        out_samples[frame * 2] = (int16_t)left;
        out_samples[frame * 2 + 1] = (int16_t)right;
    }
    
    // Apply master volume
    double vol_scale = music.volume / 127.0;
    for (int i = 0; i < num_samples; i++) {
        out_samples[i] = (int16_t)(out_samples[i] * vol_scale);
    }
}

// Parse next MUS event
static int parse_mus_event(void) {
    if (music.score_ptr >= music.score_end) {
        if (music.looping) {
            // Restart from beginning
            mus_header_t *hdr = (mus_header_t *)music.mus_data;
            music.score_ptr = music.mus_data + hdr->score_start;
            music.ticks = 0;
            music.next_event_tick = 0;
            return 1;
        }
        music.playing = 0;
        return 0;
    }
    
    uint8_t event = *music.score_ptr++;
    int channel = event & 0x0F;
    int type = event & 0x70;
    
    // Channel 15 is percussion (channel 9 in MIDI terms)
    int midi_channel = (channel == 15) ? 9 : channel;
    if (channel == 15) {
        music.channels[midi_channel].instrument = 128;  // drum kit
    }
    music.channels[midi_channel].active = 1;
    
    switch (type) {
        case MUS_EV_RELEASE_NOTE: {
            uint8_t note_byte = *music.score_ptr++;
            int note = note_byte & 0x7F;

            // Immediately enter release phase rather than deferring via end_tick.
            // The old approach compared (end_tick > 0) which silently broke any
            // note that started at tick 0, leaving it sustaining forever.
            for (int n = 0; n < 8; n++) {
                mus_channel_t *mc = &music.channels[midi_channel];
                if (mc->notes[n].active && mc->notes[n].note == note
                        && !mc->notes[n].in_release) {
                    mc->notes[n].end_tick = music.ticks;
                    mc->notes[n].in_release = 1;
                    // Seed release level from current ADSR position
                    int64_t age = mc->notes[n].samples_generated;
                    int64_t atk = OUTPUT_SAMPLERATE / 20;
                    int64_t dec = OUTPUT_SAMPLERATE / 5;
                    double  sus = 0.7;
                    if (age < atk)
                        mc->notes[n].release_envelope = (double)age / atk;
                    else if (age < atk + dec)
                        mc->notes[n].release_envelope = 1.0 - (1.0 - sus) * (age - atk) / dec;
                    else
                        mc->notes[n].release_envelope = sus;
                    break;
                }
            }
            break;
        }
        
        case MUS_EV_PLAY_NOTE: {
            uint8_t note_byte = *music.score_ptr++;
            int note = note_byte & 0x7F;
            int velocity = (note_byte & 0x80) ? (*music.score_ptr++ & 0x7F) : 100;
            
            // Find note slot: prefer same note (retrigger), then free, then steal oldest
            int slot = -1;
            int free_slot = -1;
            int oldest_slot = -1;
            int64_t oldest_tick = INT64_MAX;

            for (int n = 0; n < 8; n++) {
                if (music.channels[midi_channel].notes[n].active &&
                    music.channels[midi_channel].notes[n].note == note &&
                    !music.channels[midi_channel].notes[n].in_release) {
                    slot = n;
                    break; // retrigger same note immediately
                }
                if (!music.channels[midi_channel].notes[n].active && free_slot < 0) {
                    free_slot = n;
                }
                if (music.channels[midi_channel].notes[n].active &&
                    music.channels[midi_channel].notes[n].start_tick < oldest_tick) {
                    oldest_tick = music.channels[midi_channel].notes[n].start_tick;
                    oldest_slot = n;
                }
            }
            if (slot < 0) slot = (free_slot >= 0) ? free_slot : oldest_slot;
            
            if (slot >= 0) {
                music.channels[midi_channel].notes[slot].active = 1;
                music.channels[midi_channel].notes[slot].note = note;
                music.channels[midi_channel].notes[slot].velocity = velocity;
                music.channels[midi_channel].notes[slot].start_tick = music.ticks;
                music.channels[midi_channel].notes[slot].end_tick = -1; // -1 = not released
                music.channels[midi_channel].notes[slot].phase = 0.0;
                music.channels[midi_channel].notes[slot].samples_generated = 0;
                music.channels[midi_channel].notes[slot].in_release = 0;
                music.channels[midi_channel].notes[slot].release_envelope = 0.0;
                // Calculate phase increment: freq / sample_rate
                double freq = 440.0;
                if (note >= 0 && note < 128) {
                    freq = fast_freq_table[note];
                }
                music.channels[midi_channel].notes[slot].phase_inc = freq / OUTPUT_SAMPLERATE;
            }
            break;
        }
        
        case MUS_EV_PITCH_BEND: {
            uint8_t bend = *music.score_ptr++;
            // MUS pitch bend: 0-255 maps to -8192 to +8191
            music.channels[midi_channel].pitch_bend = (bend - 128) * 64;
            break;
        }
        
        case MUS_EV_SYS_EVENT: {
            uint8_t sys = *music.score_ptr++;
            // System events: 10=allsounds off, 11=reset all, 12-14=undefined
            (void)sys;
            break;
        }
        
        case MUS_EV_CONTROLLER: {
            uint8_t ctrl = *music.score_ptr++;
            uint8_t value = *music.score_ptr++;
            
            switch (ctrl) {
                case MUS_CTRL_INSTRUMENT:
                    music.channels[midi_channel].instrument = value;
                    break;
                case MUS_CTRL_VOLUME:
                    music.channels[midi_channel].volume = value;
                    break;
                case MUS_CTRL_PAN:
                    music.channels[midi_channel].pan = value;
                    break;
                case MUS_CTRL_SUSTAIN:
                    // TODO: implement sustain pedal
                    break;
            }
            break;
        }
        
        case MUS_EV_END_OF_MEASURE:
            // Just a timing marker
            break;
            
        case MUS_EV_FINISH_PLAY:
            if (music.looping) {
                mus_header_t *hdr = (mus_header_t *)music.mus_data;
                music.score_ptr = music.mus_data + hdr->score_start;
                music.ticks = 0;
                music.next_event_tick = 0;
                return 1;
            }
            music.playing = 0;
            return 0;
            
        default:
            // Unknown event, skip
            break;
    }
    
    // Check for delay byte (last bit of event byte)
    int has_delay = event & 0x80;
    if (has_delay) {
        uint32_t delay = 0;
        uint8_t delay_byte;
        do {
            delay_byte = *music.score_ptr++;
            delay = (delay << 7) | (delay_byte & 0x7F);
        } while (delay_byte & 0x80);
        
        music.next_event_tick = music.ticks + delay;
    } else {
        music.next_event_tick = music.ticks;
    }
    
    return 1;
}

// Generate music samples and mix into the output
static void update_music(void) {
    if (!music.playing || !music.sample_buffer) return;
    
    // How many samples do we need? (140 Hz tick rate → samples per tick)
    double samples_per_tick = (double)OUTPUT_SAMPLERATE / 140.0;
    
    // Process MUS events until we have enough samples
    while (music.playing && music.sample_count < SAMPLE_BUFFER_SIZE) {
        // Process events at current tick
        while (music.ticks >= music.next_event_tick && music.playing) {
            if (!parse_mus_event()) break;
        }
        
        // Synthesize samples for this tick
        int samples_to_gen = (int)samples_per_tick;
        if (music.sample_count + samples_to_gen > SAMPLE_BUFFER_SIZE) {
            samples_to_gen = SAMPLE_BUFFER_SIZE - music.sample_count;
        }
        
        synthesize_samples(&music.sample_buffer[music.sample_count * OUTPUT_CHANNELS], 
                          samples_to_gen * OUTPUT_CHANNELS);
        music.sample_count += samples_to_gen;
        music.ticks++;
    }
}

// ── Music API implementation ────────────────────────────────────────────────

boolean I_AscentOS_InitMusic(void) {
    // Ensure tables are initialized
    fast_sin(0.0);

    memset(&music, 0, sizeof(music));
    music.volume = 127;  // full volume; callers use SetMusicVolume to adjust
    music.sample_buffer = (int16_t *)malloc(SAMPLE_BUFFER_SIZE * OUTPUT_CHANNELS * sizeof(int16_t));
    
    // Initialize channels with defaults
    for (int i = 0; i < NUM_MUS_CHANNELS; i++) {
        music.channels[i].volume = 127;  // MUS default: full volume (0-127)
        music.channels[i].pan = 64;
        music.channels[i].instrument = 0;
    }
    
    return music.sample_buffer != NULL;
}

void I_AscentOS_ShutdownMusic(void) {
    if (music.sample_buffer) {
        free(music.sample_buffer);
        music.sample_buffer = NULL;
    }
    if (music.mus_data) {
        free(music.mus_data);
        music.mus_data = NULL;
    }
    memset(&music, 0, sizeof(music));
}

void I_AscentOS_SetMusicVolume(int volume) {
    music.volume = volume;
}

void I_AscentOS_PauseMusic(void) {
    music.playing = 0;
}

void I_AscentOS_ResumeMusic(void) {
    if (music.mus_data) {
        music.playing = 1;
    }
}

void *I_AscentOS_RegisterSong(void *data, int len) {
    if (!data || len < (int)sizeof(mus_header_t)) return NULL;
    
    // Free previous song
    if (music.mus_data) {
        free(music.mus_data);
    }
    
    // Copy MUS data (Doom may free the original)
    music.mus_data = (uint8_t *)malloc(len);
    if (!music.mus_data) return NULL;
    memcpy(music.mus_data, data, len);
    music.mus_len = len;
    
    // Validate MUS header
    mus_header_t *hdr = (mus_header_t *)music.mus_data;
    if (memcmp(hdr->id, "MUS\x1a", 4) != 0) {
        // Try GENMIDI header (some WADs have this)
        free(music.mus_data);
        music.mus_data = NULL;
        return NULL;
    }
    
    music.score_ptr = music.mus_data + hdr->score_start;
    music.score_end = music.mus_data + len;
    
    return music.mus_data;
}

void I_AscentOS_UnRegisterSong(void *handle) {
    (void)handle;
    if (music.mus_data) {
        free(music.mus_data);
        music.mus_data = NULL;
    }
    music.playing = 0;
}

void I_AscentOS_PlaySong(void *handle, boolean looping) {
    (void)handle;
    if (!music.mus_data) return;
    
    music.looping = looping;
    music.playing = 1;
    music.ticks = 0;
    music.next_event_tick = 0;
    music.sample_pos = 0;
    music.sample_count = 0;
    
    // Reset channels
    for (int i = 0; i < NUM_MUS_CHANNELS; i++) {
        memset(music.channels[i].notes, 0, sizeof(music.channels[i].notes));
    }
}

void I_AscentOS_StopSong(void) {
    music.playing = 0;
    music.sample_pos = 0;
    music.sample_count = 0;
    
    // Silence all notes
    for (int i = 0; i < NUM_MUS_CHANNELS; i++) {
        for (int n = 0; n < 8; n++) {
            music.channels[i].notes[n].active = 0;
        }
    }
}

boolean I_AscentOS_MusicIsPlaying(void) {
    return music.playing;
}

void I_AscentOS_PollMusic(void) {
    // Generate more samples if needed
    update_music();
}

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