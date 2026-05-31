/*
 * ScummVM for openfpgaOS -- Direct engine launcher.
 *
 * Slot map:
 *   slot 1   os.bin        (filename in data.json + instance)
 *   slot 2   <game>_os.ini -- openfpgaOS per-instance launch config
 *   slot 3   scummvm.elf   (filename in data.json + instance)
 *   slot 4   <game>.iso/.zip/.cue
 *                            ISO: mounted by the kernel at /cd via
 *                            of_iso_mount(); engine reads use plain
 *                            fopen/opendir on "/cd/..."
 *                            CUE: mounted by OpenFPGA::CueArchive
 *                            ZIP: opened as a ScummVM archive
 *   slot 5   bank.ofsf     -- soundfont (filename in data.json), auto-
 *                            bound by of_smp_bank.c at boot
 *   slot 7   <game>.bin    -- raw MODE1/2352 image used by a .cue
 *   slot 9   <game>.ini    -- nonvolatile R/W, ScummVM ConfigManager
 *   slot 10..18            -- 9 nonvolatile per-game saves
 *
 * Per-game filenames live in the instance JSON.  opendir("/") +
 * readdir() returns whatever the launcher bound to each slot, with
 * d_ino carrying the APF data-slot id + 1.  Use that slot id first so
 * the OS config .ini (slot 2) and ScummVM settings .ini (slot 9) are
 * never confused just because both have the same extension.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "common/str.h"
#include "common/archive.h"
#include "common/fs.h"
#include "common/compression/unzip.h"
#include "common/stream.h"
#include "common/config-manager.h"
#include "common/events.h"
#include "common/textconsole.h"
#include "base/version.h"
#include "base/plugins.h"
#include "base/commandLine.h"
#include "engines/engine.h"
#include "engines/metaengine.h"
#include "backend/openfpga_osystem.h"
#include "backend/openfpga_midi.h"
#include "backend/openfpga_cue_archive.h"
#include "backend/openfpga_audiocd.h"
#include "backend/openfpga_save.h"

extern "C" {
#include <of.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
}

static int g_statusRow = 1;

static void status(const char *msg) {
    printf("\x1b[%d;1H%s\n", g_statusRow++, msg);
    fflush(stdout);
    if (g_statusRow >= 29) g_statusRow = 1;
}

static void showFailureTerminal() {
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
}

[[noreturn]] static void halt(const char *msg) {
    showFailureTerminal();
    status(msg);
    for (;;) { of_input_poll(); usleep(100 * 1000); }
}

/* ── per-instance game config ──────────────────────────────────────
 *
 * Instances store these keys in slot 2's OS .ini under [scummvm],
 * read through of_config_get().  Recognised keys:
 *
 *   gameid       SCUMM game id, e.g. "monkey", "monkey2", "tentacle"
 *   engineid     usually "scumm"
 *   description  human-readable name shown in the launcher status
 *   platform     "pc" / "amiga" / "mac" / ...
 *   language     ISO 639-1, e.g. "en"
 *   music        "openfpga" routes through the SDK MIDI driver
 *   data_type    "" or "zip" for normal zip members, "cue" for a raw
 *                BIN/CUE disc image carried inside the zip
 *   data_file    launcher-visible data filename (.zip, .iso, or .cue)
 *   cue_file     optional .cue member/path when data_file is an outer zip
 */

struct GameConfig {
    char gameid     [32];
    char engineid   [32];
    char description[96];
    char platform   [16];
    char language   [8];
    char music      [16];
    char variant    [16];   /* gameVariantsTable variant; empty = first match */
    char data_type  [8];    /* ""/"zip" (normal zip), or "cue" */
    char data_file  [160];  /* launcher-visible zip/iso/cue filename */
    char cue_file   [160];  /* .cue member/path for zipped raw-disc images */
    char subdir     [64];   /* Per-game subdirectory on a multi-game
                             * compilation disc (e.g. "PQ1" inside the
                             * Police Quest collection ISO).  Empty
                             * means root. */
    int  cd_track_offset;   /* Added to every CD track # before lookup.
                             * Compilation discs (e.g. Madness) reorder
                             * tracks vs the standalone-CD baked into
                             * the game data; this offset re-aligns. */
};

