/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * FFmpeg compatibility shim for the WMA Pro decoder port.
 *
 * Lets the translated body of FFmpeg's libavcodec/wmaprodec.c compile
 * against ScummVM's primitives without rewriting every line that calls
 * into FFmpeg internals.  The goal is faithful structural mapping so
 * future FFmpeg syncs stay tractable.
 *
 * Mapping (FFmpeg -> ScummVM):
 *   GetBitContext           -> Common::BitStream8MSB *
 *   get_bits/show_bits/...  -> bitstream method calls
 *   VLC / VLCElem           -> Common::Huffman<Common::BitStream8MSB>
 *   AVTXContext, mdct_calc  -> Math::MDCT
 *   AVFloatDSPContext       -> inline DSP helpers below
 *   av_log                  -> ScummVM warning()
 *   av_malloc/free          -> new[]/delete[]
 *   AV_RL16/24/32           -> READ_LE_UINT* / hand-rolled
 *   AVCodecContext          -> WMAProCodecCtxStub (just the fields used)
 *   AVFrame / AVPacket      -> WMAProFrameStub / WMAProPacketStub
 *
 * This header is intentionally not exported beyond the WMA Pro
 * translation units (wmapro.cpp + wmapro_body.cpp).
 */

#ifndef AUDIO_DECODERS_WMAPRO_COMPAT_H
#define AUDIO_DECODERS_WMAPRO_COMPAT_H

#include "common/scummsys.h"
#include "common/bitstream.h"
#include "common/compression/huffman.h"
#include "common/endian.h"
#include "common/textconsole.h"
#include "common/util.h"

#include "math/mdct.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

