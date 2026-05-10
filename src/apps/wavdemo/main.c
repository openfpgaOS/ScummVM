/*
 * openfpgaOS WAV Player Demo
 *
 * Loads a WAV file from data slot 3 into CRAM1, plays it through
 * the 32-voice hardware mixer. Progress bar updated via timer interrupt.
 *
 * Controls:
 *   START  = play/pause
 *   SELECT = restart
 */

#include "of.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define WAV_SLOT_ID     3
#define MAX_WAV_SIZE    (4 * 1024 * 1024)

/* WAV loaded into SDRAM first, then converted to CRAM1 */
static uint8_t wav_buf[MAX_WAV_SIZE] __attribute__((aligned(4)));

/* Sample buffer (allocated from kernel CRAM1 pool) */
static int16_t *sample_buf;

/* Playback state (accessed from ISR) */
static volatile int playing;
static volatile uint32_t play_start_ms;
static uint32_t total_samples;
static uint32_t sample_rate;
static volatile int progress_dirty;

/* Timer callback: mark progress bar for update */
void timer_tick(void) {
    if (playing)
        progress_dirty = 1;
}

/* WAV header */
typedef struct {
    uint16_t format, channels;
    uint32_t sample_rate, byte_rate;
    uint16_t block_align, bits_per_sample;
    const uint8_t *data;
    uint32_t data_size;
} wav_info_t;

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int wav_parse(const uint8_t *buf, uint32_t len, wav_info_t *info) {
    if (len < 44) return -1;
    if (memcmp(buf, "RIFF", 4) != 0) return -2;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return -3;
    uint32_t pos = 12;
    int have_fmt = 0;
    while (pos + 8 <= len) {
        uint32_t csz = read32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            if (csz < 16) return -4;
            info->format = read16(buf + pos + 8);
            info->channels = read16(buf + pos + 10);
            info->sample_rate = read32(buf + pos + 12);
            info->byte_rate = read32(buf + pos + 16);
            info->block_align = read16(buf + pos + 20);
            info->bits_per_sample = read16(buf + pos + 22);
            have_fmt = 1;
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            if (!have_fmt) return -5;
            info->data = buf + pos + 8;
            info->data_size = csz;
            if (pos + 8 + csz > len) info->data_size = len - pos - 8;
            return 0;
        }
        pos += 8 + csz;
        if (csz & 1) pos++;
    }
    return -6;
}

static void draw_progress(void) {
    uint32_t elapsed_ms = of_time_ms() - play_start_ms;
    uint32_t pos = (uint32_t)((uint64_t)elapsed_ms * sample_rate / 1000);
    if (pos > total_samples) pos = total_samples;
    int pct = (int)((uint64_t)pos * 100 / total_samples);
    int filled = pct * 28 / 100;
    printf("\033[11;2H[");
    for (int i = 0; i < 28; i++)
        putchar(i < filled ? '#' : '-');
    printf("] %d%%  ", pct);

    if (pos >= total_samples && playing) {
        playing = 0;
        printf("\033[9;3H[FINISHED]   \n");
    }
}

static int start_play(void) {
    int voice = of_mixer_play(
        (const uint8_t *)sample_buf,
        total_samples, sample_rate, 0, 255);
    play_start_ms = of_time_ms();
    playing = 1;
    printf("\033[9;3H[PLAYING]    \n");
    return voice;
}

