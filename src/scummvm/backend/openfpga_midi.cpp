/*
 * openfpga_midi.cpp -- ScummVM MIDI driver bridging to of_smp_voice.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_midi.h"

#include "common/error.h"
#include "common/timer.h"

extern "C" {
#include <of.h>
#include <of_smp_bank.h>
#include <of_smp_voice.h>
#include <string.h>
}

namespace {
/* IRQ-side state for the multiplexed timer tick.
 *
 *  - smp_voice_tick advances voice envelopes every tick (~1 kHz).
 *  - The MidiParser timer proc, when installed, fires every
 *    MIDI_TICK_US microseconds of wall-clock time (matches
 *    MidiDriver_MPU401::getBaseTempo() == 10000 us, i.e. 100 Hz).
 *
 * We drive the MIDI dispatch from of_time_us() instead of an IRQ
 * counter so the music tempo stays correct even if the SDK quantises
 * or jitters the requested 1 kHz timer rate -- a counter-only
 * dispatch was producing half-speed playback on hardware, which is
 * the classic symptom of the actual IRQ rate being lower than
 * requested.
 *
 * send() and smp_voice_* are single-threaded with respect to this
 * ISR (the SDK voice engine is already designed to be driven from
 * it), so dispatching MIDI events directly from here is safe and
 * gives sample-accurate timing -- Note Off lands at exactly the
 * scheduled tick instead of waiting for the next pollEvent. */
#define MIDI_TICK_US  10000u   /* must match MidiDriver_MPU401::getBaseTempo() */

Common::TimerManager::TimerProc g_midiTimerProc  = nullptr;
void                           *g_midiTimerParam = nullptr;
uint32_t                        g_midiNextDispatchUs = 0;

/* IRQ runs in M-mode; calling of_time_us() (an ecall) from inside the
 * IRQ handler re-enters the OS trap dispatcher and corrupts the saved
 * return context.
 *
 * Keep the IRQ minimal: just run the SMP voice envelope tick.  Timing
 * for the iMUSE dispatch is done from the main thread using of_time_us
 * (safe to ecall from there) so it tracks real wall-clock regardless
 * of the IRQ rate the SDK actually configured. */
void midi_tick_irq(void) {
    smp_voice_tick();
}
} // namespace

/* Called from the main thread (via pollEvent in our OSystem backend).
 * Computes elapsed wall-clock since the last call and dispatches the
 * appropriate number of MIDI_TICK_US slots to iMUSE.  Tracks real time
 * regardless of the actual IRQ frequency. */
void openfpga_midi_pump_pending() {
    if (!g_midiTimerProc) return;
    static uint32_t lastUs = 0;
    uint32_t now = of_time_us();
    if (lastUs == 0) { lastUs = now; return; }
    int32_t elapsed = (int32_t)(now - lastUs);
    if (elapsed < (int32_t)MIDI_TICK_US) return;
    int slots = elapsed / MIDI_TICK_US;
    /* Dispatch all pending slots so Note-Off events fire on time and
     * notes don't ring forever.  iMUSE's timer callback is cheap
     * (~bookkeeping + per-event send()); even a 200-slot catch-up
     * burst after a stall is sub-millisecond.  We still ratchet
     * lastUs to NOW so we don't keep replaying old time. */
    if (slots > 256) slots = 256;     /* sanity ceiling */
    lastUs += slots * MIDI_TICK_US;
    for (int i = 0; i < slots; ++i)
        g_midiTimerProc(g_midiTimerParam);
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

int OpenFPGAMidiDriver::open() {
    warning("[openfpga_midi] open()");
    if (_open) return MERR_ALREADY_OPEN;

    if (of_midi_init() != OF_MIDI_OK) {
        warning("[openfpga_midi] of_midi_init FAILED");
        return MERR_DEVICE_NOT_AVAILABLE;
    }

    if (of_smp_bank_get() == nullptr) {
        warning("[openfpga_midi] SMP bank NOT loaded");
        return MERR_DEVICE_NOT_AVAILABLE;
    }
    warning("[openfpga_midi] open OK -- MIDI driver active");

    /* Single 1 kHz IRQ drives both voice envelopes and the MidiParser
     * tempo callback (see midi_tick_irq in the anon namespace above).
     * of_midi_init() itself doesn't install a callback -- that's
     * of_midi_play()'s job for file-based playback. */
    of_timer_set_callback(midi_tick_irq, 1000);

    _open = true;
    return 0;
}

void OpenFPGAMidiDriver::close() {
    if (!_open) return;
    of_timer_stop();
    g_midiTimerProc  = nullptr;
    g_midiTimerParam = nullptr;
    smp_voice_all_off_global();
    _open = false;
}

void OpenFPGAMidiDriver::setTimerCallback(void *timer_param,
                                          Common::TimerManager::TimerProc timer_proc) {
    /* Don't go through DefaultTimerManager; route the proc straight
     * to our hardware-timer ISR so events fire on the cycle they're
     * scheduled on.  Seed the next-dispatch deadline relative to
     * the current wall clock so the very first tick fires roughly
     * one base-tempo period after install (avoids back-to-back
     * dispatches if of_time_us happened to be near zero). */
    g_midiTimerParam     = timer_param;
    g_midiTimerProc      = timer_proc;
}

void OpenFPGAMidiDriver::send(uint32 b) {
    if (!_open) return;

    /* TEMP trace: every 256th MIDI msg + every note-on. */
    {
        static uint32 sendCount  = 0;
        static uint32 noteOnCount = 0;
        ++sendCount;
        const uint8_t st = b & 0xF0;
        if (st == 0x90 && ((b >> 16) & 0x7F) > 0) {
            ++noteOnCount;
            if ((noteOnCount % 16) == 1)
                warning("[openfpga_midi] note-on #%u msg=0x%06x", noteOnCount, b);
        }
        if ((sendCount % 512) == 1)
            warning("[openfpga_midi] sends=%u note-ons=%u", sendCount, noteOnCount);
    }

    const uint8_t status = b & 0xFF;
    const uint8_t ch     = status & 0x0F;
    const uint8_t d1     = (b >> 8)  & 0x7F;
    const uint8_t d2     = (b >> 16) & 0x7F;

    /* GM percussion lives on channel 10 (index 9) in bank 128. */
    const int bank = (ch == 9) ? 128 : 0;

    switch (status & 0xF0) {
    case 0x80:  /* Note off */
        smp_voice_note_off(ch, d1);
        break;

    case 0x90:  /* Note on (vel 0 == note off) */
        if (d2 == 0) {
            smp_voice_note_off(ch, d1);
        } else {
            const ofsf_zone_t *zones[8];
            int n = of_smp_zone_lookup(bank, _program[ch], d1, d2, zones, 8);
            const void *base = of_smp_bank_sample_base();
            for (int i = 0; i < n; i++)
                smp_voice_note_on(zones[i], ch, d1, d2, base);
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
        case 0x7B:  /* CC123 all notes off */
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
