/*
 * of_services.h -- openfpgaOS OS Services Table
 *
 * Direct function pointer table for OS services. Apps call through
 * this table instead of ecall, saving ~50 cycles per call.
 *
 * The table address is delivered to apps via the AT_OF_SVC auxv tag
 * set up by the kernel ELF loader (see of_app_abi.h). of_init.c's
 * constructor stashes the pointer in _of_svc_ptr before main() runs;
 * apps just use the OF_SVC macro:
 *
 *   uint8_t *fb = OF_SVC->video_get_surface();
 */

#ifndef OF_SERVICES_H
#define OF_SERVICES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_SVC_MAGIC    0x4F535643  /* 'OSVC' */
#define OF_SVC_VERSION  1

/* Forward declare input state struct */
struct of_input_state;

/* Forward declare AWE per-voice config -- full definition in of_awe.h.
 * Kept opaque here so this header doesn't pull the AWE-specific types
 * into every TU that just wants the services table. */
struct awe_voice_t;

struct of_services_table {
    uint32_t magic;
    uint32_t version;
    uint32_t count;         /* Number of function pointers */

    /* -- Video (12) -- */
    void      (*video_init)(void);
    uint8_t * (*video_get_surface)(void);
    uint8_t * (*video_flip)(void);
    void      (*video_wait_flip)(void);
    void      (*video_vsync)(void);
    void      (*video_set_palette)(uint8_t index, uint32_t rgb);
    void      (*video_set_palette_bulk)(const uint32_t *pal, int count);
    void      (*video_set_palette_vga4)(const uint8_t *vga_pal, int count);
    void      (*video_clear)(uint8_t color);
    void      (*video_flush_cache)(void);
    void      (*video_set_display_mode)(int mode);
    void      (*video_set_color_mode)(int mode);

    /* -- Input (4) -- */
    void      (*input_poll)(void);
    void      (*input_get_state)(int player, void *out);
    void      (*input_poll_p0)(void *out);
    void      (*input_set_deadzone)(int16_t deadzone);

    /* -- Mixer core (22) -- (extensions appended below) */
    void      (*mixer_init)(int max_voices, int output_rate);
    int       (*mixer_play)(const uint8_t *pcm_s16, uint32_t sample_count,
                            uint32_t sample_rate, int priority, int volume);
    void      (*mixer_stop)(int voice);
    void      (*mixer_stop_all)(void);
    void      (*mixer_set_volume)(int voice, int volume);
    void      (*mixer_set_pan)(int voice, int pan);
    int       (*mixer_voice_active)(int voice);
    void      (*mixer_pump)(void);
    void      (*mixer_set_loop)(int voice, int loop_start, int loop_end);
    void      (*mixer_set_rate)(int voice, int sample_rate_hz);
    void      (*mixer_set_rate_raw)(int voice, uint32_t rate_fp16);
    void      (*mixer_set_vol_lr)(int voice, int vol_l, int vol_r);
    void      (*mixer_set_bidi)(int voice, int enable);
    int       (*mixer_get_position)(int voice);
    void      (*mixer_set_position)(int voice, int sample_offset);
    void      (*mixer_set_voice)(int voice, int sample_rate_hz, int vol_l, int vol_r);
    void      (*mixer_set_voice_raw)(int voice, uint32_t rate_fp16, int vol_l, int vol_r);
    void      (*mixer_set_vol_rate)(int voice, int rate);
    uint32_t  (*mixer_poll_ended)(void);
    void *    (*mixer_alloc_samples)(uint32_t size);
    void      (*mixer_free_samples)(void);
    void      (*mixer_set_end_callback)(void (*cb)(uint32_t ended_mask));

    /* -- Audio (3) -- */
    void      (*audio_init)(void);
    int       (*audio_write)(const int16_t *samples, int count);
    int       (*audio_get_free)(void);

    /* -- Timer (5) -- */
    void      (*timer_set_callback)(void (*cb)(void), uint32_t hz);
    void      (*timer_stop)(void);
    uint32_t  (*timer_get_us)(void);
    uint32_t  (*timer_get_ms)(void);
    void      (*timer_delay_us)(uint32_t us);

    /* -- Cache (3) -- */
    void      (*cache_flush)(void);
    void      (*cache_clean_range)(void *addr, uint32_t size);
    void      (*cache_inval_range)(void *addr, uint32_t size);

    /* -- Vsync callback (1) -- */
    void      (*video_set_vsync_callback)(void (*cb)(void));

    /* -- File (2) -- */
    long      (*file_size)(const char *path);
    long      (*file_size_fd)(int fd);

    /* -- Mixer + audio extensions (5+4, append-only to preserve ABI) --
     *    The audio_stream_* slots logically belong to the audio module
     *    but live here for ABI append-only ordering. */
    void      (*mixer_retrigger)(int voice, const uint8_t *pcm_s16,
                                 uint32_t sample_count, uint32_t sample_rate,
                                 int volume);
    int       (*mixer_play_8bit)(const uint8_t *pcm_s8, uint32_t sample_count,
                                 uint32_t sample_rate, int priority, int volume);
    void      (*mixer_set_group)(int voice, int group);
    void      (*mixer_set_group_volume)(int group, int volume);
    void      (*mixer_set_master_volume)(int volume);
    void      (*mixer_set_filter)(int voice, int cutoff_q016, int q, int enable);
    int       (*audio_stream_open)(int sample_rate);
    int       (*audio_stream_write)(const int16_t *samples, int count);
    int       (*audio_stream_ready)(void);
    void      (*audio_stream_close)(void);

