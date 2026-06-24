/*
 * openfpga_midi.cpp -- ScummVM MIDI driver bridging to of_smp_voice.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_midi.h"
#include "openfpga_memory.h"
#include "openfpga_mixer.h"

#include "common/debug.h"
#include "common/error.h"
#include "common/timer.h"

extern "C" {
#include <of.h>
#include <of_mixer.h>
#include <of_smp_bank.h>
#include <of_smp_voice.h>
#include <string.h>
#include <stdio.h>
}

extern "C" void midi_tick_irq(void);

namespace {
/* IRQ-side state for the multiplexed timer tick.
 *
 *  - smp_voice_tick advances voice envelopes every tick (~1 kHz).
 *  - The MidiParser timer proc, when installed, is scheduled from this
 *    IRQ every MIDI_TICK_US microseconds of wall-clock time (matches
 *    MidiDriver_MPU401::getBaseTempo() == 10000 us, i.e. 100 Hz).
 *
 * The parser callback itself must not run from IRQ.  Some iMUSE paths
 * allocate, touch engine state, or re-enter services that assume the
 * normal app stack.  The IRQ only records pending ticks; OSystem pumps
 * them from the main thread. */
#define MIDI_TICK_US  10000u   /* must match MidiDriver_MPU401::getBaseTempo() */
#define MIDI_STUCK_NOTE_TIMEOUT_US 4000000u
/* Max time a channel may keep the sustain pedal (CC64) latched before we
 * force-release it.  A note-off received while sustain is held parks a
 * looping voice in ENV_SUSTAIN until the sustain-off arrives; iMUSE jumps /
 * track changes occasionally drop that sustain-off, hanging pads/organ
 * forever.  Bound it.  Generous so legitimately long pedal passages survive;
 * only voices already key-released (sustain_held) are affected. */
#define MIDI_SUSTAIN_MAX_US 8000000u
#define MIDI_PANIC_QUIET_US 250000u
#define MIDI_MAX_PENDING_TICKS 512u
#define MIDI_MAX_PUMP_TICKS 128u

volatile Common::TimerManager::TimerProc g_midiTimerProc  = nullptr;
void                                    *g_midiTimerParam = nullptr;
volatile uint32_t                        g_midiLastDispatchUs = 0;
volatile uint16_t                        g_midiPendingTicks = 0;
volatile uint32_t                        g_midiSuppressUntilUs = 0;
volatile uint32_t                        g_midiLastEventUs = 0;
volatile uint16_t                        g_midiActiveNoteCount = 0;
volatile bool                            g_midiPaused = false;
volatile bool                            g_midiTimerArmed = false;
volatile bool                            g_midiInIrq = false;
volatile bool                            g_midiPanicActive = false;
uint32_t                                 g_midiActiveNotes[16][4];
/* Per-channel timestamp of when CC64 sustain was engaged (0 = pedal up).
 * Used by the sustain-hang watchdog in midi_tick_irq. */
volatile uint32_t                        g_sustainSinceUs[16];

void stopMusicMixerVoices() {
    /* Do not call of_mixer_stop_all() from MIDI panic.  ScummVM speech,
     * SFX, and CDDA share the same SDK audio service, so a broad mixer
     * stop can silence non-MIDI audio after a skip/pause transition.
     * Stop only mixer voices tagged as music; smp_voice_all_off_global()
     * already clears every voice the software synth still owns, and this
     * loop catches any stale music handles left in firmware state. */
    for (int i = 0; i < OF_MIXER_MAX_VOICES; ++i) {
        if (!of_mixer_voice_active(i))
            continue;
        int group = of_mixer_voice_group(i);
        if (group == OF_MIXER_GROUP_MUSIC)
            of_mixer_stop(i);
    }
}

void clearActiveNotes() {
    memset(g_midiActiveNotes, 0, sizeof(g_midiActiveNotes));
    g_midiActiveNoteCount = 0;
}

