/*
 * openfpga_midi.h -- ScummVM MIDI driver backed by openfpgaOS's
 *                    sample-based synth (of_smp_voice + .ofsf bank).
 *
 * Translates ScummVM MIDI events (note on/off, controllers, program
 * change, pitch bend) into smp_voice_* calls. Envelopes are advanced
 * by smp_voice_tick installed as a 1 kHz machine-timer ISR.
 *
 * Requires a .ofsf bank to be staged in a data slot — the kernel auto-
 * loads it at boot. open() fails with MERR_DEVICE_NOT_AVAILABLE if no
 * bank is present, which lets ScummVM fall back to software OPL.
 */

#ifndef OPENFPGA_MIDI_H
#define OPENFPGA_MIDI_H

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "audio/mpu401.h"
#include "audio/musicplugin.h"
#include "base/plugins.h"

class OpenFPGAMidiDriver : public MidiDriver_MPU401 {
public:
    OpenFPGAMidiDriver();
    ~OpenFPGAMidiDriver() override;

    int open() override;
    void close() override;
    bool isOpen() const override { return _open; }
    void send(uint32 b) override;

private:
    bool    _open;
    uint8_t _program[16];     /* GM program (0..127) per channel */
    uint8_t _volume[16];      /* CC7 channel volume   (0..127, default 100) */
    uint8_t _expression[16];  /* CC11 expression      (0..127, default 127) */
    uint8_t _brightness[16];  /* CC74                 (0..127, default 64)  */
    uint8_t _resonance[16];   /* CC71                 (0..127, default 64)  */
};

class OpenFPGAMidiPlugin : public MusicPluginObject {
public:
    const char *getName() const override { return "openfpgaOS MIDI"; }
    const char *getId() const override   { return "openfpga"; }
    MusicDevices getDevices() const override;
    Common::Error createInstance(MidiDriver **mididriver,
                                 MidiDriver::DeviceHandle = 0) const override;
};

/* Plugin provider exposing OpenFPGAMidiPlugin to the plugin manager.
 *
 * REGISTER_PLUGIN_STATIC alone is not enough on this build: with
 * -ffunction-sections -fdata-sections + --gc-sections, the linker drops
 * any static plugin object that nothing in the main reachability graph
 * touches. Vanilla ScummVM wires statics in via LINK_PLUGIN(...) calls
 * in StaticPluginProvider::getPlugins(); to avoid patching that file we
 * register our own provider from main(). */
PluginProvider *createOpenFPGAMidiPluginProvider();

#endif /* OPENFPGA_MIDI_H */
