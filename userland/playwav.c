#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// OSS /dev/dsp ioctls
#define SNDCTL_DSP_SPEED    0xC0045002
#define SNDCTL_DSP_STEREO   0xC0045003
#define SNDCTL_DSP_SETFMT   0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006

#define AFMT_U8     0x00000008
#define AFMT_S16_LE 0x00000010

#pragma pack(push, 1)
typedef struct {
    char riff_id[4];
    uint32_t riff_size;
    char wave_id[4];
} WavHeader;

typedef struct {
    char chunk_id[4];
    uint32_t chunk_size;
} ChunkHeader;

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;
#pragma pack(pop)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <file.wav>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("playwav: could not open %s\n", argv[1]);
        return 1;
    }

    WavHeader header;
    if (read(fd, &header, sizeof(WavHeader)) != sizeof(WavHeader)) {
        printf("playwav: invalid file\n");
        close(fd); return 1;
    }

    if (strncmp(header.riff_id, "RIFF", 4) != 0 || strncmp(header.wave_id, "WAVE", 4) != 0) {
        printf("playwav: not a valid WAV file\n");
        close(fd); return 1;
    }

    ChunkHeader chunk_hdr;
    FmtChunk fmt;
    int fmt_found = 0;
    uint32_t data_size = 0;

    while (read(fd, &chunk_hdr, sizeof(ChunkHeader)) == sizeof(ChunkHeader)) {
        if (strncmp(chunk_hdr.chunk_id, "fmt ", 4) == 0) {
            read(fd, &fmt, sizeof(FmtChunk));
            if (chunk_hdr.chunk_size > sizeof(FmtChunk)) {
                lseek(fd, chunk_hdr.chunk_size - sizeof(FmtChunk), SEEK_CUR);
            }
            fmt_found = 1;
        } else if (strncmp(chunk_hdr.chunk_id, "data", 4) == 0) {
            data_size = chunk_hdr.chunk_size;
            break; // Data chunk found, stop parsing headers
        } else {
            lseek(fd, chunk_hdr.chunk_size, SEEK_CUR);
        }
    }

    if (!fmt_found || data_size == 0) {
        printf("playwav: invalid WAV format (missing fmt or data chunk)\n");
        close(fd); return 1;
    }

    if (fmt.audio_format != 1 || (fmt.num_channels != 1 && fmt.num_channels != 2) || (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16)) {
        printf("playwav: unsupported WAV format. Required: PCM, 1/2 ch, 8/16-bit.\n");
        printf("Got: fmt=%d, ch=%d, bits=%d\n", fmt.audio_format, fmt.num_channels, fmt.bits_per_sample);
        close(fd); return 1;
    }

    printf("Playing: %s (Sample Rate: %d Hz, %d channels, %d bits)\n", argv[1], fmt.sample_rate, fmt.num_channels, fmt.bits_per_sample);

    // Open the audio device
    int dsp_fd = open("/dev/dsp", O_WRONLY);
    if (dsp_fd < 0) {
        printf("playwav: could not open /dev/dsp\n");
        close(fd); return 1;
    }

    // Configure the audio device using standard OSS ioctls
    int sample_rate = (int)fmt.sample_rate;
    if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &sample_rate) < 0) {
        perror("ioctl SNDCTL_DSP_SPEED");
    }

    int channels = (int)fmt.num_channels;
    if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &channels) < 0) {
        perror("ioctl SNDCTL_DSP_CHANNELS");
    }

    int format = (fmt.bits_per_sample == 16) ? AFMT_S16_LE : AFMT_U8;
    if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &format) < 0) {
        perror("ioctl SNDCTL_DSP_SETFMT");
    }

    uint32_t chunk_size = 65536;
    uint8_t *buffer = malloc(chunk_size);
    if (!buffer) {
        printf("playwav: failed to allocate memory\n");
        close(fd); close(dsp_fd); return 1;
    }

    int bytes_read;
    while ((bytes_read = read(fd, buffer, chunk_size)) > 0) {
        if (write(dsp_fd, buffer, bytes_read) != bytes_read) {
            printf("playwav: write to /dev/dsp failed\n");
            break;
        }
    }

    free(buffer);
    close(dsp_fd);
    close(fd);
    printf("Playback finished.\n");
    return 0;
}

