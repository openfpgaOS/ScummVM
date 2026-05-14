/*
 * openfpga_osystem.cpp -- ScummVM OSystem backend for openfpgaOS
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "openfpga_osystem.h"

#include "backends/mutex/null/null-mutex.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "common/memstream.h"
#include "common/stream.h"
#include "openfpga_fs.h"
#include "openfpga_mixer.h"
#include "openfpga_save.h"

extern "C" {
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
}

/* ═══════════════════════════════════════════════════════════════════
 * OpenFPGAGraphicsManager
 * ═══════════════════════════════════════════════════════════════════ */

OpenFPGAGraphicsManager::OpenFPGAGraphicsManager()
    : _screenW(320), _screenH(200), _screenChangeID(0),
      _overlayVisible(false), _paletteDirty(false), _screenDirty(false),
      _cursorX(160), _cursorY(100), _cursorHotX(0), _cursorHotY(0),
      _cursorW(0), _cursorH(0), _cursorKeycolor(0), _cursorVisible(false) {
    memset(_screenBuf, 0, sizeof(_screenBuf));
    memset(_palette, 0, sizeof(_palette));
    memset(_cursorData, 0, sizeof(_cursorData));
    memset(&_frameSurface, 0, sizeof(_frameSurface));
}

OpenFPGAGraphicsManager::~OpenFPGAGraphicsManager() {
}

void OpenFPGAGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
    _screenW = width;
    _screenH = height;
    memset(_screenBuf, 0, sizeof(_screenBuf));
    _screenChangeID++;
    _screenDirty = true;

    /* Game graphics only -- main() will flip to FRAMEBUFFER right
     * before engine->run().  Configure the color mode and clear here. */
    of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
    of_video_clear(0);
    of_video_flip();
}

void OpenFPGAGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
    memcpy(_palette + start * 3, colors, num * 3);
    _paletteDirty = true;
}

void OpenFPGAGraphicsManager::grabPalette(byte *colors, uint start, uint num) const {
    memcpy(colors, _palette + start * 3, num * 3);
}

void OpenFPGAGraphicsManager::copyRectToScreen(const void *buf, int pitch,
                                                int x, int y, int w, int h) {
    const byte *src = (const byte *)buf;

    /* Clip */
    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= y * pitch; h += y; y = 0; }
    if (x + w > (int)_screenW) w = _screenW - x;
    if (y + h > (int)_screenH) h = _screenH - y;
    if (w <= 0 || h <= 0) return;

    byte *dst = _screenBuf + y * _screenW + x;
    for (int row = 0; row < h; row++) {
        memcpy(dst, src, w);
        dst += _screenW;
        src += pitch;
    }
    _screenDirty = true;
}

Graphics::Surface *OpenFPGAGraphicsManager::lockScreen() {
    _frameSurface.init(_screenW, _screenH, _screenW,
                       _screenBuf, Graphics::PixelFormat::createFormatCLUT8());
    return &_frameSurface;
}