static void cfgDefaults(GameConfig &c) {
    memset(&c, 0, sizeof(c));
    strncpy(c.engineid,  "scumm",    sizeof(c.engineid)  - 1);
    strncpy(c.platform,  "pc",       sizeof(c.platform)  - 1);
    strncpy(c.language,  "en",       sizeof(c.language)  - 1);
    strncpy(c.music,     "openfpga", sizeof(c.music)     - 1);
}

/* Stdio-backed SeekableReadStream used to hand a slot-bound game
 * archive (currently `<game>.zip`) to Common::makeZipArchive without
 * going through Common::FSNode -- ZIP archives live at the launcher
 * root, not under the /cd mount, so the regular FSNode chain doesn't
 * resolve them. */
namespace {
constexpr uint32 kSlotReadPumpChunk = 8192;

class SlotFileStream : public Common::SeekableReadStream {
public:
    explicit SlotFileStream(FILE *f) : _f(f), _size(0) {
        if (_f && fseeko(_f, 0, SEEK_END) == 0) {
            _size = (int64)ftello(_f);
            fseeko(_f, 0, SEEK_SET);
        }
    }
    ~SlotFileStream() override { if (_f) fclose(_f); }

    bool eos() const override        { return feof(_f) != 0; }
    bool err() const override        { return ferror(_f) != 0; }
    void clearErr() override         { clearerr(_f); }
    int64 pos()  const override      { return (int64)ftello(_f); }
    int64 size() const override      { return _size; }
    uint32 read(void *buf, uint32 cnt) override {
        byte *out = (byte *)buf;
        uint32 total = 0;

        while (cnt) {
            uint32 chunk = cnt < kSlotReadPumpChunk ? cnt : kSlotReadPumpChunk;
            uint32 got = (uint32)fread(out + total, 1, chunk, _f);
            total += got;
            cnt -= got;

            if (got != chunk)
                break;

            openfpga_mixer_pump_only();
        }

        return total;
    }
    bool seek(int64 offset, int whence = SEEK_SET) override {
        return fseeko(_f, (off_t)offset, whence) == 0;
    }
private:
    FILE *_f;
    int64 _size;
};
} // namespace

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = '\0';
    return s;
}

