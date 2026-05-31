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

#ifdef NONSTANDARD_PORT
    static void *operator new(size_t size);
    static void operator delete(void *p) noexcept;
    static void operator delete(void *p, size_t) noexcept;
#endif

    int open() override;
    void close() override;
    bool isOpen() const override { return _open; }
    void send(uint32 b) override;
    void sysEx(const byte *msg, uint16 length) override;

    /* Schedule the MidiParser from the same 1 kHz hardware-timer clock
     * that advances smp_voice envelopes.  The IRQ records pending parser
     * ticks, while the backend pumps the callback from the main thread. */
    void setTimerCallback(void *timer_param,
                          Common::TimerManager::TimerProc timer_proc) override;

private:
    void silenceAll();
    void resetChannelState(int ch, bool resetProgram);
    void resetDeviceState(bool resetPrograms);

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

extern "C" void openfpga_midi_panic(void);
extern "C" void openfpga_midi_pause(bool pause);
void openfpga_midi_pump_pending(void);

#ifdef NONSTANDARD_PORT
MidiDriver::DeviceHandle openfpga_midi_device_handle();
bool openfpga_midi_is_device_handle(MidiDriver::DeviceHandle handle);
MidiDriver *openfpga_midi_create_driver();
#endif

#endif /* OPENFPGA_MIDI_H */
