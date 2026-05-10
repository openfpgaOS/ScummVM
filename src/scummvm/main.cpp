/*
 * ScummVM for openfpgaOS — Direct engine launcher.
 *
 * Bypasses scummvm_main / GUI and runs the SCUMM engine directly with
 * a single hardcoded game (Monkey Island 1 VGA floppy).
 *
 * Music routes through the openfpgaOS sample-based MIDI driver
 * (backend/openfpga_midi.cpp), which fronts of_smp_voice + a .ofsf
 * SoundFont bank. If no bank is staged, the driver fails open() and
 * ScummVM falls back to its software OPL emulator.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "common/str.h"
#include "common/config-manager.h"
#include "common/events.h"
#include "common/textconsole.h"
#include "base/version.h"
#include "base/plugins.h"
#include "base/commandLine.h"
#include "engines/engine.h"
#include "engines/metaengine.h"
#include "backend/openfpga_osystem.h"
#include "backend/openfpga_fs.h"
#include "backend/openfpga_midi.h"

extern "C" {
#include <of.h>
#include <stdio.h>
#include <unistd.h>
}

/* Print a status line at the next available row of the overlay terminal.
 * Wraps back to row 1 once we hit the bottom. */
static void status(const char *msg) {
    static int row = 1;
    printf("\x1b[%d;1H%s\n", row++, msg);
    fflush(stdout);
    if (row >= 29) row = 1;
}

extern "C" int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_OVERLAY);
    of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
    of_audio_init();
    of_input_set_deadzone(4000);
    printf("\x1b[2J");
    fflush(stdout);

    status("ScummVM openfpgaOS");

    /* Map game data files to APF data slots. */
    openfpga_fs_register(3,  "000.LFL");
    openfpga_fs_register(4,  "DISK01.LEC");
    openfpga_fs_register(5,  "DISK02.LEC");
    openfpga_fs_register(6,  "DISK03.LEC");
    openfpga_fs_register(7,  "DISK04.LEC");
    openfpga_fs_register(8,  "901.LFL");
    openfpga_fs_register(20, "902.LFL");
    openfpga_fs_register(21, "903.LFL");
    openfpga_fs_register(22, "904.LFL");

    status("Init backend...");
    g_system = new OSystem_OpenFPGA();
    g_system->initBackend();

    status("Config...");
    Base::registerDefaults();
    g_system->registerDefaultSettings(Common::ConfigManager::kApplicationDomain);

    /* Hardcoded Monkey Island 1 VGA floppy config. */
    ConfMan.addGameDomain("monkey");
    ConfMan.set("gameid",      "monkey",                          "monkey");
    ConfMan.set("description", "The Secret of Monkey Island",     "monkey");
    ConfMan.set("engineid",    "scumm",                           "monkey");
    ConfMan.set("path",        ".",                               "monkey");
    ConfMan.set("platform",    "pc",                              "monkey");
    ConfMan.set("language",    "en",                              "monkey");
    /* Route music through our SDK-backed MIDI driver. The MusicPluginObject
     * id is "openfpga" (see backend/openfpga_midi.cpp). */
    ConfMan.set("music_driver", "openfpga", "monkey");
    ConfMan.setInt("music_volume", 192,    "monkey");
    ConfMan.setInt("sfx_volume",   192,    "monkey");
    ConfMan.setBool("subtitles",   true,   "monkey");
    ConfMan.setActiveDomain("monkey");

    status("Plugins...");
    PluginManager::instance().init();
    /* Register our MIDI plugin provider before loading -- StaticPluginProvider
     * doesn't know about it (we don't patch ScummVM's plugins.cpp). */
    PluginManager::instance().addPluginProvider(createOpenFPGAMidiPluginProvider());
    PluginManager::instance().loadAllPlugins();
    PluginManager::instance().loadDetectionPlugin();

    status("Finding engine...");
    const Plugin *enginePlugin = nullptr;
    const PluginList &plugins = EngineMan.getPlugins(PLUGIN_TYPE_ENGINE);
    for (PluginList::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        if (Common::String((*it)->getName()).contains("SCUMM") ||
            Common::String((*it)->getName()).contains("scumm")) {
            enginePlugin = *it;
            break;
        }
    }

    if (!enginePlugin) {
        status("ERROR: SCUMM engine not found");
        for (;;) { of_input_poll(); usleep(100 * 1000); }
    }

    status("Creating engine...");
    MetaEngine &metaEngine = enginePlugin->get<MetaEngine>();
    metaEngine.registerDefaultSettings("monkey");

    Engine *engine = nullptr;
    DetectedGame game;
    game.gameId      = "monkey";
    game.engineId    = "scumm";
    game.description = "The Secret of Monkey Island";

    Common::Error err = metaEngine.createInstance(g_system, &engine, game, nullptr);
    if (err.getCode() != Common::kNoError || !engine) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ERROR: %s", err.getDesc().c_str());
        status(buf);
        for (;;) { of_input_poll(); usleep(100 * 1000); }
    }

    engine->setMetaEngine(&metaEngine);
    Common::FSNode dir(".");
    engine->initializePath(dir);

    status("engineInit()...");
    g_system->engineInit();

    g_system->getEventManager()->purgeKeyboardEvents();
    g_system->getEventManager()->purgeMouseEvents();

    /* Clear the overlay so the engine's own log output starts clean. */
    printf("\x1b[2J\x1b[1;1H");
    fflush(stdout);

    Common::Error result = engine->run();
    char buf[64];
    snprintf(buf, sizeof(buf), "Game ended: %d", result.getCode());
    status(buf);

    metaEngine.deleteInstance(engine, game, nullptr);

    for (;;) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_START)) break;
        usleep(16 * 1000);
    }

    g_system->destroy();
    return 0;
}