int main(void) {
    printf("\033[2J\033[H");
    printf("  WAV Player Demo\n\n");
    printf("  Loading WAV file...\n");

    /* Loads from data slot 3; filename-based fopen requires SDK plumbing
     * of_file_get_name from the OS, which is not wired up yet. */
    FILE *f = fopen("slot:3", "rb");
    if (!f) {
        printf("  Error: cannot open WAV\n");
        while (1) usleep(100 * 1000);
    }

    uint32_t file_size = fread(wav_buf, 1, MAX_WAV_SIZE, f);
    fclose(f);
    printf("  Read %d bytes\n", (int)file_size);

    wav_info_t wav;
    int rc = wav_parse(wav_buf, file_size, &wav);
    if (rc < 0) {
        printf("  WAV parse error: %d\n", rc);
        while (1) usleep(100 * 1000);
    }
    if (wav.format != 1) {
        printf("  Not PCM (fmt=%d)\n", wav.format);
        while (1) usleep(100 * 1000);
    }

    total_samples = wav.data_size / wav.block_align;
    sample_rate = wav.sample_rate;
    float duration = (float)total_samples / sample_rate;

    printf("  %d-bit %s %dHz\n",
           wav.bits_per_sample,
           wav.channels == 1 ? "mono" : "stereo",
           (int)sample_rate);
    printf("  %d samples (%.1fs)\n\n",
           (int)total_samples, duration);

    /* Convert to 16-bit signed mono in sample buffer */
    printf("  Converting...\n");
    sample_buf = (int16_t *)of_mixer_alloc_samples(total_samples * sizeof(int16_t));
    if (!sample_buf) {
        printf("  Error: sample alloc failed\n");
        while (1) usleep(100 * 1000);
    }

    for (uint32_t i = 0; i < total_samples; i++) {
        int16_t s;
        if (wav.bits_per_sample == 16) {
            const int16_t *pcm = (const int16_t *)(wav.data + i * wav.block_align);
            s = (wav.channels >= 2) ? (int16_t)(((int32_t)pcm[0] + pcm[1]) >> 1) : pcm[0];
        } else {
            const uint8_t *pcm = wav.data + i * wav.block_align;
            s = (wav.channels >= 2)
                ? (int16_t)((((int)pcm[0] + pcm[1]) / 2 - 128) << 8)
                : (int16_t)(((int)pcm[0] - 128) << 8);
        }
        sample_buf[i] = s;
    }

    /* Verify CRAM1 data integrity */
    puts("  Verifying...");
    uint32_t errors = 0;
    uint32_t first_err = 0;
    for (uint32_t i = 0; i < total_samples; i++) {
        int16_t exp;
        if (wav.bits_per_sample == 16) {
            const int16_t *pcm = (const int16_t *)(wav.data + i * wav.block_align);
            exp = (wav.channels >= 2) ? (int16_t)(((int32_t)pcm[0] + pcm[1]) >> 1) : pcm[0];
        } else {
            const uint8_t *pcm = wav.data + i * wav.block_align;
            exp = (wav.channels >= 2)
                ? (int16_t)((((int)pcm[0] + pcm[1]) / 2 - 128) << 8)
                : (int16_t)(((int)pcm[0] - 128) << 8);
        }
        int16_t got = sample_buf[i];
        if (got != exp) {
            if (errors == 0) {
                first_err = i;
                printf("  ERR @%d: %d!=%d\n",
                       (int)i, (int)got, (int)exp);
            }
            errors++;
        }
    }
    if (errors == 0)
        printf("  VERIFY OK (%d)\n", (int)total_samples);
    else
        printf("  VERIFY FAIL %d @%d\n",
               (int)errors, (int)first_err);

    printf("  Press START to play\n");
    while (1) {
        of_input_poll();
        of_input_state_t st;
        of_input_state(0, &st);
        if (st.buttons_pressed & OF_BTN_START) break;
        usleep(16 * 1000);
    }

    /* Init mixer + start playback */
    of_mixer_init(32, 48000);
    int voice = start_play();

    /* Start 30 Hz timer for progress bar */
    of_timer_set_callback(timer_tick, 30);

    while (1) {
        of_input_poll();
        of_input_state_t st;
        of_input_state(0, &st);

        if (st.buttons_pressed & OF_BTN_START) {
            if (playing) {
                of_mixer_stop(voice);
                playing = 0;
                printf("\033[9;3H[PAUSED]     \n");
            } else {
                voice = start_play();
            }
        }
        if (st.buttons_pressed & OF_BTN_SELECT) {
            of_mixer_stop(voice);
            voice = start_play();
        }

        if (progress_dirty) {
            progress_dirty = 0;
            draw_progress();
        }

        usleep(16 * 1000);
    }

    return 0;
}