void clearChannelNotes(uint8_t ch) {
    if (ch >= 16)
        return;
    uint16_t removed = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t bits = g_midiActiveNotes[ch][i];
        while (bits) {
            bits &= bits - 1;
            ++removed;
        }
        g_midiActiveNotes[ch][i] = 0;
    }
    g_midiActiveNoteCount = (removed >= g_midiActiveNoteCount)
        ? 0 : (uint16_t)(g_midiActiveNoteCount - removed);
}

void markNoteOn(uint8_t ch, uint8_t note) {
    if (ch >= 16 || note >= 128)
        return;
    uint32_t mask = 1u << (note & 31);
    uint32_t &word = g_midiActiveNotes[ch][note >> 5];
    if (!(word & mask)) {
        word |= mask;
        if (g_midiActiveNoteCount != 0xFFFF)
            ++g_midiActiveNoteCount;
    }
}

void markNoteOff(uint8_t ch, uint8_t note) {
    if (ch >= 16 || note >= 128)
        return;
    uint32_t mask = 1u << (note & 31);
    uint32_t &word = g_midiActiveNotes[ch][note >> 5];
    if (word & mask) {
        word &= ~mask;
        if (g_midiActiveNoteCount)
            --g_midiActiveNoteCount;
    }
}

bool midiSuppressed(uint32_t now) {
    return g_midiSuppressUntilUs &&
           (int32_t)(now - g_midiSuppressUntilUs) < 0;
}

void panicVoicesUnlocked(bool suppressParser) {
    for (int ch = 0; ch < 16; ++ch) {
        smp_voice_update_sustain(ch, false);
        g_sustainSinceUs[ch] = 0;
    }
    smp_voice_all_off_global();
    stopMusicMixerVoices();
    clearActiveNotes();
    g_midiLastEventUs = 0;
    g_midiPendingTicks = 0;
    g_midiLastDispatchUs = OF_SVC ? OF_SVC->timer_get_us() : 0;
    g_midiSuppressUntilUs = (suppressParser && g_midiLastDispatchUs)
        ? g_midiLastDispatchUs + MIDI_PANIC_QUIET_US : 0;
}

void panicVoices(bool suppressParser) {
    const bool stopTimer = g_midiTimerArmed && !g_midiInIrq;

    /* The SDK file-MIDI player stops its timer before tearing down
     * smp_voice state. Do the same from ScummVM's main-thread panic
     * paths; otherwise a 1 kHz voice tick can race the all-off walk and
     * leave a hardware mixer voice sounding after pause/skip/save. */
    if (stopTimer) {
        g_midiPanicActive = true;
        of_timer_stop();
    }

    panicVoicesUnlocked(suppressParser);

    if (stopTimer) {
        g_midiPanicActive = false;
        of_timer_set_callback(midi_tick_irq, 1000);
    }
}

#ifdef NONSTANDARD_PORT
MidiDriver::DeviceHandle hashLiteral(const char *s) {
    if (!s || !s[0])
        return 0;

    uint hash = ((uint)(byte)s[0]) << 7;
    uint len = 0;
    while (s[len]) {
        hash = (1000003U * hash) ^ (uint)(byte)s[len];
        ++len;
    }

    return (MidiDriver::DeviceHandle)(hash ^ len);
}
#endif
} // namespace

/* Externally linkable so the mixer can register it as soon as audio
 * comes up (rather than waiting for the MIDI driver to open). */
