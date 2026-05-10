/*
 * of_smp_voice.h -- Software voice engine for sample-based MIDI synthesis.
 *
 * Manages simultaneous sample voices with DAHDSR envelopes and dual
 * LFOs, driving the hardware mixer.  All math is fixed-point Q16.16;
 * runs in the 1 kHz timer ISR on RV32IMFC.
 */

#ifndef OF_SMP_VOICE_H
#define OF_SMP_VOICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "of_smp_bank.h"

/* Cut from 28→12 so smp_voice_tick's ISR loop finishes fast enough
 * for Doom's renderer to get the CPU back.  Dense MIDI passages lose
 * the quietest couple of voices to the stealer; audible but acceptable
 * tradeoff.  Sits well under OF_MIXER_MAX_VOICES (32) so the allocator
 * never has to re-steal. */
#define SMP_MAX_VOICES 12

typedef enum {
    ENV_OFF = 0, ENV_DELAY, ENV_ATTACK, ENV_HOLD,
    ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_DONE
} env_stage_t;

typedef struct {
    env_stage_t stage;
    int32_t level;      /* Q16.16 (0=silence, 0x10000=full) */
    int32_t rate;       /* level delta per tick (sign depends on stage) */
    int32_t target;     /* level at end of current stage */
    int32_t timer;      /* ticks remaining in delay/hold (counts down) */
} env_state_t;

typedef struct {
    int32_t phase;      /* Q16.16, wraps at 0x10000 (one full cycle) */
    int32_t rate;       /* phase increment per tick */
    int32_t delay_ticks;/* delay countdown (>0 = LFO silent) */
} lfo_state_t;

typedef struct {
    int active;
    const ofsf_zone_t *zone;
    uint8_t midi_ch;
    uint8_t note;
    uint8_t velocity;
    uint8_t voice_base_vol;  /* Pre-baked at note-on: (vel_scale × initial_attn_scale) >> 8.
                                Collapses two multiplies into one slot, drops one mul/tick. */
    uint8_t sustain_held; /* CC64 holding this note in sustain */
    int mixer_voice;      /* hardware mixer voice index */
    env_state_t vol_env;
    env_state_t mod_env;
    lfo_state_t mod_lfo;
    lfo_state_t vib_lfo;
    uint32_t base_rate_fp16; /* base 16.16 playback rate (no bend/LFO) */
    int16_t cur_filter_fc;
    int16_t cur_filter_q;
    uint16_t cur_cutoff_hw;   /* last HW FC value written (skip redundant writes
                                 from fine-grained cents changes that round to
                                 the same Q0.16 HW cutoff) */
    uint32_t age;
    /* Countdown of smp_voice_tick calls until the underlying non-looping
     * sample has played to its natural end.  0 = not tracked (looping
     * sample, or untracked).  When this reaches 0 from a positive value,
     * we force ENV_DONE so the voice slot is reclaimed promptly.
     *
     * Why:  of_mixer_play on a one-shot sample eventually walks off the
     * end of LEN and stops emitting audio — but smp_voice has no way to
     * know that happened, so the slot remains allocated for the full
     * length of the SF2 volume envelope (which for drums can be
     * multi-second SUSTAIN, parking the voice forever).  During dense
     * drum tracks this fills all 28 software voices and every new note
     * must steal, producing "old data" artifacts.  Tracking the
     * expected end lets us release slots as soon as the audio has
     * actually finished. */
    int32_t sample_ticks_remaining;
} smp_voice_t;

void smp_voice_init(void);
int  smp_voice_note_on(const ofsf_zone_t *zone, int midi_ch, int note,
                       int velocity, const void *sample_base);
void smp_voice_note_off(int midi_ch, int note);
void smp_voice_tick(void);  /* 1 kHz ISR */

/* Diagnostic stats for smp_voice_tick cost.  Task #10 probe: detect
 * whether the tick exceeds its 2 ms budget (500 Hz tick rate).
 * Fields are named cycles_* but actually hold microseconds — the
 * VexRiscv here does not expose rdcycle to user mode, so of_time_us()
 * (kernel ecall) is used instead. */
