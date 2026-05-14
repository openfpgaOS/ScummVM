/*
 * ScummVM for openfpgaOS -- Direct engine launcher.
 *
 * Slot map (matches the Duke3D SDK layout):
 *   slot 1   os.bin        (filename in data.json + instance)
 *   slot 2   scummvm.elf   (filename in data.json + instance)
 *   slot 3   <game>.iso    -- mounted by the kernel at /cd via
 *                            of_iso_mount(); engine reads use plain
 *                            fopen/opendir on "/cd/..."
 *   slot 4   bank.ofsf     -- soundfont (filename in data.json), auto-
 *                            bound by of_smp_bank.c at boot
 *   slot 5   <game>.cfg    -- volatile, key=value bootstrap
 *   slot 9   <game>.ini    -- nonvolatile R/W, ScummVM ConfigManager
 *   slot 10..19            -- 10 nonvolatile per-game saves
 *
 * Per-game filenames live in the instance JSON.  This code never
 * hard-codes them: opendir("/") + readdir() returns whatever the
 * launcher bound to each slot, so picking the .cfg / .iso / .ini at
 * runtime is just a matter of matching by extension.
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

extern "C" {
#include <of.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
}

static void status(const char *msg) {
    static int row = 1;
    printf("\x1b[%d;1H%s\n", row++, msg);
    fflush(stdout);
    if (row >= 29) row = 1;
}

[[noreturn]] static void halt(const char *msg) {
    status(msg);
    for (;;) { of_input_poll(); usleep(100 * 1000); }
}

/* ── game.cfg parsing ──────────────────────────────────────────────
 *
 * Plain key=value text file shipped per-instance and bound to slot 4.
 * Recognised keys:
 *
 *   gameid       SCUMM game id, e.g. "monkey", "monkey2", "tentacle"
 *   engineid     usually "scumm"
 *   description  human-readable name shown in the launcher status
 *   platform     "pc" / "amiga" / "mac" / ...
 *   language     ISO 639-1, e.g. "en"
 *   music        "openfpga" routes through the SDK MIDI driver
 */

struct GameConfig {
    char gameid     [32];
    char engineid   [32];
    char description[64];
    char platform   [16];
    char language   [8];
    char music      [16];
    char variant    [16];   /* gameVariantsTable variant; empty = first match */
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
        return (uint32)fread(buf, 1, cnt, _f);
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

/* Parse already-loaded key=value text into `cfg`. */
static bool parseGameConfig(const char *text, size_t len, GameConfig &cfg) {
    cfgDefaults(cfg);
    if (!text || !len) return false;

    char line[160];
    size_t i = 0;
    while (i < len) {
        size_t j = 0;
        while (i < len && text[i] != '\n' && j + 1 < sizeof(line))
            line[j++] = text[i++];
        line[j] = '\0';
        while (i < len && text[i] != '\n') ++i;
        if (i < len) ++i;       /* skip the '\n' */

        char *p = trim(line);
        if (!*p || p[0] == '#' || p[0] == ';' || p[0] == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);

        if      (!strcmp(k, "gameid"))      copyField(cfg.gameid,      sizeof(cfg.gameid),      v);
        else if (!strcmp(k, "engineid"))    copyField(cfg.engineid,    sizeof(cfg.engineid),    v);
        else if (!strcmp(k, "description")) copyField(cfg.description, sizeof(cfg.description), v);
        else if (!strcmp(k, "platform"))    copyField(cfg.platform,    sizeof(cfg.platform),    v);
        else if (!strcmp(k, "language"))    copyField(cfg.language,    sizeof(cfg.language),    v);
        else if (!strcmp(k, "music"))       copyField(cfg.music,       sizeof(cfg.music),       v);
        else if (!strcmp(k, "variant"))     copyField(cfg.variant,     sizeof(cfg.variant),     v);
    }
    return cfg.gameid[0] != '\0';
}

static bool loadGameConfig(const char *path, GameConfig &cfg) {
    cfgDefaults(cfg);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || p[0] == '#' || p[0] == ';' || p[0] == '[') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);

        if      (!strcmp(k, "gameid"))      copyField(cfg.gameid,      sizeof(cfg.gameid),      v);
        else if (!strcmp(k, "engineid"))    copyField(cfg.engineid,    sizeof(cfg.engineid),    v);
        else if (!strcmp(k, "description")) copyField(cfg.description, sizeof(cfg.description), v);
        else if (!strcmp(k, "platform"))    copyField(cfg.platform,    sizeof(cfg.platform),    v);
        else if (!strcmp(k, "language"))    copyField(cfg.language,    sizeof(cfg.language),    v);
        else if (!strcmp(k, "music"))       copyField(cfg.music,       sizeof(cfg.music),       v);
        else if (!strcmp(k, "variant"))     copyField(cfg.variant,     sizeof(cfg.variant),     v);
    }
    fclose(f);
    return cfg.gameid[0] != '\0';
}