extern "C" void midi_tick_irq(void) {
    g_midiInIrq = true;
    if (g_midiPanicActive) {
        g_midiInIrq = false;
        return;
    }

    smp_voice_tick();

    /* Do not dispatch ScummVM MIDI events here.  The parser can allocate
     * and manipulate engine structures; running it on the machine-timer
     * stack corrupts malloc-heavy games such as Day of the Tentacle.
     *
     * Do not call of_time_us() from IRQ: it is an ecall and nests traps.
     * The service-table timer read is direct and is already used from
     * IRQ by the SDK MIDI player. */
    Common::TimerManager::TimerProc proc =
        (Common::TimerManager::TimerProc)g_midiTimerProc;
    if (proc) {
        uint32_t now = OF_SVC->timer_get_us();
        if (g_midiPaused || midiSuppressed(now)) {
            g_midiLastDispatchUs = now;
            g_midiPendingTicks = 0;
        } else if (g_midiLastDispatchUs == 0) {
            g_midiLastDispatchUs = now;
        } else {
            int32_t elapsed = (int32_t)(now - g_midiLastDispatchUs);
            if (elapsed >= (int32_t)MIDI_TICK_US) {
                int slots = elapsed / (int32_t)MIDI_TICK_US;
                if (slots > 2)
                    slots = 2;
                g_midiLastDispatchUs += slots * MIDI_TICK_US;
                uint32_t pending = g_midiPendingTicks + slots;
                g_midiPendingTicks = (uint16_t)((pending > MIDI_MAX_PENDING_TICKS)
                    ? MIDI_MAX_PENDING_TICKS : pending);
            }
        }

        if (g_midiActiveNoteCount && g_midiLastEventUs &&
            (uint32_t)(now - g_midiLastEventUs) > MIDI_STUCK_NOTE_TIMEOUT_US) {
            panicVoicesUnlocked(true);
        }

        /* Sustain-hang watchdog: if a channel held the sustain pedal longer
         * than MIDI_SUSTAIN_MAX_US, the sustain-off was almost certainly
         * dropped across an iMUSE jump/track change.  Force-release just that
         * channel (releases key-up voices parked in ENV_SUSTAIN) -- targeted,
         * so no global all-off / song mute. */
        for (int ch = 0; ch < 16; ++ch) {
            if (g_sustainSinceUs[ch] &&
                (uint32_t)(now - g_sustainSinceUs[ch]) > MIDI_SUSTAIN_MAX_US) {
                smp_voice_update_sustain(ch, false);
                g_sustainSinceUs[ch] = 0;
            }
        }
    }

    /* IRQ-side audio drain is intentionally NOT done here.  Tried both
     * stream_write (mono API, heap-corrupted) and audio_write (stereo,
     * low-level FIFO); the latter caused audible modulation/vibrato instead
     * of crashes but is still wrong.  Hypothesis: of_audio_write's FIFO write
     * path is non-reentrant in some way that's only exposed at sub-1 ms
     * preemption granularity.  Audio drain runs entirely from the main thread
     * (delayMillis 1 ms tick + pollEvent/updateScreen). */
    g_midiInIrq = false;
}

void openfpga_midi_pump_pending(void) {
    /* Reap orphaned HW music voices left looping by the SDK mixer's stale-handle
     * path (see smp_voice_reap_orphans).  Main-thread, cheap, runs every pump so
     * a dropped looping voice drones for at most ~one frame instead of forever. */
    smp_voice_reap_orphans();

    Common::TimerManager::TimerProc proc =
        (Common::TimerManager::TimerProc)g_midiTimerProc;
    void *param = g_midiTimerParam;

    if (!proc) {
        g_midiPendingTicks = 0;
        return;
    }

    uint32_t now = OF_SVC ? OF_SVC->timer_get_us() : 0;
    if (g_midiPaused || midiSuppressed(now)) {
        g_midiPendingTicks = 0;
        return;
    }
    if (g_midiSuppressUntilUs)
        g_midiSuppressUntilUs = 0;

    uint16_t ticks = g_midiPendingTicks;
    if (!ticks)
        return;
    if (ticks > MIDI_MAX_PUMP_TICKS)
        ticks = MIDI_MAX_PUMP_TICKS;

    g_midiPendingTicks = (g_midiPendingTicks > ticks)
        ? (uint16_t)(g_midiPendingTicks - ticks) : 0;

    for (uint16_t i = 0; i < ticks; ++i)
        proc(param);
}

OpenFPGAMidiDriver::OpenFPGAMidiDriver() : _open(false) {
    memset(_program, 0, sizeof(_program));
    for (int i = 0; i < 16; i++) {
        _volume[i]     = 100;
        _expression[i] = 127;
        _brightness[i] = 64;
        _resonance[i]  = 64;
    }
}

