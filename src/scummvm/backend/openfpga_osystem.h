/*
 * openfpga_osystem.h -- ScummVM OSystem backend for openfpgaOS
 *
 * Uses ModularGraphicsBackend + ModularMixerBackend from ScummVM.
 * Provides a real GraphicsManager that renders to the 320x240
 * framebuffer via openfpgaOS SDK syscalls.
 */

#ifndef OPENFPGA_OSYSTEM_H
#define OPENFPGA_OSYSTEM_H

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "common/events.h"
#include "backends/modular-backend.h"
#include "backends/graphics/graphics.h"
#include "graphics/surface.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <of.h>
#ifdef __cplusplus
}
#endif

/* Max supported surface -- a clamp, NOT a forced size.  Raised to 640x480 for
 * the SCI32 set, which renders CLUT8 at up to 640x480.  Smaller games drive
 * their own dimensions via initSize() and keep a 320x240 framebuffer (see
 * configureFramebufferMode()).  640x480x8 = 300 KiB, fits one 1 MiB OS FB slot. */
#define OPENFPGA_SCREEN_W  640
#define OPENFPGA_SCREEN_H  480

/* ── OpenFPGAGraphicsManager ─────────────────────────────────────── */

class OpenFPGAGraphicsManager : public GraphicsManager {
public:
    OpenFPGAGraphicsManager();
    virtual ~OpenFPGAGraphicsManager();

    bool hasFeature(OSystem::Feature f) const override { return false; }
    void setFeatureState(OSystem::Feature f, bool enable) override {}
    bool getFeatureState(OSystem::Feature f) const override { return false; }

    void initSize(uint width, uint height, const Graphics::PixelFormat *format = nullptr) override;
    int getScreenChangeID() const override { return _screenChangeID; }

    void beginGFXTransaction() override {}
    OSystem::TransactionError endGFXTransaction() override { return OSystem::kTransactionSuccess; }

    int16 getHeight() const override { return _screenH; }
    int16 getWidth() const override { return _screenW; }
    void setPalette(const byte *colors, uint start, uint num) override;
    void grabPalette(byte *colors, uint start, uint num) const override;
    void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override;
    Graphics::Surface *lockScreen() override;
    void unlockScreen() override;
    void fillScreen(uint32 col) override;
    void fillScreen(const Common::Rect &r, uint32 col) override;
    void updateScreen() override;
    void setShakePos(int shakeXOffset, int shakeYOffset) override {}
    void setFocusRectangle(const Common::Rect &rect) override {}
    void clearFocusRectangle() override {}

    /* Overlay */
    void showOverlay(bool inGUI) override { _overlayVisible = true; }
    void hideOverlay() override { _overlayVisible = false; }
    bool isOverlayVisible() const override { return _overlayVisible; }
    Graphics::PixelFormat getOverlayFormat() const override;
    void clearOverlay() override {}
    void grabOverlay(Graphics::Surface &surface) const override {}
    void copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) override {}
    int16 getOverlayHeight() const override { return _screenH; }
    int16 getOverlayWidth() const override { return _screenW; }

    /* Mouse cursor */
    bool showMouse(bool visible) override;
    void warpMouse(int x, int y) override;
    void setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
                        uint32 keycolor, bool dontScale = false,
                        const Graphics::PixelFormat *format = nullptr,
                        const byte *mask = nullptr) override;
    void setCursorPalette(const byte *colors, uint start, uint num) override {}

    /* Internal: get cursor position for event generation */
    int16 getMouseX() const { return _cursorX; }
    int16 getMouseY() const { return _cursorY; }
    void moveMouse(int dx, int dy);

    /* On-screen keypad legend: pollEvent pushes the controller keypad-mode
     * state here so updateScreen() can draw a visible legend over the game. */
    void setKeypadMode(bool on);

    /* Draw the ScummVM logo splash directly to the framebuffer (shown while
     * the engine loads, before it produces its first frame). */
    void showSplash();

private:
    uint _screenW, _screenH;
    int _screenChangeID;
    bool _overlayVisible;

    /* Framebuffer. 16-byte aligned so the SCI->_screenBuf (copyRectToScreen) and
     * _screenBuf->fb (updateScreen) copies stay co-aligned with the word-aligned
     * SCI _currentBuffer and hardware framebuffer -- musl memcpy then takes its
     * fast 16B-unrolled path instead of the shifted-word/byte path. */
    alignas(16) uint8_t _screenBuf[OPENFPGA_SCREEN_W * OPENFPGA_SCREEN_H];
    uint8_t _palette[256 * 3];
    bool _paletteDirty;
    bool _screenDirty;
    Graphics::Surface _frameSurface;

    /* Cursor */
    int16 _cursorX, _cursorY;
    int16 _cursorHotX, _cursorHotY;
    uint _cursorW, _cursorH;
    uint32 _cursorKeycolor;
    uint8_t _cursorData[64 * 64];
    bool _cursorVisible;

    /* GPU-triggered triple-buffer presentation. */
    bool _gpuReady;
    int _videoBufIdx;
    uint32 _videoFence;
    uint32 _gpuCleanMask;

    /* GPU stall guard.  When the platform menu (Pocket "Core Settings") is
     * open the display controller stops retiring GPU fences.  The per-frame
     * fence wait in clearFrameBorders() would then spin ~5 s and trap
     * (of_gpu_wait -> __builtin_trap -> ebreak), and any further command
     * emission would eventually hang forever on of_gpu's unbounded ring-space
     * spin.  When a bounded wait times out we latch _gpuStalled, stop emitting
     * GPU commands, and present nothing.  Each frame we re-probe the pending
     * fence (a plain MMIO read -- no ring writes); once it retires the menu
     * has closed and the display is live again, so rendering auto-resumes. */
    bool _gpuStalled;
    uint32 _gpuStallToken;

    /* True from showSplash() until the engine pushes its first real frame.
     * While set, initSize() must NOT blank the display so the logo stays up
     * through the engine's slow data load. */
    bool _splashActive;

    /* True while the controller numeric keypad is active (pushed from
     * OSystem_OpenFPGA::pollEvent); draws the on-screen keypad legend. */
    bool _keypadMode;

    void ensureGpuReady();
    uint8_t *acquireFrameBuffer();
    /* Bounded, non-fatal replacement for of_gpu_wait(): returns true when the
     * fence retired, false on timeout (caller latches the GPU as stalled). */
    bool waitGpuFenceBounded(uint32 token);
    void clearFrameBorders(uint8_t *fb, uint fbW, uint fbH, uint fbStride,
                           int xOff, int yOff, uint copyW, uint copyH);
    void presentFrame();
    void drawCursor(uint8_t *dst, uint fbW, uint fbH, uint fbStride,
                    int xOff, int yOff, uint clipW, uint clipH) const;
    void drawKeypadLegend(uint8_t *fb, uint fbW, uint fbH, uint fbStride) const;
};

