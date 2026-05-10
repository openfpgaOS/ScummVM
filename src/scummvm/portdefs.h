/*
 * portdefs.h -- ScummVM port definitions for openfpgaOS
 *
 * Included by ScummVM's common/scummsys.h when NONSTANDARD_PORT is
 * defined. With the new SDK we statically link upstream musl 1.2.5,
 * which provides a complete C library (stdio, stdlib, math, etc.),
 * so most of what this header used to patch around is gone.
 */

#ifndef _OPENFPGA_PORTDEFS_H
#define _OPENFPGA_PORTDEFS_H

/* Standard C headers (provided by musl) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>

/* C++ headers (our freestanding shims under compat/cxx_includes/).
 * The toolchain's libstdc++ is built against newlib and links against
 * symbols musl doesn't ship, so we substitute minimal shims for the
 * handful of headers ScummVM needs. */
#ifdef __cplusplus
#include <new>
#include <limits>
#endif

/* Workaround: Replace dynamic_cast with reinterpret_cast since we use -fno-rtti.
 * This loses type safety but allows compilation. The SCUMM engine's actual
 * use of dynamic_cast is limited and safe in practice. */
#define dynamic_cast reinterpret_cast

/* ScummVM platform configuration — endianness and alignment are set via
 * a minimal config.h (HAVE_CONFIG_H) to avoid the platform-detection
 * error in scummsys.h. */

/* Disable features not available on this platform */
#ifndef DISABLE_COMMAND_LINE
#define DISABLE_COMMAND_LINE
#endif

/* We stream audio from disk to save memory */
#ifndef STREAM_AUDIO_FROM_DISK
#define STREAM_AUDIO_FROM_DISK
#endif

#endif /* _OPENFPGA_PORTDEFS_H */
