/*
 * zlib_stub.cpp -- Minimal zlib stubs for ScummVM
 *
 * SCUMM v5/v6 games (MI1, MI2, Indy4, DOTT, Sam&Max) don't need zlib.
 * Only COMI (v7) and some later games use zlib compression.
 * These stubs allow linking without the real zlib library.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "common/str.h"
#include "common/savefile.h"
#include "common/stream.h"

extern "C" {
#include <stdint.h>

/* Minimal zlib type stubs */
typedef void *voidpf;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef unsigned char Byte;
typedef Byte *Bytef;
typedef uLong *uLongf;

/* Return codes */
#define Z_OK            0
#define Z_STREAM_END    1
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_VERSION_ERROR (-6)

int compress(Bytef *, uLongf *, const Bytef *, uLong) {
    return Z_STREAM_ERROR;
}

int uncompress(Bytef *, uLongf *, const Bytef *, uLong) {
    return Z_STREAM_ERROR;
}

uLong crc32(uLong crc, const Bytef *buf, uInt len) {
    (void)buf; (void)len;
    return crc;
}

uLong adler32(uLong adler, const Bytef *buf, uInt len) {
    (void)buf; (void)len;
    return adler;
}

} /* extern "C" */

/* ScummVM zlib wrapper stubs — these are called by common/ code but
 * we don't have zlib available. Return error/null. */
namespace Common {

bool inflateZlib(byte *dst, unsigned long *dstLen, const byte *src, unsigned long srcLen) {
    return false;
}

bool inflateZlibHeaderless(byte *dst, uint *dstLen, const byte *src, uint srcLen,
                           const byte *dict, uint dictLen) {
    return false;
}

bool inflateClickteam(byte *dst, uint *dstLen, const byte *src, uint srcLen) {
    return false;
}

SeekableReadStream *wrapCompressedReadStream(SeekableReadStream *toBeWrapped,
                                              DisposeAfterUse::Flag disposeParent,
                                              uint64 knownSize) {
    /* No compression support — return the stream as-is */
    return toBeWrapped;
}

WriteStream *wrapCompressedWriteStream(WriteStream *toBeWrapped) {
    return toBeWrapped;
}

SeekableReadStream *wrapClickteamReadStream(SeekableReadStream *toBeWrapped,
                                             DisposeAfterUse::Flag disposeParent,
                                             uint64 knownSize) {
    return toBeWrapped;
}

/* matchString, tag2string, replace are now provided by str.cpp (no SCUMMVM_UTIL) */

/* OutSaveFile and SaveFileManager are now in backends/saves/savefile.cpp */

} /* namespace Common */

/* SVGBitmap stub — include the actual header to get the right class layout */
#include "graphics/svg.h"

namespace Graphics {
SVGBitmap::SVGBitmap(Common::SeekableReadStream *in, int targetWidth, int targetHeight) {
    /* SVG not supported on this platform — create empty surface */
    (void)in;
    create(targetWidth > 0 ? targetWidth : 1, targetHeight > 0 ? targetHeight : 1,
           PixelFormat(4, 8, 8, 8, 8, 24, 16, 8, 0));
}
}

/* GUI stub */
namespace GUI {
void dumpAllDialogs(const Common::String &) {}
}