/* Per-instance scummvm.ini filename, set by main() after it discovers
 * which slot the launcher bound (e.g. monkey1.ini).  createConfig{Read,
 * Write}Stream / getDefaultConfigFileName resolve through this. */
void openfpga_set_config_path(const char *path);

/* Pump audio + MIDI + timers.  Called from every OSystem entry that
 * may stall the main thread long enough to drain the 21 ms audio FIFO
 * (pollEvent, updateScreen, delayMillis). */
void openfpga_drive_audio_and_timers(void);
void openfpga_mixer_pump_only(void);

/* Draw the ScummVM logo splash to the framebuffer (called from main() while
 * the engine loads, before it produces its first frame). */
void openfpga_show_splash(void);

/* Forward decl for header consumers. */
namespace Common { class TimerManager; }
class MixerManager;

/* Stash the concrete manager pointers used by
 * openfpga_drive_audio_and_timers (set once during initBackend). */
void openfpga_set_pump_managers(Common::TimerManager *t, MixerManager *m);

/* ── OSystem_OpenFPGA ────────────────────────────────────────────── */

class OSystem_OpenFPGA : public ModularMixerBackend, public ModularGraphicsBackend, Common::EventSource {
public:
    OSystem_OpenFPGA();
    virtual ~OSystem_OpenFPGA();

    void initBackend() override;

    bool pollEvent(Common::Event &event) override;

    /* Intercept kFeatureVirtualKeyboard (the engine's MI2 room-108
     * copy-protection signal); all other features forward to the
     * graphics backend. */
    void setFeatureState(Feature f, bool enable) override;
    bool getFeatureState(Feature f) override;

    Common::MutexInternal *createMutex() override;
    uint32 getMillis(bool skipRecord = false) override;
    void delayMillis(uint msecs) override;
    void getTimeAndDate(TimeDate &td, bool skipRecord = false) const override;

    void quit() override;

    void logMessage(LogMessageType::Type type, const char *message) override;

    void addSysArchivesToSearchSet(Common::SearchSet &s, int priority) override;

    /* Override filesystem methods — no writable filesystem on this platform */
    Common::SeekableReadStream *createConfigReadStream() override;
    Common::WriteStream *createConfigWriteStream() override;
    Common::Path getDefaultConfigFileName() override;
    Common::Path getDefaultLogFileName() override;
    void messageBox(LogMessageType::Type type, const char *message) override;
    void fatalError() override;

private:
    uint32 _startTime;
    OpenFPGAGraphicsManager *_ofGfx;  /* Direct access to our graphics manager */

    /* Input state */
    bool _mouseButtonL, _mouseButtonR;
    int  _autoDismissCounter;
    bool _ignoreInitialButtons;
    bool _dockKbArmed;  /* dock keyboard trusted only after a clean (no-key)
                         * HID frame -- filters a stuck/phantom boot key */
    bool _keypadMode;   /* SELECT toggles a controller numeric keypad so a
                         * keyboard-less user can type codes (e.g. MI2's
                         * copy-protection numbers). */

    /* MI2 copy-protection auto-bypass.  The engine raises
     * kFeatureVirtualKeyboard while its room-108 code screen is up; our
     * build accepts any digits there, so we auto-type until it clears.
     * Capped (_copyProtectKeys) so a screen that does not auto-accept
     * hands control back instead of spinning forever. */
    bool _copyProtectActive;
    int  _copyProtectKeys;

    /* Live volume (0..255), SELECT acts as a hold-modifier:
     *   SELECT + Up/Down    -> master output volume
     *   SELECT + Left/Right -> music volume (ScummVM kMusicSoundType for CD
     *                          music + the of_mixer MUSIC group for MIDI synth)
     * A SELECT *tap* (released with no volume change) toggles keypad mode. */
    int  _masterVolume;
    int  _musicVolume;
    bool _selectHeld;
    bool _selectConsumed;

    uint32 _lastMouseTick;
    int32 _mouseAccumX, _mouseAccumY;
    Common::Queue<Common::Event> _eventQueue;

    void queueKey(Common::KeyCode keycode, uint16 ascii, byte flags = 0);
    bool popQueuedEvent(Common::Event &event);
};

#endif /* OPENFPGA_OSYSTEM_H */