OpenFPGAMidiDriver::~OpenFPGAMidiDriver() {
    if (_open) close();
}

#ifdef NONSTANDARD_PORT
void *OpenFPGAMidiDriver::operator new(size_t size) {
    return openfpga_pool_alloc(kOpenFPGAPoolMidiDriver, size);
}

void OpenFPGAMidiDriver::operator delete(void *p) noexcept {
    openfpga_pool_free(kOpenFPGAPoolMidiDriver, p);
}

void OpenFPGAMidiDriver::operator delete(void *p, size_t) noexcept {
    OpenFPGAMidiDriver::operator delete(p);
}
#endif

int OpenFPGAMidiDriver::open() {
    debug(1, "[openfpga_midi] open()");
    if (_open) return MERR_ALREADY_OPEN;

    if (of_midi_init() != OF_MIDI_OK) {
        warning("[openfpga_midi] of_midi_init FAILED");
        return MERR_DEVICE_NOT_AVAILABLE;
    }

    if (of_smp_bank_get() == nullptr) {
        warning("[openfpga_midi] SMP bank NOT loaded");
        return MERR_DEVICE_NOT_AVAILABLE;
    }
    debug(1, "[openfpga_midi] open OK -- MIDI driver active");
    resetDeviceState(true);

    /* Single 1 kHz IRQ drives both voice envelopes and the MidiParser
     * tempo callback (see midi_tick_irq in the anon namespace above).
     * of_midi_init() itself doesn't install a callback -- that's
     * of_midi_play()'s job for file-based playback. */
    of_timer_set_callback(midi_tick_irq, 1000);
    g_midiTimerArmed = true;

    _open = true;
    return 0;
}

void OpenFPGAMidiDriver::close() {
    if (!_open) return;
    of_timer_stop();
    g_midiTimerArmed = false;
    g_midiTimerProc  = nullptr;
    g_midiTimerParam = nullptr;
    g_midiLastDispatchUs = 0;
    g_midiPendingTicks = 0;
    g_midiSuppressUntilUs = 0;
    g_midiPaused = false;
    silenceAll();
    resetDeviceState(true);
    _open = false;
}

void OpenFPGAMidiDriver::setTimerCallback(void *timer_param,
                                          Common::TimerManager::TimerProc timer_proc) {
    Common::TimerManager::TimerProc oldProc =
        (Common::TimerManager::TimerProc)g_midiTimerProc;
    void *oldParam = g_midiTimerParam;

    if (_open && oldProc && (!timer_proc ||
        oldProc != timer_proc || oldParam != timer_param)) {
        g_midiTimerProc = nullptr;
        g_midiTimerParam = nullptr;
        g_midiLastDispatchUs = 0;
        g_midiPendingTicks = 0;
        g_midiSuppressUntilUs = 0;
        g_midiPaused = false;
        silenceAll();
    }

    g_midiTimerParam     = timer_param;
    g_midiLastDispatchUs = 0;
    g_midiPendingTicks   = 0;
    g_midiTimerProc      = timer_proc;
}

void OpenFPGAMidiDriver::silenceAll() {
    panicVoices(false);
}

void OpenFPGAMidiDriver::resetChannelState(int ch, bool resetProgram) {
    if (ch < 0 || ch > 15)
        return;
    if (resetProgram) {
        _program[ch] = 0;
        _volume[ch] = 100;
    }
    _expression[ch] = 127;
    _brightness[ch] = 64;
    _resonance[ch]  = 64;
    smp_voice_update_mod(ch, 0);
    smp_voice_update_sustain(ch, false);
    g_sustainSinceUs[ch] = 0;
    smp_voice_update_volume(ch, _volume[ch], _expression[ch]);
    smp_voice_update_pan(ch, 64);
    smp_voice_update_bend(ch, 0);
    smp_voice_update_filter(ch, _brightness[ch], _resonance[ch]);
    smp_voice_update_reverb_send(ch, 40);
    smp_voice_update_chorus_send(ch, 0);
}