    /* -- Filesystem (1) -- append-only, ABI-stable --
     *    Register a filename→slot mapping for fopen() by name.
     *    The openFPGA manifest identifies data slots by numeric id;
     *    this service lets apps tell the kernel which id holds which
     *    filename so fopen() by name resolves correctly. Overwrites
     *    any prior mapping for the same filename. Max 16 entries. */
    void      (*file_slot_register)(uint32_t slot_id, const char *filename);

    /* -- SoundFont preload (append-only, ABI-stable) --
     *    The kernel auto-loads the first .ofsf file it finds in a data
     *    slot during boot. Apps should check smp_bank_preload_base and,
     *    when non-NULL, skip of_smp_bank_load() / of_mixer_alloc_samples
     *    and reuse the preloaded CRAM1 buffer directly. Older firmware
     *    leaves these as NULL/0. */
    const void *smp_bank_preload_base;
    uint32_t    smp_bank_preload_size;

    /* -- AWE coprocessor (RETIRED — slots kept for ABI stability) --
     *    The AWE fabric coprocessor was removed; all of these slots are
     *    wired to no-op stubs in services_table.c so SDK apps built
     *    against the old ABI still link.  They silently do nothing.
     *    Do NOT call these from new code; use the mixer + smp_voice
     *    paths instead.  See of_awe.h for matching app-side stubs. */
    void      (*awe_voice_load)(int voice, const struct awe_voice_t *v);
    void      (*awe_voice_trigger)(int voice);
    void      (*awe_voice_release)(int voice);
    void      (*awe_voice_stop)(int voice);
    void      (*awe_channel_set_volume)(int ch, int vol_0_127);
    void      (*awe_channel_set_expression)(int ch, int expr_0_127);
    void      (*awe_channel_set_pan)(int ch, int pan_0_127);
    void      (*awe_channel_set_bend)(int ch, int bend_signed_8192);
    void      (*awe_channel_set_mod)(int ch, int mod_0_127);
    void      (*awe_channel_set_sustain)(int ch, int on_off);
    void      (*awe_channel_set_brightness)(int ch, int br_0_127);
    void      (*awe_channel_set_resonance)(int ch, int q_0_127);
    void      (*awe_channel_set_reverb_send)(int ch, int send_0_255);
    void      (*awe_channel_set_chorus_send)(int ch, int send_0_255);
    void      (*awe_set_master_volume)(int vol_0_255);
    void      (*awe_set_bend_range)(int cents);
    uint64_t  (*awe_active_mask)(void);

    /* Retired — returns 0. */
    uint32_t  (*awe_tick_count)(void);

    /* Retired — no-op. */
    void      (*awe_set_hw_envelope)(int enabled);

    /* Retired — no-op. */
    void      (*awe_set_reverb_level)(int level);
    void      (*awe_set_reverb_feedback)(int feedback);

    /* Retired — no-op. */
    void      (*awe_set_chorus_level)(int level);
    void      (*awe_set_chorus_rate)(int rate);
    void      (*awe_set_chorus_depth)(int depth);

    /* Retired — no-op. */
    void      (*awe_ramp1_trigger)(int voice, int stage, uint32_t rate);

    /* -- Mixer group-aware allocation (append-only, ABI-stable) --
     *    Atomic alloc-and-tag entry; lets callers that know which
     *    group a new voice belongs to bias the slot search and steal
     *    paths so MUSIC and SFX don't collide in the same slot range.
     *    `mixer_voice_group` reads back the current tag for a slot
     *    (cheap shadow read) so callers can validate ownership before
     *    writing — used by the SW MIDI ISR to drop stale references
     *    when a slot has been reassigned to another group.  Older
     *    firmware leaves these as NULL; callers should fall back to
     *    of_mixer_play + of_mixer_set_group when these are absent. */
    int       (*mixer_alloc_for_group)(int group, const uint8_t *pcm_s16,
                                       uint32_t sample_count,
                                       uint32_t sample_rate,
                                       int priority, int volume);
    int       (*mixer_voice_group)(int voice);

    /* -- Cache (append-only) -- */
    /* Range-granular writeback + invalidate (cbo.flush per line).
     * On this VexiiRiscv config, cbo.clean alone has been observed
     * to leave dirty lines in L1 (the bank-preload + audio-mixer
     * paths both regressed when using clean-only).  Use this when
     * preparing a buffer to be read by an external AXI master like
     * the GPU's m_rd_* or the audio mixer's per-voice fetch — they
     * read DRAM directly, not through the CPU's cache.
     * Older firmware leaves this NULL; callers should fall back to
     * cache_flush() (full sweep) when this is absent. */
    void      (*cache_flush_range)(void *addr, uint32_t size);
};

#ifndef OF_PC

/* Populated by of_init.c's constructor from the AT_OF_SVC auxv tag.
 * Apps must not read this directly -- use the OF_SVC macro so the
 * indirection can change without breaking the API. */
extern const struct of_services_table *_of_svc_ptr;

#define OF_SVC (_of_svc_ptr)

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_SERVICES_H */
