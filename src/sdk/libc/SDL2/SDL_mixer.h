/*
 * SDL_mixer shim for openfpgaOS
 *
 * Minimal SDL_mixer implementation wrapping of_mixer for SFX.
 * Music (OGG/MOD) is stubbed — not supported on FPGA.
 * On PC builds, this header is never used — the real SDL_mixer is linked.
 */

#ifndef _OF_SDL_MIXER_SHIM_H
#define _OF_SDL_MIXER_SHIM_H

#ifdef OF_PC
#include_next <SDL2/SDL_mixer.h>
#else

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ======================================================================
 * Constants
 * ====================================================================== */

#define MIX_INIT_OGG     0x00000002
#define MIX_INIT_MP3     0x00000008
#define MIX_MAX_VOLUME   128

/* ======================================================================
 * Types
 * ====================================================================== */

typedef struct {
    uint8_t  *pcm_u8;
    uint32_t  sample_count;
    uint32_t  sample_rate;
    int       volume;
} Mix_Chunk;

typedef struct { int unused; } Mix_Music;

/* ======================================================================
 * Internal state
 * ====================================================================== */

static int __mix_initialized;
static int __mix_max_channels = 8;
static int __mix_voice_ids[32];

/* ======================================================================
 * Init / Open / Close
 * ====================================================================== */

static inline int Mix_Init(int flags) { (void)flags; return flags; }
static inline void Mix_Quit(void) { __mix_initialized = 0; }

static inline int Mix_OpenAudio(int freq, uint16_t fmt, int ch, int sz) {
    (void)freq; (void)fmt; (void)ch; (void)sz;
    return 0; /* defer init until first play */
}

static inline void Mix_CloseAudio(void) {
    if (__mix_initialized) of_mixer_stop_all();
    __mix_initialized = 0;
}

static inline const char *Mix_GetError(void) { return ""; }

/* ======================================================================
 * WAV loading
 * ====================================================================== */

static inline Mix_Chunk *Mix_LoadWAV(const char *file) {
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(f); return NULL; }
    fread(data, 1, (size_t)size, f);
    fclose(f);

    of_codec_result_t result;
    if (of_codec_parse_wav(data, (uint32_t)size, &result) < 0) {
        free(data);
        return NULL;
    }

    uint32_t num_samples = result.pcm_len;
    if (result.bits_per_sample == 16) num_samples /= 2;
    if (result.channels == 2) num_samples /= 2;

    uint8_t *pcm_u8 = (uint8_t *)malloc(num_samples);
    if (!pcm_u8) { free(data); return NULL; }

    if (result.bits_per_sample == 16) {
        const int16_t *s = (const int16_t *)result.pcm;
        int step = result.channels;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_u8[i] = (uint8_t)((s[i * step] >> 8) + 128);
    } else {
        int step = result.channels;
        for (uint32_t i = 0; i < num_samples; i++)
            pcm_u8[i] = result.pcm[i * step];
    }
    free(data);

    Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    if (!chunk) { free(pcm_u8); return NULL; }
    chunk->pcm_u8 = pcm_u8;
    chunk->sample_count = num_samples;
    chunk->sample_rate = result.sample_rate;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

static inline void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    free(chunk->pcm_u8);
    free(chunk);
}

/* ======================================================================
 * Channel playback (SFX)
 * ====================================================================== */

static inline int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) {
    if (!chunk || !chunk->pcm_u8) return -1;
    (void)loops;

    if (!__mix_initialized) {
        of_audio_init();
        of_mixer_init(__mix_max_channels, OF_AUDIO_RATE);
        __mix_initialized = 1;
        memset(__mix_voice_ids, -1, sizeof(__mix_voice_ids));
    }

    int vol = (chunk->volume * 255) / 128;
    int voice = of_mixer_play(chunk->pcm_u8, chunk->sample_count,
                              chunk->sample_rate, 0, vol);
    if (voice < 0) return -1;

    if (channel < 0) {
        for (int i = 0; i < __mix_max_channels; i++) {
            if (__mix_voice_ids[i] < 0 || !of_mixer_voice_active(__mix_voice_ids[i])) {
                channel = i; break;
            }
        }
        if (channel < 0) channel = 0;
    }
    if (channel < 32) __mix_voice_ids[channel] = voice;
    return channel;
}

static inline void Mix_HaltChannel(int channel) {
    if (!__mix_initialized) return;
    if (channel < 0) { of_mixer_stop_all(); return; }
    if (channel < 32 && __mix_voice_ids[channel] >= 0)
        of_mixer_stop(__mix_voice_ids[channel]);
}

static inline void Mix_Pause(int ch)  { (void)ch; }
static inline void Mix_Resume(int ch) { (void)ch; }

/* ======================================================================
 * Music (stubs)
 * ====================================================================== */

static inline Mix_Music *Mix_LoadMUS(const char *f) { (void)f; return NULL; }
static inline void Mix_FreeMusic(Mix_Music *m)      { (void)m; }
static inline int Mix_PlayMusic(Mix_Music *m, int l) { (void)m; (void)l; return -1; }
static inline int Mix_FadeInMusic(Mix_Music *m, int l, int ms) { (void)m;(void)l;(void)ms; return -1; }
static inline int Mix_FadeOutMusic(int ms)           { (void)ms; return 0; }
static inline void Mix_HaltMusic(void)               {}
static inline void Mix_PauseMusic(void)               {}
static inline void Mix_ResumeMusic(void)              {}

#endif /* OF_PC */
#endif /* _OF_SDL_MIXER_SHIM_H */