void OpenFPGAMidiDriver::resetDeviceState(bool resetPrograms) {
    for (int ch = 0; ch < 16; ++ch)
        resetChannelState(ch, resetPrograms);
}

void OpenFPGAMidiDriver::send(uint32 b) {
    if (!_open) return;

    g_midiLastEventUs = OF_SVC->timer_get_us();

    const uint8_t status = b & 0xFF;
    const uint8_t ch     = status & 0x0F;
    const uint8_t d1     = (b >> 8)  & 0x7F;
    const uint8_t d2     = (b >> 16) & 0x7F;

    /* Pause menus may still run small scripts after the engine-level panic
     * clear.  Do not let those scripts start new hardware voices while the
     * MIDI clock is stopped, or the matching note-off will not arrive until
     * the menu exits.  Note-offs and controller resets still pass through. */
    const bool blockNewNotes = g_midiPaused || midiSuppressed(g_midiLastEventUs);
    if (blockNewNotes && (status & 0xF0) == 0x90 && d2 != 0)
        return;

    /* GM percussion lives on channel 10 (index 9) in bank 128. */
    const int bank = (ch == 9) ? 128 : 0;

    switch (status & 0xF0) {
    case 0x80:  /* Note off */
        markNoteOff(ch, d1);
        smp_voice_note_off(ch, d1);
        break;

    case 0x90:  /* Note on (vel 0 == note off) */
        if (d2 == 0) {
            markNoteOff(ch, d1);
            smp_voice_note_off(ch, d1);
        } else {
            const ofsf_zone_t *zones[8];
            int n = of_smp_zone_lookup(bank, _program[ch], d1, d2, zones, 8);
            const void *base = of_smp_bank_sample_base();
            for (int i = 0; i < n; i++)
                smp_voice_note_on(zones[i], ch, d1, d2, base);
            if (n > 0)
                markNoteOn(ch, d1);
        }
        break;

    case 0xB0:  /* Control change */
        switch (d1) {
        case 0x01:  /* CC1 mod wheel */
            smp_voice_update_mod(ch, d2);
            break;
        case 0x07:  /* CC7 channel volume */
            _volume[ch] = d2;
            smp_voice_update_volume(ch, _volume[ch], _expression[ch]);
            break;
        case 0x0A:  /* CC10 pan */
            smp_voice_update_pan(ch, d2);
            break;
        case 0x0B:  /* CC11 expression */
            _expression[ch] = d2;
            smp_voice_update_volume(ch, _volume[ch], _expression[ch]);
            break;
        case 0x40:  /* CC64 sustain */
            smp_voice_update_sustain(ch, d2 >= 64);
            if (d2 >= 64) {
                if (!g_sustainSinceUs[ch])
                    g_sustainSinceUs[ch] = g_midiLastEventUs;  /* engaged now */
            } else {
                g_sustainSinceUs[ch] = 0;                      /* released */
            }
            break;
        case 0x47:  /* CC71 resonance */
            _resonance[ch] = d2;
            smp_voice_update_filter(ch, _brightness[ch], _resonance[ch]);
            break;
        case 0x4A:  /* CC74 brightness / cutoff */
            _brightness[ch] = d2;
            smp_voice_update_filter(ch, _brightness[ch], _resonance[ch]);
            break;
        case 0x5B:  /* CC91 reverb send */
            smp_voice_update_reverb_send(ch, d2);
            break;
        case 0x5D:  /* CC93 chorus send */
            smp_voice_update_chorus_send(ch, d2);
            break;
        case 0x78:  /* CC120 all sound off */
            smp_voice_update_sustain(ch, false);
            g_sustainSinceUs[ch] = 0;
            clearChannelNotes(ch);
            smp_voice_all_off(ch);
            break;
        case 0x79:  /* CC121 reset all controllers */
            resetChannelState(ch, false);
            break;
        case 0x7B:  /* CC123 all notes off */
        case 0x7C:  /* CC124 omni off, also all notes off */
        case 0x7D:  /* CC125 omni on, also all notes off */
        case 0x7E:  /* CC126 mono on, also all notes off */
        case 0x7F:  /* CC127 poly on, also all notes off */
            smp_voice_update_sustain(ch, false);
            g_sustainSinceUs[ch] = 0;
            clearChannelNotes(ch);
            smp_voice_all_off(ch);
            break;
        }
        break;

    case 0xC0:  /* Program change */
        _program[ch] = d1;
        break;

    case 0xE0: { /* Pitch bend (14-bit, centered at 8192) */
        int bend = (int)((d2 << 7) | d1) - 8192;
        smp_voice_update_bend(ch, bend);
        break;
    }
    }
}