static void copyField(char *dst, size_t cap, const char *src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static bool hasExtensionI(const char *name, const char *ext) {
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    return nl >= el && !strcasecmp(name + nl - el, ext);
}

static bool isDotDir(const char *name) {
    return name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static bool isDataImage(const char *name) {
    return hasExtensionI(name, ".iso") ||
           hasExtensionI(name, ".zip") ||
           hasExtensionI(name, ".cue");
}

static bool isSoundBank(const char *name) {
    return hasExtensionI(name, ".ofsf") || hasExtensionI(name, ".dat");
}

static void rememberPath(char *dst, size_t cap, const char *name) {
    if (!dst[0])
        snprintf(dst, cap, "%s", name);
}

static void rememberDataPath(const char *name, char *isoPath, size_t isoCap,
                             char *zipPath, size_t zipCap,
                             char *cuePath, size_t cueCap) {
    if (hasExtensionI(name, ".iso"))
        rememberPath(isoPath, isoCap, name);
    else if (hasExtensionI(name, ".zip"))
        rememberPath(zipPath, zipCap, name);
    else if (hasExtensionI(name, ".cue"))
        rememberPath(cuePath, cueCap, name);
}

static void readInstanceArg(int argc, char **argv, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        copyField(out, cap, argv[1]);
        return;
    }

    if (of_config_get("os", "ARGS", out, (uint32_t)cap) == 0) {
        char *trimmed = trim(out);
        if (trimmed != out)
            memmove(out, trimmed, strlen(trimmed) + 1);
    }
}

static bool getOSConfigString(const char *section, const char *key,
                              char *dst, size_t cap) {
    if (!dst || cap == 0)
        return false;

    char buf[256];
    if (of_config_get(section, key, buf, sizeof(buf)) != 0)
        return false;

    char *value = trim(buf);
    copyField(dst, cap, value);
    return value[0] != '\0';
}

static bool loadGameConfigFromOS(GameConfig &cfg) {
    cfgDefaults(cfg);

    getOSConfigString("scummvm", "gameid",      cfg.gameid,      sizeof(cfg.gameid));
    getOSConfigString("scummvm", "engineid",    cfg.engineid,    sizeof(cfg.engineid));
    getOSConfigString("scummvm", "description", cfg.description, sizeof(cfg.description));
    getOSConfigString("scummvm", "platform",    cfg.platform,    sizeof(cfg.platform));
    getOSConfigString("scummvm", "language",    cfg.language,    sizeof(cfg.language));
    getOSConfigString("scummvm", "music",       cfg.music,       sizeof(cfg.music));
    getOSConfigString("scummvm", "variant",     cfg.variant,     sizeof(cfg.variant));
    getOSConfigString("scummvm", "data_type",   cfg.data_type,   sizeof(cfg.data_type));
    getOSConfigString("scummvm", "data_file",   cfg.data_file,   sizeof(cfg.data_file));
    getOSConfigString("scummvm", "cue_file",    cfg.cue_file,    sizeof(cfg.cue_file));
    getOSConfigString("scummvm", "subdir",      cfg.subdir,      sizeof(cfg.subdir));
    cfg.cd_track_offset = of_config_get_int("scummvm", "cd_track_offset", 0);

    return cfg.gameid[0] != '\0';
}

extern "C" int main(int argc, char **argv) {
    of_video_init();
    of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
    of_video_palette(0, 0);
    of_video_clear(0);
    of_video_flip();
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    of_audio_init();
    of_input_set_deadzone(4000);
    printf("\x1b[2J");
    fflush(stdout);

    status("ScummVM openfpgaOS");
    status("Build: text-console-debugger memory-pools gpu-async-cdda 2026-05-21");

    char instanceArg[96];
    readInstanceArg(argc, argv, instanceArg, sizeof(instanceArg));
    if (instanceArg[0]) {
        char argMsg[140];
        snprintf(argMsg, sizeof(argMsg), "Instance: %s", instanceArg);
        status(argMsg);
    }

    /* opendir("/") returns every file the launcher bound to this
     * instance.  Prefer slot ids from d_ino so the two .ini slots are
     * unambiguous; keep a small extension fallback for older manifests
     * and PC-side shims that may not expose the new slot ids. */
    char osConfigPath[256] = "", isoPath[256] = "", zipPath[256] = "",
         iniPath[256] = "", cuePath[256] = "";
    char savePaths[OPENFPGA_MAX_SAVES][256];
    memset(savePaths, 0, sizeof(savePaths));
    {
        DIR *d = opendir("/");
        if (!d) halt("ERROR: opendir(/) failed");
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            if (isDotDir(n))
                continue;

            const long slotId = (long)e->d_ino - 1;
            bool handled = false;

            switch (slotId) {
            case 1:
                handled = !strcasecmp(n, "os.bin");
                break;
            case 2:
                if (hasExtensionI(n, ".ini")) {
                    rememberPath(osConfigPath, sizeof(osConfigPath), n);
                    handled = true;
                } else if (hasExtensionI(n, ".elf")) {
                    handled = true; /* legacy app slot */
                }
                break;
            case 3:
                if (hasExtensionI(n, ".elf")) {
                    handled = true;
                } else if (isDataImage(n)) {
                    rememberDataPath(n, isoPath, sizeof(isoPath),
                                     zipPath, sizeof(zipPath),
                                     cuePath, sizeof(cuePath));
                    handled = true; /* legacy game-data slot */
                }
                break;
            case 4:
                if (isDataImage(n)) {
                    rememberDataPath(n, isoPath, sizeof(isoPath),
                                     zipPath, sizeof(zipPath),
                                     cuePath, sizeof(cuePath));
                    handled = true;
                } else if (isSoundBank(n)) {
                    handled = true; /* legacy bank slot */
                }
                break;
            case 5:
                if (isSoundBank(n))
                    handled = true;
                break;
            case 7:
                if (hasExtensionI(n, ".bin"))
                    handled = true; /* raw .cue image, opened by cue filename */
                break;
            case 9:
                if (hasExtensionI(n, ".ini")) {
                    rememberPath(iniPath, sizeof(iniPath), n);
                    handled = true;
                }
                break;
            default:
                break;
            }

            if (slotId >= 10 && slotId < 10 + OPENFPGA_MAX_SAVES &&
                hasExtensionI(n, ".sav")) {
                snprintf(savePaths[slotId - 10], sizeof(savePaths[0]), "%s", n);
                handled = true;
            }

            if (handled)
                continue;

            if (!strcasecmp(n, "os.bin") || !strcasecmp(n, "scummvm.elf") ||
                !strcasecmp(n, "bank.ofsf") || hasExtensionI(n, "_os.ini"))
                continue;

            if (isDataImage(n))
                rememberDataPath(n, isoPath, sizeof(isoPath),
                                 zipPath, sizeof(zipPath),
                                 cuePath, sizeof(cuePath));
            else if (hasExtensionI(n, ".ini") && !iniPath[0])
                snprintf(iniPath, sizeof(iniPath), "%s", n);
            else if (hasExtensionI(n, ".sav")) {
                if (slotId >= 10 && slotId < 10 + OPENFPGA_MAX_SAVES)
                    snprintf(savePaths[slotId - 10], sizeof(savePaths[0]), "%s", n);
            }
        }
        closedir(d);
    }

    if (osConfigPath[0]) {
        char osMsg[300];
        snprintf(osMsg, sizeof(osMsg), "OS config: %s", osConfigPath);
        status(osMsg);
    }

    GameConfig cfg;
    bool cfgLoaded = loadGameConfigFromOS(cfg);

    if (!zipPath[0] && cfgLoaded && cfg.data_file[0]) {
        if (hasExtensionI(cfg.data_file, ".zip"))
            snprintf(zipPath, sizeof(zipPath), "%s", cfg.data_file);
        else if (hasExtensionI(cfg.data_file, ".iso"))
            snprintf(isoPath, sizeof(isoPath), "%s", cfg.data_file);
        else if (!cuePath[0] && hasExtensionI(cfg.data_file, ".cue"))
            snprintf(cuePath, sizeof(cuePath), "%s", cfg.data_file);
    }

    if (!cuePath[0] && cfgLoaded && cfg.cue_file[0] &&
        hasExtensionI(cfg.cue_file, ".cue") && !zipPath[0])
        snprintf(cuePath, sizeof(cuePath), "%s", cfg.cue_file);

    if (!zipPath[0] && !cuePath[0] && (!cfgLoaded || !isoPath[0])) {
        /* Diagnostic: list every file the launcher actually exposed so we
         * can see what was bound and what's missing. */
        showFailureTerminal();
        status("ERROR: instance is missing its game data file.");
        status("  expected one of:");
        status("    monkeyN.zip                   (zipped game data)");
        status("    monkeyN.iso  + slot-2 config  (cooked ISO)");
        status("    monkeyN.cue  + slot-2 config  (raw .cue/.bin)");
        status("Slots seen by the launcher:");
        DIR *d = opendir("/");
        if (d) {
            struct dirent *e;
            char buf[320];
            while ((e = readdir(d)) != NULL) {
                snprintf(buf, sizeof(buf), "  slot %2ld: %s",
                         (long)e->d_ino - 1, e->d_name);
                status(buf);
            }
            closedir(d);
        }
        for (;;) { of_input_poll(); usleep(100 * 1000); }
    }

    char buf[320];
    Common::Archive *gameZip = nullptr;
    if (zipPath[0]) {
        snprintf(buf, sizeof(buf), "Opening %s ...", zipPath);
        status(buf);
        FILE *zf = fopen(zipPath, "rb");
        if (!zf) halt("ERROR: cannot fopen .zip");
        gameZip = Common::makeZipArchive(new SlotFileStream(zf),
                                         /*flattenTree=*/true);
        if (!gameZip) halt("ERROR: invalid .zip (no central directory)");
    }

    if (!cfgLoaded) {
        halt("ERROR: no usable ScummVM config ([scummvm])");
    }

    /* Stash the per-instance .ini for the OSystem config R/W hooks. */
    openfpga_set_config_path(iniPath[0] ? iniPath : nullptr);
    for (int i = 0; i < OPENFPGA_MAX_SAVES; ++i)
        openfpga_set_save_path(i, savePaths[i][0] ? savePaths[i] : nullptr);

    snprintf(buf, sizeof(buf), "Game: %s",
             cfg.description[0] ? cfg.description : cfg.gameid);
    status(buf);

    /* For ISO games, mount it now. */
    if (!gameZip && isoPath[0]) {
        snprintf(buf, sizeof(buf), "Mounting %s -> /cd ...", isoPath);
        status(buf);
        if (of_iso_mount(isoPath, "/cd") < 0)
            halt("ERROR: of_iso_mount failed");
    }

    status("Init backend...");
    g_system = new OSystem_OpenFPGA();
    g_system->initBackend();

    status("Config...");
    Base::registerDefaults();
    g_system->registerDefaultSettings(Common::ConfigManager::kApplicationDomain);

    const char *gid = cfg.gameid;
    ConfMan.addGameDomain(gid);
    ConfMan.set("gameid",      gid,           gid);
    ConfMan.set("engineid",    cfg.engineid,  gid);
    ConfMan.set("description", cfg.description[0] ? cfg.description : gid, gid);
    /* Per-game path: defaults to the /cd ISO mount root.  For multi-
     * game compilation discs (e.g. Police Quest collection, Space
     * Quest collection) [scummvm] may set `subdir=PQ1` to point at
     * the game's subdirectory on the disc. */
    {
        char gamePath[160] = "/cd";
        if (cfg.subdir[0])
            snprintf(gamePath, sizeof(gamePath), "/cd/%s", cfg.subdir);
        ConfMan.set("path", gamePath, gid);
    }
    ConfMan.set("platform",    cfg.platform,  gid);
    ConfMan.set("language",    cfg.language,  gid);
	ConfMan.set("music_driver", cfg.music,    gid);
	ConfMan.setInt("music_volume", 192,       gid);
	ConfMan.setInt("sfx_volume",   192,       gid);
	ConfMan.setInt("speech_volume", 224,      gid);
	ConfMan.setBool("speech_mute", false,     gid);
	ConfMan.setBool("subtitles",   true,      gid);

    /* Skip SCUMM's MD5 detection -- known game, and the file scan
     * starves the launcher UART pump on the Pocket. */
    ConfMan.setBool("openfpga_skip_detection", true, gid);

    /* Enable multi-MIDI so iMUSE creates BOTH the AdLib (OPL emu) and
     * our openfpga GM drivers.  Without this, picking our GM driver
     * makes iMUSE skip AdLib music in the SCUMM PAK (the ADL/SBL
     * sub-blocks).  With multi-MIDI, AdLib music renders through OPL
     * emu into our mixer, and MT-32-style cues go through our driver. */
    ConfMan.setBool("multi_midi", false, gid);  /* trap-isolation: AdLib OPL off; only our GM driver */

    /* Optional per-game variant override (e.g. "Floppy", "CD", "SE").
     * Leave empty and the fast path picks the first matching entry
     * in gameVariantsTable for `gameid`. */
    if (cfg.variant[0])
        ConfMan.set("extra", cfg.variant, gid);

    /* Force classic MIDI for SE variants.  MI1/MI2 SE's remastered
     * audio is xWMA inside the XACT wave banks, and ScummVM's WMA
     * codec is a stub for the SE path (HeaderlessWMAStream is TODO
     * in upstream).  With use_remastered_audio=false the engine
     * plays the AdLib/MT-32 MIDI scores from the classic LFL
     * resources via our openfpga MIDI driver -- same audio as the
     * original floppy/CD release. */
    ConfMan.setBool("use_remastered_audio", false, gid);

    /* Skip copy-protection screens.  MI2's Dial-A-Pirate, Indy 4's
     * passport-Q&A, and DOTT's "type word from manual" are all gated
     * on this flag; off-by-default for handheld UX (the manual is
     * usually not at hand, and saves include the post-protection
     * state anyway).  Users who want the original challenge can add
     * `copy_protection=true` to [scummvm] to re-enable. */
    ConfMan.setBool("copy_protection", false, gid);

    /* The themed ScummVM global menu is not stable on this backend yet.
     * Keep SCUMM on its original in-engine menus so F5/Start does not
     * enter the GUI theme evaluator path. */
    if (!strcmp(cfg.engineid, "scumm"))
        ConfMan.setBool("original_gui", true, gid);

    ConfMan.setActiveDomain(gid);

    status("Plugins...");
    PluginManager::instance().init();
    PluginManager::instance().addPluginProvider(createOpenFPGAMidiPluginProvider());
    PluginManager::instance().loadAllPlugins();
    PluginManager::instance().loadDetectionPlugin();

    status("Finding engine...");
    const Plugin *enginePlugin = nullptr;
    const PluginList &plugins = EngineMan.getPlugins(PLUGIN_TYPE_ENGINE);
    for (PluginList::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        Common::String n = (*it)->getName();
        if (n.equalsIgnoreCase(cfg.engineid) || n.contains(cfg.engineid)) {
            enginePlugin = *it;
            break;
        }
    }
    if (!enginePlugin) halt("ERROR: engine plugin not found");

    status("Creating engine...");
    MetaEngine &metaEngine = enginePlugin->get<MetaEngine>();
    metaEngine.registerDefaultSettings(gid);

    Engine *engine = nullptr;
    DetectedGame game;
    game.gameId      = gid;
    game.engineId    = cfg.engineid;
    game.description = cfg.description[0] ? cfg.description : gid;

    /* Apply [scummvm]'s CD track offset to our AudioCDManager.  The
     * manager was instantiated in OSystem::initBackend() before we
     * read config, so we patch it now.  Default 0 (no remap) matches
     * single-disc rips; compilation discs like MI Madness need +1. */
    {
        OpenFPGAAudioCDManager *mgr =
            (OpenFPGAAudioCDManager *)g_system->getAudioCDManager();
        if (mgr) {
            if (cfg.cd_track_offset != 0) {
                mgr->setTrackOffset(cfg.cd_track_offset);
                status("AudioCD track offset applied");
            }
            /* Retained for older AudioCD managers; the current CDDA
             * path reads through SearchMan so compressed zip members
             * and extracted folders follow the same code. */
            if (zipPath[0])
                mgr->setZipPath(zipPath);
            const char *configuredCue = nullptr;
            if (cuePath[0]) {
                configuredCue = cuePath;
            } else if (!strcmp(cfg.data_type, "cue")) {
                if (cfg.cue_file[0])
                    configuredCue = cfg.cue_file;
                else if (cfg.data_file[0] && hasExtensionI(cfg.data_file, ".cue"))
                    configuredCue = cfg.data_file;
            }
            if (configuredCue)
                mgr->setCuePath(configuredCue);
        }
    }

    /* Register the game zip in SearchMan BEFORE createInstance so the
     * metaengine's filename-pattern probe (Common::File::exists) can
     * see zip entries.  For ISO mode there's nothing to register yet --
     * engine->initializePath(dir) below adds /cd.  For .cue mode, we
     * mount the Track 01 MODE1/2352 .bin via OpenFPGA::CueArchive,
     * which exposes the ISO 9660 contents as archive members. */
    if (gameZip) {
        SearchMan.add("openfpga_game_zip", gameZip, /*priority=*/100);

        /* If [scummvm] declares cue-style data, mount the cue from
         * inside the zip.  Common::File::open inside CueArchive::create
         * routes through SearchMan, so the zip-resident .cue + .bin
         * files are reachable now that gameZip is registered.  CueArchive's
         * priority needs to BEAT the raw zip so engine lookups of files
         * inside the ISO 9660 tree (MONKEY.000, etc.) hit the cooked
         * stream rather than the literal zip member of the same name
         * (there is none -- only the cue + .bin files in the zip). */
        if (!strcmp(cfg.data_type, "cue")) {
            Common::String cueToMount;
            if (cfg.cue_file[0])
                cueToMount = cfg.cue_file;
            else if (cfg.data_file[0] && hasExtensionI(cfg.data_file, ".cue"))
                cueToMount = cfg.data_file;
            OpenFPGA::CueArchive *cueArchive =
                OpenFPGA::CueArchive::create(cueToMount);
            if (!cueArchive)
                halt("ERROR: failed to mount cue/.bin disc image");
            SearchMan.add("openfpga_cue_archive", cueArchive, /*priority=*/200);
        }
    } else if (cuePath[0]) {
        /* Standalone .cue/.bin bound directly (no zip wrapper). */
        OpenFPGA::CueArchive *cueArchive = OpenFPGA::CueArchive::create(cuePath);
        if (!cueArchive)
            halt("ERROR: failed to mount .cue/.bin disc image");
        SearchMan.add("openfpga_cue_archive", cueArchive, /*priority=*/100);
    }

    Common::Error err = metaEngine.createInstance(g_system, &engine, game, nullptr);
    if (err.getCode() != Common::kNoError || !engine) {
        snprintf(buf, sizeof(buf), "ERROR: %s", err.getDesc().c_str());
        halt(buf);
    }

    engine->setMetaEngine(&metaEngine);
    char gamePath[160] = "/cd";
    if (cfg.subdir[0])
        snprintf(gamePath, sizeof(gamePath), "/cd/%s", cfg.subdir);
    Common::FSNode dir(gamePath);
    const bool archiveBackedGame = (gameZip != nullptr) || cuePath[0];
    if (!archiveBackedGame)
        engine->initializePath(dir);

    if (gameZip) {
        /* Zip already in SearchMan above. */
    } else if (!archiveBackedGame) {
        /* ISO backend: Engine::initializePath added /cd with depth=4
         * but flat=false, so files in subdirectories of the ISO end
         * up cached with their subdir prefix and the engine's bare-
         * name lookup misses them.  Add a second flat pass so
         * subdirectory contents collapse into the root namespace.
         * ignoreClashes=true silences the "name clash" warning when
         * an ISO has, e.g., ATLANTIS.EXE both at root and inside
         * ATLANTIS/. */
        SearchMan.add("openfpga_iso_flat",
                      new Common::FSDirectory(dir, /*depth=*/4, /*flat=*/true,
                                              /*ignoreClashes=*/true),
                      /*priority=*/0);
    }

    status("engineInit()...");
    g_system->engineInit();

    g_system->getEventManager()->purgeKeyboardEvents();
    g_system->getEventManager()->purgeMouseEvents();

    printf("\x1b[2J\x1b[1;1H");
    fflush(stdout);

    /* Startup has kept the terminal hidden since video init.  Re-assert
     * framebuffer-only mode before entering the engine, and re-assert
     * 8-bit color: some kernel versions reset color mode
     * when display mode is switched, and a stale 4-bit color mode
     * gives a 16-color "EGA" look on the FB.  fatalError() flips
     * back to TERMINAL so a crash is still visible. */
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    of_video_set_color_mode(OF_VIDEO_MODE_8BIT);

    Common::Error result = engine->run();

    /* Engine returned -- show terminal again for the "Game ended"
     * status and the press-START-to-exit prompt. */
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
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