extern "C" int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
    of_audio_init();
    of_input_set_deadzone(4000);
    printf("\x1b[2J");
    fflush(stdout);

    status("ScummVM openfpgaOS");

    /* opendir("/") returns every file the launcher bound to this
     * instance.  Find the .cfg, the game data file (.iso or .zip),
     * and the per-game .ini by extension, skipping the shared files
     * we ship in common/. */
    char cfgPath[64] = "", isoPath[64] = "", zipPath[64] = "", iniPath[64] = "";
    {
        DIR *d = opendir("/");
        if (!d) halt("ERROR: opendir(/) failed");
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *n = e->d_name;
            if (!strcasecmp(n, "os.bin") || !strcasecmp(n, "scummvm.elf") ||
                !strcasecmp(n, "bank.ofsf"))
                continue;
            size_t nl = strlen(n);
            if      (nl >= 4 && !strcasecmp(n + nl - 4, ".cfg") && !cfgPath[0])
                snprintf(cfgPath, sizeof(cfgPath), "%s", n);
            else if (nl >= 4 && !strcasecmp(n + nl - 4, ".iso") && !isoPath[0])
                snprintf(isoPath, sizeof(isoPath), "%s", n);
            else if (nl >= 4 && !strcasecmp(n + nl - 4, ".zip") && !zipPath[0])
                snprintf(zipPath, sizeof(zipPath), "%s", n);
            else if (nl >= 4 && !strcasecmp(n + nl - 4, ".ini") && !iniPath[0])
                snprintf(iniPath, sizeof(iniPath), "%s", n);
        }
        closedir(d);
    }

    if (!zipPath[0] && (!cfgPath[0] || !isoPath[0])) {
        /* Diagnostic: list every file the launcher actually exposed so we
         * can see what was bound and what's missing. */
        status("ERROR: ISO mode needs both .cfg and .iso bound.  Slots seen:");
        DIR *d = opendir("/");
        if (d) {
            struct dirent *e;
            char buf[80];
            while ((e = readdir(d)) != NULL) {
                snprintf(buf, sizeof(buf), "  slot %2ld: %s",
                         (long)e->d_ino - 1, e->d_name);
                status(buf);
            }
            closedir(d);
        }
        for (;;) { of_input_poll(); usleep(100 * 1000); }
    }

    /* Game config can come from one of two places:
     *
     *   1. A `game.cfg` member inside the .zip (preferred; ships with
     *      the archive, no second SD-card file to manage).
     *   2. The bound .cfg slot (ISO games or zips that opted out).
     *
     * For zips we open + parse the archive first so we can search it
     * for game.cfg before falling back to the slot-bound cfg path. */
    char buf[80];
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

    GameConfig cfg;
    bool cfgLoaded = false;

    if (gameZip) {
        /* Look for game.cfg inside the zip. */
        Common::SeekableReadStream *cs =
            gameZip->createReadStreamForMember(Common::Path("game.cfg"));
        if (cs) {
            uint32 n = (uint32)cs->size();
            char *txt = (char *)malloc(n + 1);
            if (txt) {
                cs->read(txt, n);
                txt[n] = '\0';
                cfgLoaded = parseGameConfig(txt, n, cfg);
                free(txt);
            }
            delete cs;
        }
    }
    if (!cfgLoaded && cfgPath[0])
        cfgLoaded = loadGameConfig(cfgPath, cfg);

    if (!cfgLoaded) {
        halt("ERROR: no usable game.cfg (in zip or slot 5)");
    }

    /* Stash the per-instance .ini for the OSystem config R/W hooks. */
    openfpga_set_config_path(iniPath[0] ? iniPath : nullptr);

    snprintf(buf, sizeof(buf), "Game: %s",
             cfg.description[0] ? cfg.description : cfg.gameid);
    status(buf);

    /* For ISO games, mount it now.  ZIP was already opened above so
     * we could read game.cfg out of it. */
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
    ConfMan.set("path",        "/cd",         gid);
    ConfMan.set("platform",    cfg.platform,  gid);
    ConfMan.set("language",    cfg.language,  gid);
    ConfMan.set("music_driver", cfg.music,    gid);
    ConfMan.setInt("music_volume", 192,       gid);
    ConfMan.setInt("sfx_volume",   192,       gid);
    ConfMan.setBool("subtitles",   true,      gid);

    /* Skip SCUMM's MD5 detection -- known game, and the file scan
     * starves the launcher UART pump on the Pocket. */
    ConfMan.setBool("openfpga_skip_detection", true, gid);

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

    Common::Error err = metaEngine.createInstance(g_system, &engine, game, nullptr);
    if (err.getCode() != Common::kNoError || !engine) {
        snprintf(buf, sizeof(buf), "ERROR: %s", err.getDesc().c_str());
        halt(buf);
    }

    engine->setMetaEngine(&metaEngine);
    Common::FSNode dir("/cd");
    engine->initializePath(dir);

    if (gameZip) {
        /* ZIP backend: take precedence over the (empty when zip-mode)
         * /cd directory.  Common::ZipArchive's central directory is
         * already a flat path map -- no flattening needed. */
        SearchMan.add("openfpga_game_zip", gameZip, /*priority=*/100);
    } else {
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

    /* Game is taking the screen now; hide the terminal so engine
     * warning prints / debug output don't bleed onto the framebuffer.
     * Re-assert 8-bit color: some kernel versions reset color mode
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
