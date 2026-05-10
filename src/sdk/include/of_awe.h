/*
 * of_awe.h -- AWE audio coprocessor API for openfpgaOS (retired).
 *
 * The AWE fabric coprocessor was retired; the CPU-side software voice
 * engine (of_smp_voice) drives the software mixer directly.  This header
 * is preserved for source compatibility with SDK apps that still reference
 * the AWE API — every entry point is a no-op inline stub.
 *
 * Apps that want polyphonic SF2/MIDI should use of_midi.h + of_smp_voice,
 * which now run entirely on the CPU.
 */

#ifndef OF_AWE_H
#define OF_AWE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_AWE_MAX_VOICES    32
#define OF_AWE_NUM_CHANNELS  16
#define OF_AWE_NUM_SEGMENTS  4

#define AWE_SEG_NEXT_STAY    0xFE
#define AWE_SEG_NEXT_DONE    0xFF

#define AWE_LOOP_NONE        0
#define AWE_LOOP_FORWARD     1
#define AWE_LOOP_BIDI        3

#define AWE_INTERP_NONE      0
#define AWE_INTERP_LINEAR    1
#define AWE_INTERP_CUBIC     2

#define AWE_WAVE_TRIANGLE    0
#define AWE_WAVE_SINE        1
#define AWE_WAVE_SQUARE      2
#define AWE_WAVE_SAWTOOTH    3
#define AWE_WAVE_RANDOM      4

#define AWE_FILTER_BYPASS    13500

#define AWE_ENV_ATTACK       2
#define AWE_ENV_RELEASE      6
#define AWE_ENV_DONE         7

typedef struct {
    uint32_t target;
    uint32_t rate;
    uint32_t timer_ticks;
    uint8_t  next;
    uint8_t  _pad[3];
} awe_segment_t;

typedef struct {
    uint32_t rate;
    uint32_t delay_ticks;
    uint8_t  waveform;
    uint8_t  _pad[3];
} awe_lfo_t;

typedef struct {
    int16_t lfo0_pitch;
    int16_t lfo0_filter;
    int16_t lfo1_pitch;
    int16_t ramp1_pitch;
    int16_t ramp1_filter;
    int16_t _pad;
} awe_mm_t;

typedef struct awe_voice_t {
    const void *base;
    uint32_t length;
    uint32_t loop_start;
    uint32_t loop_end;
    uint8_t  loop_mode;
    uint8_t  interp_mode;
    uint8_t  fmt16;
    uint8_t  _pad_play;
    uint8_t  midi_channel;
    uint8_t  voice_base_vol;
    int16_t  pan_base;
    uint32_t base_rate;
    int16_t  initial_fc;
    int16_t  initial_q;
    uint32_t vol_delay_ticks;
    uint32_t vol_attack_rate;
    uint32_t vol_hold_ticks;
    uint32_t vol_decay_rate;
    uint32_t vol_sustain_level;
    uint32_t vol_release_ticks;
    awe_segment_t ramp0_segs[OF_AWE_NUM_SEGMENTS];
    awe_segment_t ramp1_segs[OF_AWE_NUM_SEGMENTS];
    awe_lfo_t lfo0;
    awe_lfo_t lfo1;
    awe_mm_t mm;
    uint8_t  reverb_send;
    uint8_t  chorus_send;
    uint8_t  _pad_sends[2];
} awe_voice_t;

/* All entry points below are inline no-ops (pure source compatibility). */

static inline void     of_awe_voice_load(int v, const awe_voice_t *p)    { (void)v; (void)p; }
static inline void     of_awe_voice_trigger(int v)                       { (void)v; }
static inline void     of_awe_voice_release(int v)                       { (void)v; }
static inline void     of_awe_voice_stop(int v)                          { (void)v; }
static inline void     of_awe_channel_set_volume(int c, int x)           { (void)c; (void)x; }
static inline void     of_awe_channel_set_expression(int c, int x)       { (void)c; (void)x; }
static inline void     of_awe_channel_set_pan(int c, int x)              { (void)c; (void)x; }
static inline void     of_awe_channel_set_bend(int c, int x)             { (void)c; (void)x; }
static inline void     of_awe_channel_set_mod(int c, int x)              { (void)c; (void)x; }
static inline void     of_awe_channel_set_sustain(int c, int x)          { (void)c; (void)x; }
static inline void     of_awe_channel_set_brightness(int c, int x)       { (void)c; (void)x; }
static inline void     of_awe_channel_set_resonance(int c, int x)        { (void)c; (void)x; }
static inline void     of_awe_channel_set_reverb_send(int c, int x)      { (void)c; (void)x; }
static inline void     of_awe_channel_set_chorus_send(int c, int x)      { (void)c; (void)x; }
static inline void     of_awe_set_master_volume(int v)                   { (void)v; }
static inline void     of_awe_set_bend_range(int c)                      { (void)c; }
static inline uint64_t of_awe_active_mask(void)                          { return 0; }
static inline uint32_t of_awe_tick_count(void)                           { return 0; }
static inline void     of_awe_set_hw_envelope(int e)                     { (void)e; }
static inline void     of_awe_set_reverb_level(int l)                    { (void)l; }
static inline void     of_awe_set_reverb_feedback(int f)                 { (void)f; }
static inline void     of_awe_set_chorus_level(int l)                    { (void)l; }
static inline void     of_awe_set_chorus_rate(int r)                     { (void)r; }
static inline void     of_awe_set_chorus_depth(int d)                    { (void)d; }
static inline void     of_awe_ramp1_trigger(int v, int s, uint32_t r)    { (void)v; (void)s; (void)r; }

#ifdef __cplusplus
}
#endif

#endif /* OF_AWE_H */