void OpenFPGAGraphicsManager::unlockScreen() {
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::fillScreen(uint32 col) {
    memset(_screenBuf, col & 0xFF, _screenW * _screenH);
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::fillScreen(const Common::Rect &r, uint32 col) {
    for (int y = r.top; y < r.bottom && y < (int)_screenH; y++) {
        for (int x = r.left; x < r.right && x < (int)_screenW; x++) {
            _screenBuf[y * _screenW + x] = col & 0xFF;
        }
    }
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::updateScreen() {
    if (_paletteDirty) {
        uint32_t pal32[256];
        for (int i = 0; i < 256; i++) {
            pal32[i] = ((uint32_t)_palette[i*3]     << 16) |
                       ((uint32_t)_palette[i*3 + 1] <<  8) |
                        (uint32_t)_palette[i*3 + 2];
        }
        of_video_palette_bulk(pal32, 256);
        _paletteDirty = false;
    }

    /* Always blit -- engines expect updateScreen() to push every frame
     * regardless of dirty tracking. */
    of_video_blit_letterbox(_screenBuf, _screenW, _screenH);

    if (_cursorVisible)
        drawCursor(of_video_surface());

    of_video_flush();
    of_video_flip();
    _screenDirty = false;
}

Graphics::PixelFormat OpenFPGAGraphicsManager::getOverlayFormat() const {
    return Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0);
}

bool OpenFPGAGraphicsManager::showMouse(bool visible) {
    bool prev = _cursorVisible;
    _cursorVisible = visible;
    _screenDirty = true;
    return prev;
}

void OpenFPGAGraphicsManager::warpMouse(int x, int y) {
    _cursorX = x;
    _cursorY = y;
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::setMouseCursor(const void *buf, uint w, uint h,
                                              int hotspotX, int hotspotY,
                                              uint32 keycolor, bool dontScale,
                                              const Graphics::PixelFormat *format,
                                              const byte *mask) {
    _cursorW = (w > 64) ? 64 : w;
    _cursorH = (h > 64) ? 64 : h;
    _cursorHotX = hotspotX;
    _cursorHotY = hotspotY;
    _cursorKeycolor = keycolor;

    const byte *src = (const byte *)buf;
    for (uint row = 0; row < _cursorH; row++)
        memcpy(_cursorData + row * 64, src + row * w, _cursorW);

    _screenDirty = true;
}

void OpenFPGAGraphicsManager::moveMouse(int dx, int dy) {
    _cursorX += dx;
    _cursorY += dy;
    if (_cursorX < 0) _cursorX = 0;
    if (_cursorY < 0) _cursorY = 0;
    if (_cursorX >= (int16)_screenW) _cursorX = _screenW - 1;
    if (_cursorY >= (int16)_screenH) _cursorY = _screenH - 1;
    _screenDirty = true;
}

void OpenFPGAGraphicsManager::drawCursor(uint8_t *dst) const {
    if (!_cursorVisible || _cursorW == 0 || _cursorH == 0)
        return;

    int yOff = (OPENFPGA_SCREEN_H - _screenH) / 2;
    int cx = _cursorX - _cursorHotX;
    int cy = _cursorY - _cursorHotY + yOff;

    for (uint row = 0; row < _cursorH; row++) {
        int sy = cy + (int)row;
        if (sy < 0 || sy >= OPENFPGA_SCREEN_H) continue;
        for (uint col = 0; col < _cursorW; col++) {
            int sx = cx + (int)col;
            if (sx < 0 || sx >= OPENFPGA_SCREEN_W) continue;
            uint8_t px = _cursorData[row * 64 + col];
            if (px != (_cursorKeycolor & 0xFF))
                dst[sy * OPENFPGA_SCREEN_W + sx] = px;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * OSystem_OpenFPGA
 * ═══════════════════════════════════════════════════════════════════ */

OSystem_OpenFPGA::OSystem_OpenFPGA()
    : _startTime(0), _ofGfx(nullptr),
      _mouseButtonL(false), _mouseButtonR(false), _autoDismissCounter(60) {
}

OSystem_OpenFPGA::~OSystem_OpenFPGA() {
}

void OSystem_OpenFPGA::initBackend() {
    /* Guard against double init (main calls us, then scummvm_main calls us again) */
    if (_ofGfx)
        return;

    _startTime = of_time_ms();

    _ofGfx = new OpenFPGAGraphicsManager();
    _graphicsManager = _ofGfx;

    _mixerManager = new OpenFPGAMixerManager();
    _mixerManager->init();

    _timerManager = new DefaultTimerManager();
    _eventManager = new DefaultEventManager(this);
    _savefileManager = new OpenFPGASaveFileManager();
    _fsFactory = new OpenFPGAFilesystemFactory();

    BaseBackend::initBackend();
}

bool OSystem_OpenFPGA::pollEvent(Common::Event &event) {
    /* Pump timers + audio mixer.  pollEvent fires at engine frame
     * rate; the mixer's update() tops up the SDK audio FIFO while it
     * has room, so as long as we're called more often than the FIFO
     * drains (~21 ms at 48 kHz stereo) audio stays gapless. */
    ((DefaultTimerManager *)getTimerManager())->checkTimers();
    ((OpenFPGAMixerManager *)_mixerManager)->update();

    of_input_poll();

    /* Auto-dismiss GUI dialogs: send Return keypress once after startup.
     * This handles the "unknown game version" dialog that blocks. */
    if (_autoDismissCounter > 0) {
        _autoDismissCounter--;
        if (_autoDismissCounter == 0) {
            event.type = Common::EVENT_KEYDOWN;
            event.kbd.keycode = Common::KEYCODE_RETURN;
            event.kbd.ascii = '\r';
            event.kbd.flags = 0;
            return true;
        }
    }

    /* Analog stick → mouse movement */
    of_input_state_t state;
    of_input_state(0, &state);

    const int deadzone = 4000;
    const int maxSpeed = 6;
    int lx = state.joy_lx;
    int ly = state.joy_ly;
    if (lx > -deadzone && lx < deadzone) lx = 0;
    if (ly > -deadzone && ly < deadzone) ly = 0;

    int dx = 0, dy = 0;
    if (lx != 0) dx = (lx * maxSpeed) / 32767;
    if (ly != 0) dy = (ly * maxSpeed) / 32767;

    /* D-pad for precision */
    if (of_btn(OF_BTN_LEFT))  dx -= 1;
    if (of_btn(OF_BTN_RIGHT)) dx += 1;
    if (of_btn(OF_BTN_UP))    dy -= 1;
    if (of_btn(OF_BTN_DOWN))  dy += 1;

    if (dx != 0 || dy != 0) {
        _ofGfx->moveMouse(dx, dy);
        event.type = Common::EVENT_MOUSEMOVE;
        event.mouse.x = _ofGfx->getMouseX();
        event.mouse.y = _ofGfx->getMouseY();
        return true;
    }

    /* A = left click */
    bool aDown = of_btn(OF_BTN_A) != 0;
    if (aDown != _mouseButtonL) {
        _mouseButtonL = aDown;
        event.type = aDown ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
        event.mouse.x = _ofGfx->getMouseX();
        event.mouse.y = _ofGfx->getMouseY();
        return true;
    }

    /* B = right click */
    bool bDown = of_btn(OF_BTN_B) != 0;
    if (bDown != _mouseButtonR) {
        _mouseButtonR = bDown;
        event.type = bDown ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP;
        event.mouse.x = _ofGfx->getMouseX();
        event.mouse.y = _ofGfx->getMouseY();
        return true;
    }

    /* Start = F5 (save/load menu) */
    if (of_btn_pressed(OF_BTN_START)) {
        event.type = Common::EVENT_KEYDOWN;
        event.kbd.keycode = Common::KEYCODE_F5;
        event.kbd.ascii = Common::ASCII_F5;
        event.kbd.flags = 0;
        return true;
    }

    /* Select = Escape (skip cutscene) */
    if (of_btn_pressed(OF_BTN_SELECT)) {
        event.type = Common::EVENT_KEYDOWN;
        event.kbd.keycode = Common::KEYCODE_ESCAPE;
        event.kbd.ascii = 27;
        event.kbd.flags = 0;
        return true;
    }

    /* Y = pause */
    if (of_btn_pressed(OF_BTN_Y)) {
        event.type = Common::EVENT_KEYDOWN;
        event.kbd.keycode = Common::KEYCODE_SPACE;
        event.kbd.ascii = ' ';
        event.kbd.flags = 0;
        return true;
    }

    return false;
}

Common::MutexInternal *OSystem_OpenFPGA::createMutex() {
    return new NullMutexInternal();
}

uint32 OSystem_OpenFPGA::getMillis(bool skipRecord) {
    return of_time_ms() - _startTime;
}

void OSystem_OpenFPGA::delayMillis(uint msecs) {
    usleep(msecs * 1000);
}

void OSystem_OpenFPGA::getTimeAndDate(TimeDate &td, bool skipRecord) const {
    /* No RTC on Pocket — return a fixed date */
    td.tm_sec = 0;
    td.tm_min = 0;
    td.tm_hour = 12;
    td.tm_mday = 1;
    td.tm_mon = 0;
    td.tm_year = 125; /* 2025 - 1900 */
    td.tm_wday = 3;
}

void OSystem_OpenFPGA::quit() {
    for (;;) {}
}

void OSystem_OpenFPGA::logMessage(LogMessageType::Type type, const char *message) {
    /* Show latest message at the bottom two lines (28-29).  Use the
     * VT100 "Erase in Line" escape (\x1b[2K) to clear each row -- vs
     * writing 40 literal spaces, which used to flood phdpd's mirrored
     * byte stream with whitespace even though the on-screen result was
     * the same. */
    char buf[80];
    strncpy(buf, message, 79);
    buf[79] = '\0';
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    printf("\x1b[28;1H\x1b[2K\x1b[29;1H\x1b[2K\x1b[28;1H%s", buf);
    fflush(stdout);
}

void OSystem_OpenFPGA::addSysArchivesToSearchSet(Common::SearchSet &s, int priority) {
    /* No system archives on this platform */
}

/* ScummVM's persistent config slot is per-game (monkey1.ini etc.).
 * main() resolves the actual filename via opendir at startup and
 * stashes it here. */

namespace {

char g_iniPath[64] = "";
const char *iniPath() { return g_iniPath[0] ? g_iniPath : nullptr; }

class ConfigWriteStream : public Common::MemoryWriteStreamDynamic {
public:
    ConfigWriteStream(const char *path)
        : Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES),
          _path(path), _flushed(false), _err(false) {}

    ~ConfigWriteStream() override { flushToSlot(); }

    bool flush() override { flushToSlot(); return !_err; }
    bool err() const override { return _err || Common::MemoryWriteStreamDynamic::err(); }

private:
    void flushToSlot() {
        if (_flushed) return;
        _flushed = true;
        FILE *f = fopen(_path, "wb");
        if (!f) { _err = true; return; }
        uint32 n = size();
        if (n && fwrite(getData(), 1, n, f) != n) _err = true;
        fclose(f);
    }
    const char *_path;
    bool        _flushed;
    bool        _err;
};

} // namespace

void openfpga_set_config_path(const char *path) {
    if (!path) { g_iniPath[0] = '\0'; return; }
    snprintf(g_iniPath, sizeof(g_iniPath), "%s", path);
}

Common::SeekableReadStream *OSystem_OpenFPGA::createConfigReadStream() {
    const char *path = iniPath();
    if (!path) return nullptr;
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return nullptr; }
    fseek(f, 0, SEEK_SET);
    byte *buf = (byte *)malloc((size_t)sz);
    if (!buf) { fclose(f); return nullptr; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return nullptr; }
    fclose(f);
    return new Common::MemoryReadStream(buf, (uint32)sz, DisposeAfterUse::YES);
}

Common::WriteStream *OSystem_OpenFPGA::createConfigWriteStream() {
    /* Writes to the nonvolatile config slot currently starve the
     * launcher's UART pump on the Pocket -- same shape as the ISO read
     * issue the cdiso CR flagged for the OS-side yielding cache-fill
     * follow-up.  Until that lands, refuse config persistence so
     * ScummVM's setAndFlush calls (notably updateGameGUIOptions during
     * createInstance) early-return from flushToDisk instead of
     * blocking.  Engine options stay in-memory for the session. */
    return nullptr;
    (void)iniPath();
}

Common::Path OSystem_OpenFPGA::getDefaultConfigFileName() {
    const char *path = iniPath();
    return path ? Common::Path(path) : Common::Path();
}

Common::Path OSystem_OpenFPGA::getDefaultLogFileName() {
    return Common::Path();
}

void OSystem_OpenFPGA::messageBox(LogMessageType::Type type, const char *message) {
    /* Don't show GUI dialog — just log and continue.  Use \x1b[2K to
     * clear each row instead of padding spaces. */
    char buf[41];
    strncpy(buf, message, 40);
    buf[40] = '\0';
    printf("\x1b[25;1H\x1b[2KMSGBOX:\x1b[26;1H\x1b[2K%s", buf);
    fflush(stdout);
}

void OSystem_OpenFPGA::fatalError() {
    /* Show the terminal so the user can read the panic message even
     * if the engine had switched to framebuffer-only display. */
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\x1b[27;1H\x1b[2K*** FATAL ERROR — halted ***");
    fflush(stdout);
    for (;;) { usleep(100 * 1000); }
}