namespace Audio {
namespace WMAProInternal {

// ---------------------------------------------------------------------------
// Bitstream (GetBitContext shim over Common::BitStream8MSB)
// ---------------------------------------------------------------------------

struct GetBitContext {
	Common::BitStream8MSB *bs;
	uint32 sizeInBits;
};

static inline int init_get_bits(GetBitContext *s, const uint8 *, int) {
	// In our port we always wire the BitStream8MSB outside and store the
	// pointer.  init_get_bits is a no-op; callers that need to re-init
	// use init_get_bits8 with a fresh stream.
	(void)s;
	return 0;
}

static inline int init_get_bits8(GetBitContext *s, const uint8 *, int) {
	(void)s;
	return 0;
}

static inline unsigned get_bits(GetBitContext *s, int n) {
	return s->bs->getBits(n);
}

static inline unsigned get_bits1(GetBitContext *s) {
	return s->bs->getBit();
}

static inline unsigned get_bitsz(GetBitContext *s, int n) {
	return n ? s->bs->getBits(n) : 0;
}

static inline unsigned show_bits(GetBitContext *s, int n) {
	return s->bs->peekBits(n);
}

static inline void skip_bits(GetBitContext *s, int n) {
	s->bs->skip(n);
}

static inline void skip_bits1(GetBitContext *s) {
	s->bs->skip(1);
}

static inline void skip_bits_long(GetBitContext *s, int n) {
	s->bs->skip(n);
}

static inline int get_bits_count(GetBitContext *s) {
	return (int)s->bs->pos();
}

static inline int get_bits_left(GetBitContext *s) {
	return (int)(s->sizeInBits - s->bs->pos());
}

static inline void align_get_bits(GetBitContext *s) {
	int n = s->bs->pos() & 7;
	if (n)
		s->bs->skip(8 - n);
}

// ---------------------------------------------------------------------------
// VLC (Variable-length code) shim
//
// FFmpeg's table-driven VLCElem decoder is replaced by Common::Huffman.
// We expose just the surface wmaprodec.c uses: VLCElem table type,
// VLC struct holding (table,bits), get_vlc2(), and ff_init_vlc_sparse.
// ---------------------------------------------------------------------------

typedef int VLCElem;   // opaque -- the actual table is owned by Huffman

class VLCWrapper {
public:
	VLCWrapper() : huffman(nullptr) {}
	~VLCWrapper() { delete huffman; }
	Common::Huffman<Common::BitStream8MSB> *huffman;
};

typedef struct VLC {
	int             bits;
	VLCWrapper      *wrapper;   // ScummVM-side state
	const VLCElem   *table;     // FFmpeg-side; we ignore it
	int             table_size;
} VLC;

static inline int get_vlc2(GetBitContext *s, const VLCElem *table, int bits, int depth) {
	// Resolve table->wrapper via a pointer hack: in our translation we
	// always pass &someVLC.table where someVLC has a wrapper attached.
	// We rely on (table - offset) to recover the parent VLC, but that's
	// fragile -- so wmaprodec.c needs lightly editing to pass the VLC
	// or wrapper directly via the macros below.
	(void)table; (void)bits; (void)depth; (void)s;
	error("WMAProInternal::get_vlc2 stub hit -- decoder not finished");
	return -1;
}

// Helper macro used inside the translated body to pull from a VLC*
#define WMAPRO_VLC(vlcPtr, defaultDepth) \
	((vlcPtr).wrapper->huffman->getSymbol(*bs_for_huffman))

// ---------------------------------------------------------------------------
// MDCT shim (uses ScummVM's Math::MDCT)
// ---------------------------------------------------------------------------

typedef Math::MDCT AVTXContext;
typedef void (*av_tx_fn)(AVTXContext *s, void *dst, void *src, ptrdiff_t stride);

static inline int ff_tx_init(AVTXContext **ctx, av_tx_fn *fn,
                             int type, int inv, int len, const void *scale,
                             uint64 flags) {
	(void)type; (void)scale; (void)flags;
	// ScummVM's MDCT: bit-count, inverse-flag, scale.  We pass log2(len).
	int logN = 0;
	while ((1 << logN) < len) ++logN;
	*ctx = new Math::MDCT(logN, inv != 0, 1.0f);
	*fn = nullptr;   // we call ctx->calcIMDCT directly in the body
	return 0;
}

static inline void av_tx_uninit(AVTXContext **ctx) {
	delete *ctx;
	*ctx = nullptr;
}

// ---------------------------------------------------------------------------
// FloatDSP shim (inline implementations of the bits wmaprodec.c uses)
// ---------------------------------------------------------------------------

typedef struct AVFloatDSPContext { int dummy; } AVFloatDSPContext;

static inline AVFloatDSPContext *avpriv_float_dsp_alloc(int) {
	return new AVFloatDSPContext{0};
}

static inline void wmapro_vector_fmul_window(float *dst, const float *src0,
                                              const float *src1,
                                              const float *win, int len) {
	for (int i = 0; i < len; i++) {
		float s0 = src0[len - i - 1];
		float s1 = src1[i];
		float w0 = win[i];
		float w1 = win[len - i - 1];
		dst[i]           = s0 * w1 - s1 * w0;
		dst[2 * len - 1 - i] = s0 * w0 + s1 * w1;
	}
}

static inline void wmapro_vector_fmul_scalar(float *dst, const float *src,
                                              float mul, int len) {
	for (int i = 0; i < len; i++) dst[i] = src[i] * mul;
}

static inline void wmapro_vector_fmac_scalar(float *dst, const float *src,
                                              float mul, int len) {
	for (int i = 0; i < len; i++) dst[i] += src[i] * mul;
}

// ---------------------------------------------------------------------------
// AVCodecContext / AVFrame / AVPacket minimal stubs
// ---------------------------------------------------------------------------

typedef struct AVCodecContext {
	int sample_rate;
	int channels;
	int bit_rate;
	int block_align;
	uint8 *extradata;
	int extradata_size;
	int err_recognition;
} AVCodecContext;

typedef struct AVFrame {
	int16 *extended_data;   // we keep mono interleaved int16 for now
	int nb_samples;
	int channels;
} AVFrame;

typedef struct AVPacket {
	const uint8 *data;
	int size;
} AVPacket;

// ---------------------------------------------------------------------------
// Memory shim
// ---------------------------------------------------------------------------

static inline void *av_malloc(size_t n)         { return ::malloc(n); }
static inline void *av_mallocz(size_t n)        { void *p = ::malloc(n); if (p) ::memset(p, 0, n); return p; }
static inline void *av_calloc(size_t a, size_t b) { return ::calloc(a, b); }
static inline void  av_free(void *p)            { ::free(p); }
template<typename T>
static inline void  av_freep(T **pp)            { ::free(*pp); *pp = nullptr; }

// ---------------------------------------------------------------------------
// Logging shim
// ---------------------------------------------------------------------------

enum {
	AV_LOG_ERROR   = 0,
	AV_LOG_WARNING = 1,
	AV_LOG_INFO    = 2,
	AV_LOG_DEBUG   = 3
};

#define av_log(ctx, level, fmt, ...) \
	do { (void)(ctx); (void)(level); ::warning(fmt, ##__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// IntReadWrite shim
// ---------------------------------------------------------------------------

static inline uint16 AV_RL16(const void *p) { return READ_LE_UINT16(p); }
static inline uint32 AV_RL24(const void *p) {
	const uint8 *b = (const uint8 *)p;
	return (uint32)b[0] | ((uint32)b[1] << 8) | ((uint32)b[2] << 16);
}
static inline uint32 AV_RL32(const void *p) { return READ_LE_UINT32(p); }

// ---------------------------------------------------------------------------
// Attributes / alignment
// ---------------------------------------------------------------------------

#define av_cold
#define av_unused
#define av_always_inline inline
#define av_check_constexpr(x) (x)

#define DECLARE_ALIGNED(N, T, V) T V
#define AV_INPUT_BUFFER_PADDING_SIZE 64

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

#define AVERROR(e)              (-(e))
#define AVERROR_INVALIDDATA     (-22)
#define AVERROR_PATCHWELCOME    (-40)
#define AVERROR_EOF             (-541478725)

// ---------------------------------------------------------------------------
// Tx flags placeholder
// ---------------------------------------------------------------------------

#define AV_TX_FLOAT_MDCT 0
#define AV_TX_INPLACE    0

} // namespace WMAProInternal
} // namespace Audio

#endif // AUDIO_DECODERS_WMAPRO_COMPAT_H