void OpenFPGAMidiDriver::sysEx(const byte *msg, uint16 length) {
    if (!_open || !msg || !length)
        return;

    /* GM/GS reset messages mean the upstream driver is replacing the
     * current music state.  The sample synth has no native SysEx, but
     * it must still behave like a real module: silence outstanding
     * voices and restore channel defaults so interrupted notes cannot
     * survive the reset.  ScummVM passes SysEx payloads without F0/F7. */
    bool reset = false;
    if (length >= 4 && msg[0] == 0x7E && msg[2] == 0x09 &&
        (msg[3] == 0x01 || msg[3] == 0x02 || msg[3] == 0x03)) {
        reset = true; /* GM System On/Off/GM2 On */
    } else if (length >= 9 && msg[0] == 0x41 && msg[2] == 0x42 &&
               msg[3] == 0x12 && msg[4] == 0x40 &&
               msg[5] == 0x00 && msg[6] == 0x7F) {
        reset = true; /* Roland GS Reset */
    }

    if (reset) {
        silenceAll();
        resetDeviceState(true);
    }
}

extern "C" void openfpga_midi_panic(void) {
    panicVoices(true);
}

extern "C" void openfpga_midi_pause(bool pause) {
    if (pause) {
        g_midiPaused = true;
        panicVoices(false);
        return;
    }

    g_midiPendingTicks = 0;
    g_midiSuppressUntilUs = 0;
    g_midiLastDispatchUs = OF_SVC ? OF_SVC->timer_get_us() : 0;
    g_midiPaused = false;
}

/* ─── Plugin glue ────────────────────────────────────────────────── */

MusicDevices OpenFPGAMidiPlugin::getDevices() const {
    MusicDevices devices;
    devices.push_back(MusicDevice(this, "", MT_GM));
    return devices;
}

Common::Error OpenFPGAMidiPlugin::createInstance(MidiDriver **mididriver,
                                                  MidiDriver::DeviceHandle) const {
    *mididriver = new OpenFPGAMidiDriver();
    return Common::kNoError;
}

#ifdef NONSTANDARD_PORT
MidiDriver::DeviceHandle openfpga_midi_device_handle() {
    return hashLiteral("openfpga");
}

bool openfpga_midi_is_device_handle(MidiDriver::DeviceHandle handle) {
    return handle != 0 && handle == openfpga_midi_device_handle();
}

MidiDriver *openfpga_midi_create_driver() {
    return new OpenFPGAMidiDriver();
}
#endif

REGISTER_PLUGIN_STATIC(OPENFPGA_MIDI, PLUGIN_TYPE_MUSIC, OpenFPGAMidiPlugin);

/* Forward decls of the symbols REGISTER_PLUGIN_STATIC defined just above —
 * the provider below references them so --gc-sections keeps everything
 * connected to a real load chain. */
extern const PluginType g_OPENFPGA_MIDI_type;
extern PluginObject *g_OPENFPGA_MIDI_getObject();

namespace {
class OpenFPGAMidiPluginProvider : public PluginProvider {
public:
    PluginList getPlugins() override {
        PluginList pl;
        pl.push_back(new StaticPlugin(g_OPENFPGA_MIDI_getObject(),
                                       g_OPENFPGA_MIDI_type));
        return pl;
    }
};
}

PluginProvider *createOpenFPGAMidiPluginProvider() {
    return new OpenFPGAMidiPluginProvider();
}
