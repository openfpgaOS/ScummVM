/*
 * openfpga_midi.cpp -- ScummVM MIDI driver bridging to of_smp_voice.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_midi.h"

#include "common/error.h"

extern "C" {
#include <of.h>
#include <of_smp_bank.h>
#include <of_smp_voice.h>
#include <string.h>
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
    if (_open) return MERR_ALREADY_OPEN;

    if (of_midi_init() != OF_MIDI_OK)
        return MERR_DEVICE_NOT_AVAILABLE;

    /* Without a SoundFont bank we have nothing to synthesize from --
     * let ScummVM fall back to its software OPL emu. */
    if (of_smp_bank_get() == nullptr)
        return MERR_DEVICE_NOT_AVAILABLE;

    /* Drive envelopes at 1 kHz from the machine-timer ISR. of_midi_init()
     * itself doesn't install a callback (that's of_midi_play()'s job for
     * file-based playback); for streamed events we own the timer. */
    of_timer_set_callback(smp_voice_tick, 1000);

    _open = true;
    return 0;
}

void OpenFPGAMidiDriver::close() {
    if (!_open) return;
    of_timer_stop();
    smp_voice_all_off_global();
    _open = false;
}

void OpenFPGAMidiDriver::send(uint32 b) {
    if (!_open) return;

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
