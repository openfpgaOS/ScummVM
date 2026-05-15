// Minimal stubs for ScummVM externs referenced by the audio + common
// libraries we pull in, but which aren't actually exercised by our
// WMA Pro test path.

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "common/scummsys.h"
#include "common/textconsole.h"
#include "common/mutex.h"
#include "common/types.h"
#include "common/stream.h"
#include "common/system.h"

// --- debug / debugN / debugC -----------------------------------------------

void debug(int level, const char *fmt, ...) {
	(void)level;
	va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a);
}
void debug(const char *fmt, ...) {
	va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a);
}
void debugN(int level, const char *fmt, ...) {
	(void)level;
	va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}
void debugN(const char *fmt, ...) {
	va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}
void debugC(int level, uint32 chan, const char *fmt, ...) {
	(void)level; (void)chan;
	va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a);
}
void debugCN(int level, uint32 chan, const char *fmt, ...) {
	(void)level; (void)chan;
	va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}

// --- Mutex / StackLock -----------------------------------------------------

namespace Common {

Mutex::Mutex() : _mutex(nullptr) {}
Mutex::~Mutex() {}
bool Mutex::lock()   { return true; }
bool Mutex::unlock() { return true; }

StackLock::StackLock(const Mutex &mutex, const char *mutexName)
	: _mutex(mutex._mutex), _mutexName(mutexName) {}
StackLock::StackLock(MutexInternal *mutex, const char *mutexName)
	: _mutex(mutex), _mutexName(mutexName) {}
StackLock::~StackLock() {}
bool StackLock::lock()   { return true; }
bool StackLock::unlock() { return true; }

} // namespace Common

// --- Audio helpers referenced by raw.cpp's vtable / templates --------------

namespace Audio {

// raw.cpp's seek() instantiates this; we don't exercise seek in the test.
uint32 convertTimeToStreamPos(const class Timestamp &, int, bool) { return 0; }

class QueuingAudioStream;
QueuingAudioStream *makeQueuingAudioStream(int, bool) { return nullptr; }

class AudioStream;
class SeekableAudioStream;
SeekableAudioStream *makeQuickTimeStream(class Common::SeekableReadStream *,
                                         DisposeAfterUse::Flag) { return nullptr; }
SeekableAudioStream *makeWAVStream(class Common::SeekableReadStream *,
                                   DisposeAfterUse::Flag) { return nullptr; }

} // namespace Audio

// --- Common::File stubs (never actually opened in our test) ----------------

#include "common/file.h"

namespace Common {

File::File() : _handle(nullptr) {}
File::~File() {}
bool File::open(const Path &) { return false; }
bool File::open(const Path &, Archive &) { return false; }
bool File::open(SeekableReadStream *, const String &) { return false; }
bool File::open(const FSNode &) { return false; }
bool File::isOpen() const { return false; }
void File::close() {}
bool File::err() const { return true; }
void File::clearErr() {}
bool File::eos() const { return true; }
int64 File::pos()  const { return 0; }
int64 File::size() const { return 0; }
uint32 File::read(void *, uint32) { return 0; }
bool File::seek(int64, int) { return false; }
bool File::exists(const Path &) { return false; }

} // namespace Common

// --- OSystem ---------------------------------------------------------------

OSystem *g_system = nullptr;