typedef struct {
    uint32_t cycles_max;     /* worst-case microseconds for a single tick */
    uint32_t cycles_last;    /* microseconds of most recent tick */
    uint32_t spike_count;    /* ticks where microseconds > 2000 */
    uint32_t tick_count;     /* total ticks since reset */
    uint8_t  active_peak;    /* max active voices seen since reset */
    /* Stage histogram — snapshot at the most recent tick. */
    uint8_t  stage_sustain;  /* voices in ENV_SUSTAIN (waiting for note-off) */
    uint8_t  stage_release;  /* voices in ENV_RELEASE (fading out) */
    uint8_t  stage_decay;    /* voices in ENV_DECAY (between attack and sustain) */
    uint8_t  sustain_held;   /* voices with CC64 sustain pedal still holding them */
    /* Per-MIDI-channel active voice count — snapshot at most recent tick.
     * Lets you see which channels are actually producing sound (and
     * release tails), vs. channels that should be silent.  Index 9 is
     * the drum channel. */
    uint8_t  ch_active[16];

    /* MMIO-write counters — incremented on every actual HW write since
     * last reset (after the cache-skip guards, so these are the writes
     * that genuinely hit the mixer bus). */
    uint32_t filter_writes;  /* of_mixer_set_filter calls */
    uint32_t rate_writes;    /* pitch / voice rate register writes */
    uint32_t vol_writes;     /* volume register writes */

    /* Pump() interval stats — populated by smp_voice_tick_record_pump(),
     * lets us see whether of_midi_pump is called smoothly or in bursts. */
    uint32_t pump_count;             /* total of_midi_pump calls since reset */
    uint32_t pump_interval_max_us;   /* worst-case gap between pumps */
    uint32_t pump_interval_min_us;   /* best-case gap (UINT32_MAX if no data) */
    uint32_t pump_burst_count;       /* pumps where >1 ticks fired */
    uint32_t pump_budget_exceeded;   /* pumps where tick_budget==0 at end */

    /* Biggest single-tick jump in HW cutoff (Q0.16 units, 0..65535) across
     * all voices since reset.  Large jumps → larger SVF state-variable
     * transients.  Useful for judging whether filter LFOs / env sweeps
     * are producing audibly steppy cutoff trajectories. */
    uint16_t cutoff_delta_max;
} smp_tick_stats_t;

void smp_voice_tick_get_stats(smp_tick_stats_t *out);
void smp_voice_tick_reset_stats(void);

/* Feed pump() timing into the probe.  `elapsed_us` is the wallclock gap
 * since the previous pump; `ticks_fired` is how many smp_voice_tick calls
 * the pump dispatched; `budget_exceeded` is 1 iff the pump hit its
 * tick_budget cap (i.e. had to drop accumulated ticks). */
void smp_voice_tick_record_pump(uint32_t elapsed_us, int ticks_fired,
                                int budget_exceeded);
void smp_voice_update_volume(int midi_ch, int volume, int expression);
void smp_voice_update_pan(int midi_ch, int pan);
void smp_voice_update_bend(int midi_ch, int bend);
void smp_voice_update_mod(int midi_ch, int mod_depth);
void smp_voice_update_sustain(int midi_ch, int sustain_on);
void smp_voice_update_filter(int midi_ch, int brightness, int resonance);
/* CC91 (reverb send) and CC93 (chorus send), 0..127 — stored per channel
 * but not yet acted on (the CPU mixer has no per-voice send paths). */
void smp_voice_update_reverb_send(int midi_ch, int send_0_127);
void smp_voice_update_chorus_send(int midi_ch, int send_0_127);
void smp_voice_all_off(int midi_ch);
void smp_voice_all_off_global(void);
void smp_voice_set_master_volume(int vol);

/* AWE-backend redirect (retired).  Preserved as ABI no-ops so existing
 * SDK apps that enable/query the AWE path still link; the CPU-side SW
 * voice engine is the only backend now. */
void smp_voice_enable_awe_backend(int on);
int  smp_voice_awe_backend_enabled(void);

/* ------------------------------------------------------------------ */
/* Mixer-write trace (OF_TRACE_MIXER_WRITES)                          */
/* ------------------------------------------------------------------ */
/* Compile with -DOF_TRACE_MIXER_WRITES to log every rate / vol / filter
 * mixer write the voice engine performs into an in-memory ring buffer.
 * Used for bit-identical pre/post-refactor verification by replaying a
 * deterministic MIDI clip before and after a change and diffing the
 * dumped traces.
 *
 * Zero overhead when the flag is not defined — the API symbols exist
 * but are no-ops. */

#define SMP_TRACE_OP_RATE        1u   /* arg0=rate_fp16 */
#define SMP_TRACE_OP_VOL_LR      2u   /* arg0=vol_l, arg1=vol_r */
#define SMP_TRACE_OP_VOICE_RAW   3u   /* arg0=rate_fp16, arg1=vol_l, arg2=vol_r */
#define SMP_TRACE_OP_FILTER      4u   /* arg0=cutoff_q016, arg1=q, arg2=enable */

typedef struct {
    uint32_t seq;       /* monotonic sequence number since last reset */
    uint8_t  op;        /* SMP_TRACE_OP_* */
    uint8_t  voice;     /* hardware mixer voice index */
    uint16_t _pad;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
} smp_mixer_trace_entry_t;

/* Zero the ring and the sequence counter. */
void smp_mixer_trace_reset(void);

/* Copy up to `max` oldest-first entries into `out`.  Returns the number
 * copied (0 if out==NULL or tracing is disabled).  Entries older than
 * the ring capacity are dropped; the returned count never exceeds the
 * smaller of `max` and the ring capacity. */
uint32_t smp_mixer_trace_dump(smp_mixer_trace_entry_t *out, uint32_t max);

/* Total entries recorded since reset (including ones overwritten by
 * ring wrap) — lets the caller detect wrap so it can warn / run a
 * shorter clip. */
uint32_t smp_mixer_trace_total(void);

#ifdef __cplusplus
}
#endif

#endif /* OF_SMP_VOICE_H */
