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

/* Screen dimensions */
#define OPENFPGA_SCREEN_W  320
#define OPENFPGA_SCREEN_H  240

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

private:
    uint _screenW, _screenH;
    int _screenChangeID;
    bool _overlayVisible;

    /* Framebuffer */
    uint8_t _screenBuf[320 * 200];
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

    void drawCursor(uint8_t *dst) const;
};

/* Per-instance scummvm.ini filename, set by main() after it discovers
 * which slot the launcher bound (e.g. monkey1.ini).  createConfig{Read,
 * Write}Stream / getDefaultConfigFileName resolve through this. */
void openfpga_set_config_path(const char *path);

/* ── OSystem_OpenFPGA ────────────────────────────────────────────── */

class OSystem_OpenFPGA : public ModularMixerBackend, public ModularGraphicsBackend, Common::EventSource {
public:
    OSystem_OpenFPGA();
    virtual ~OSystem_OpenFPGA();

    void initBackend() override;

    bool pollEvent(Common::Event &event) override;

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
};

#endif /* OPENFPGA_OSYSTEM_H */
