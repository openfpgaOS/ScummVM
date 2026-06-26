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

// Based on FFmpeg's libavcodec/wmaprodec.c (LGPL).
// WMA Pro / xWMA decoder port for ScummVM.
//
// Translation status:
//   * State, constants, ctor + init: translated and compiles.
//   * Init helpers (initFromExtradata, initVlcTables, initMdctsAndWindows,
//     initSfbOffsets): translated; tables/MDCTs/windows allocated.
//   * Decoder helpers (decodeSubframeLength, decodeTilehdr,
//     decodeDecorrelationMatrix, decodeChannelTransform, decodeCoeffs,
//     decodeScaleFactors, decodeSubframe, decodeFrameInternal,
//     decodePacket): scaffolded; return -1 / no-op until the bodies are
//     translated from FFmpeg in follow-up commits.
//
// decodeFrame() returns nullptr (silent) for now.

#include "audio/decoders/wmapro.h"
#include "audio/decoders/wmaprodata.h"

#include "audio/audiostream.h"
#include "audio/decoders/raw.h"
#include "common/bitstream.h"
#include "common/compression/huffman.h"
#include "common/endian.h"
#include "common/memstream.h"
#include "common/stream.h"
#include "common/textconsole.h"
#include "common/util.h"

#include "common/algorithm.h"
#include "common/array.h"

#include "math/mdct.h"
#include "math/sinewindows.h"

#include <math.h>
#include <string.h>

namespace Audio {

namespace {

/**
 * Canonical-Huffman code generator.
 *
 * FFmpeg's wmaprodata.h ships Huffman tables as (symbol, length) pairs
 * with no explicit code bits.  ScummVM's Common::Huffman wants explicit
 * code bits.  This helper synthesises canonical Huffman codes from the
 * (length, symbol) input arrays and hands the (codes, lengths, symbols)
 * triple to a new Huffman.
 *
 * Length=0 entries are filtered out (FFmpeg uses them as "unused").
 * symBias is added to every symbol (mirrors FFmpeg's xlat).
 */
struct WmaproVlcEntry {
	int     origIdx;
	uint8   len;
	uint32  sym;
};

static bool wmaproVlcLess(const WmaproVlcEntry &a, const WmaproVlcEntry &b) {
	if (a.len != b.len) return a.len < b.len;
	return a.origIdx < b.origIdx;
}

static Common::Huffman<Common::BitStream8MSB> *buildCanonicalVlc(
		const uint8 *lengths, int lenStride,
		const void *symbols, int symStride, int symSize,
		int count, int32 symBias) {

	Common::Array<WmaproVlcEntry> entries;
	entries.reserve(count);
	uint8 maxLen = 0;
	for (int i = 0; i < count; ++i) {
		uint8 len = *(const uint8 *)((const uint8 *)lengths + i * lenStride);
		if (len == 0) continue;
		uint32 sym;
		if (symSize == 1) {
			sym = *(const uint8 *)((const uint8 *)symbols + i * symStride);
		} else {
			sym = *(const uint16 *)((const uint8 *)symbols + i * symStride);
		}
		WmaproVlcEntry e = { i, len, (uint32)((int32)sym + symBias) };
		entries.push_back(e);
		if (len > maxLen) maxLen = len;
	}
	if (entries.empty()) return nullptr;

	Common::sort(entries.begin(), entries.end(), wmaproVlcLess);

	const int n = (int)entries.size();
	Common::Array<uint32> codes((uint32)n);
	Common::Array<uint8>  lens((uint32)n);
	Common::Array<uint32> syms((uint32)n);
	uint32 code = 0;
	uint8  prevLen = 0;
	for (int i = 0; i < n; ++i) {
		if (entries[i].len > prevLen) {
			code <<= (entries[i].len - prevLen);
			prevLen = entries[i].len;
		}
		codes[i] = code;
		lens[i]  = entries[i].len;
		syms[i]  = entries[i].sym;
		++code;
	}

	return new Common::Huffman<Common::BitStream8MSB>(
		maxLen, (uint32)n, codes.data(), lens.data(), syms.data());
}

// From FFmpeg's wma.c.  Decodes a variable-length escape value used
// by the run-length coefficient coder.
static uint32 wmaGetLargeVal(Common::BitStream8MSB &bs) {
	int nBits = 8;
	if (bs.getBit()) {
		nBits += 8;
		if (bs.getBit()) {
			nBits += 8;
			if (bs.getBit())
				nBits += 7;
		}
	}
	return bs.getBits(nBits);
}

// From FFmpeg's wma.c.  Decodes run-length-coded spectral coefficients.
// Returns 0 on success, -1 on overflow.
static int wmaRunLevelDecode(Common::BitStream8MSB &bs,
                             Common::Huffman<Common::BitStream8MSB> *vlc,
                             const float *levelTable, const uint16 *runTable,
                             int version, float *ptr, int offset,
                             int numCoefs, int blockLen,
                             int frameLenBits, int coefNbBits) {
	const uint32 *ilvl = reinterpret_cast<const uint32 *>(levelTable);
	uint32       *iptr = reinterpret_cast<uint32 *>(ptr);
	const uint32  coefMask = blockLen - 1;

	for (; offset < numCoefs; ++offset) {
		uint32 code = vlc->getSymbol(bs);
		if (code > 1) {
			offset += runTable[code];
			uint32 sign = bs.getBit() ? 0u : 0xFFFFFFFFu;
			iptr[offset & coefMask] = ilvl[code] ^ (sign & 0x80000000u);
		} else if (code == 1) {
			break;
		} else {
			int level;
			if (!version) {
				level = bs.getBits(coefNbBits);
				offset += bs.getBits(frameLenBits);
			} else {
				level = wmaGetLargeVal(bs);
				if (bs.getBit()) {
					if (bs.getBit()) {
						if (bs.getBit()) {
							warning("WMAProCodec: broken escape sequence");
							return -1;
						} else {
							offset += bs.getBits(frameLenBits) + 4;
						}
					} else {
						offset += bs.getBits(2) + 1;
					}
				}
			}
			int sign = bs.getBit() ? 0 : -1;
			ptr[offset & coefMask] = (float)((level ^ sign) - sign);
		}
	}

	if (offset > numCoefs) {
		warning("WMAProCodec: spectral RLE overflow (%d > %d)", offset, numCoefs);
		return -1;
	}
	return 0;
}

} // anon namespace

// --------------------------------------------------------------------------
// Helpers translated 1:1 from FFmpeg
// --------------------------------------------------------------------------

// From FFmpeg's wma_common.c.
static int ff_wma_get_frame_len_bits(int sampleRate, int version,
                                     uint32 decodeFlags) {
	int frameLenBits;

	if (sampleRate <= 16000)
		frameLenBits = 9;
	else if (sampleRate <= 22050 || (sampleRate <= 32000 && version == 1))
		frameLenBits = 10;
	else if (sampleRate <= 48000 || version < 3)
		frameLenBits = 11;
	else if (sampleRate <= 96000)
		frameLenBits = 12;
	else
		frameLenBits = 13;

	if (version == 3) {
		int tmp = decodeFlags & 0x6;
		if (tmp == 0x2)
			++frameLenBits;
		else if (tmp == 0x4)
			--frameLenBits;
		else if (tmp == 0x6)
			frameLenBits -= 2;
	}
	return frameLenBits;
}

// av_log2 / av_popcount lightweight equivalents.
static int wmaproLog2(uint32 v) {
	int n = 0;
	while (v >>= 1) ++n;
	return n;
}

static int wmaproPopcount(uint32 v) {
	int c = 0;
	for (; v; v >>= 1) c += v & 1;
	return c;
}

static int wmaproClip(int v, int lo, int hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

// --------------------------------------------------------------------------
// WMAProCodec
// --------------------------------------------------------------------------

WMAProCodec::WMAProCodec(uint32 sampleRate, uint8 channels, uint32 bitRate,
                         uint32 blockAlign,
                         Common::SeekableReadStream *extraData)
	: _sampleRate(sampleRate),
	  _channels(channels),
	  _bitRate(bitRate),
	  _blockAlign(blockAlign),
	  _initialized(false),
	  _decodeFlags(0),
	  _lenPrefix(0),
	  _dynamicRangeCompression(0),
	  _bitsPerSample(16),
	  _samplesPerFrame(0),
	  _trimStart(0),
	  _trimEnd(0),
	  _log2FrameSize(0),
	  _lfeChannel(-1),
	  _maxNumSubframes(0),
	  _subframeLenBits(0),
	  _maxSubframeLenBit(0),
	  _minSamplesPerSubframe(0),
	  _sfVlc(nullptr),
	  _sfRlVlc(nullptr),
	  _vec4Vlc(nullptr),
	  _vec2Vlc(nullptr),
	  _vec1Vlc(nullptr),
	  _numSavedBits(0),
	  _frameOffset(0),
	  _subframeOffset(0),
	  _packetOffset(0),
	  _packetSequenceNumber(0),
	  _packetLoss(1),
	  _packetDone(0),
	  _frameNum(0),
	  _bufBitSize(0),
	  _drcGain(0),
	  _skipFrame(1),
	  _parsedAllSubframes(0),
	  _bs(nullptr),
	  _bsBits(0),
	  _subframeLen(0),
	  _nbChannels(channels),
	  _channelsForCurSubframe(0),
	  _numBands(0),
	  _transmitNumVecCoeffs(0),
	  _curSfbOffsets(nullptr),
	  _tableIdx(0),
	  _escLen(0),
	  _numChgroups(0) {

	memset(_mdct, 0, sizeof(_mdct));
	memset(_windows, 0, sizeof(_windows));
	memset(_coefVlc, 0, sizeof(_coefVlc));
	memset(_numSfb, 0, sizeof(_numSfb));
	memset(_sfbOffsets, 0, sizeof(_sfbOffsets));
	memset(_sfOffsets, 0, sizeof(_sfOffsets));
	memset(_subwooferCutoffs, 0, sizeof(_subwooferCutoffs));
	memset(_chgroup, 0, sizeof(_chgroup));
	memset(_channelCtx, 0, sizeof(_channelCtx));
	memset(_channelIndexesForCurSubframe, 0, sizeof(_channelIndexesForCurSubframe));
	memset(_frameData, 0, sizeof(_frameData));

	for (int i = 0; i < 33; ++i)
		_sin64[i] = sinf(i * (float)M_PI / 64.0f);

	_initialized = initFromExtradata(extraData);
	if (!_initialized) {
		warning("WMAProCodec: extradata init failed -- decoder unusable");
	}
}

WMAProCodec::~WMAProCodec() {
	for (int i = 0; i < (int)WMAPRO_BLOCK_SIZES; ++i) {
		delete _mdct[i];
		_mdct[i] = nullptr;
	}
	delete _sfVlc;
	delete _sfRlVlc;
	delete _vec4Vlc;
	delete _vec2Vlc;
	delete _vec1Vlc;
	for (int i = 0; i < 2; ++i)
		delete _coefVlc[i];
}

bool WMAProCodec::initFromExtradata(Common::SeekableReadStream *extra) {
	// FFmpeg's decode_init handles XMA1/XMA2/WMAPRO extradata variants.
	// We only support WMAPRO (xWMA / WMA Pro) with extradata >= 18 bytes.
	// The shape is the WAVEFORMATEX cbSize blob from the .wav/.xwb header:
	//
	//   offset  size  field
	//   0       2     bits_per_sample (often 16)
	//   2       4     channel_mask
	//   6       4     (reserved / GUID portion in some files)
	//   10      4     reserved
	//   14      2     decode_flags
	//   16      2     (varies)

	uint32 channelMask = 0;

	if (extra && extra->size() >= 18) {
		extra->seek(0);
		uint8 buf[18];
		extra->read(buf, 18);
		_bitsPerSample = (uint8)READ_LE_UINT16(buf + 0);
		channelMask    = READ_LE_UINT32(buf + 2);
		_decodeFlags   = READ_LE_UINT16(buf + 14);
		_nbChannels    = channelMask ? wmaproPopcount(channelMask) : _channels;
		if (_bitsPerSample < 1 || _bitsPerSample > 32) {
			warning("WMAProCodec: invalid bits_per_sample=%u", _bitsPerSample);
			return false;
		}
	} else {
		// XACT xWMA defaults.  Unlike WMA Pro in ASF, xWMA frames in
		// XACT wave banks have no length prefix, no DRC, and use the
		// codec-default frame length derived purely from sample rate
		// (no `decode_flags & 0x6` adjustment).  The maxNumSubframes
		// is typically 4 (log2 = 2 in bits 5:3).
		_decodeFlags   = 0x10;   // log2_max_num_subframes = 2 -> max = 4
		_bitsPerSample = 16;
		_nbChannels    = _channels;
	}

	// For XACT xWMA, _blockAlign (passed as entry.align from the XWB)
	// is NOT a WAVEFORMATEX nBlockAlign; it's an encoded index.  We
	// don't (yet) have the table to decode it, so fall back to the
	// canonical WMA Pro frame size for sample rates <= 48 kHz, which
	// matches the samples_per_frame = 1 << 11 = 2048 that those rates
	// always produce in ff_wma_get_frame_len_bits.  For higher rates
	// or future bit-exact validation we'll need the right block_align.
	_log2FrameSize = 11;

	_lenPrefix    = (_decodeFlags & 0x40) ? 1 : 0;
	int bits      = ff_wma_get_frame_len_bits(_sampleRate, 3, _decodeFlags);
	if (bits > WMAPRO_BLOCK_MAX_BITS) {
		warning("WMAProCodec: 14-bit block sizes unsupported");
		return false;
	}
	_samplesPerFrame = 1u << bits;

	int log2MaxNumSubframes = (_decodeFlags & 0x38) >> 3;
	_maxNumSubframes        = 1 << log2MaxNumSubframes;
	_maxSubframeLenBit      = (_maxNumSubframes == 16 || _maxNumSubframes == 4) ? 1 : 0;
	_subframeLenBits        = wmaproLog2(log2MaxNumSubframes) + 1;

	int numPossibleBlockSizes  = log2MaxNumSubframes + 1;
	_minSamplesPerSubframe     = _samplesPerFrame / _maxNumSubframes;
	_dynamicRangeCompression   = (_decodeFlags & 0x80) ? 1 : 0;

	if (_maxNumSubframes > MAX_SUBFRAMES) {
		warning("WMAProCodec: invalid number of subframes %d", _maxNumSubframes);
		return false;
	}
	if (_minSamplesPerSubframe < WMAPRO_BLOCK_MIN_SIZE) {
		warning("WMAProCodec: min_samples_per_subframe %d too small",
		        _minSamplesPerSubframe);
		return false;
	}
	if (_nbChannels <= 0 || _nbChannels > WMAPRO_MAX_CHANNELS) {
		warning("WMAProCodec: invalid channel count %d", _nbChannels);
		return false;
	}

	for (int i = 0; i < _nbChannels; ++i)
		_channelCtx[i].prev_block_len = _samplesPerFrame;

	_lfeChannel = -1;
	if (channelMask & 8) {
		for (uint32 mask = 1; mask < 16; mask <<= 1) {
			if (channelMask & mask)
				++_lfeChannel;
		}
	}

	initSfbOffsets();
	initMdctsAndWindows();
	initVlcTables();

	// Subwoofer cutoffs
	for (int i = 0; i < numPossibleBlockSizes; ++i) {
		int blockSize = _samplesPerFrame >> i;
		int cutoff = (440 * blockSize + 3LL * (_sampleRate >> 1) - 1) / _sampleRate;
		_subwooferCutoffs[i] = wmaproClip(cutoff, 4, blockSize);
	}

	return true;
}

void WMAProCodec::initSfbOffsets() {
	int log2MaxNumSubframes   = (_decodeFlags & 0x38) >> 3;
	int numPossibleBlockSizes = log2MaxNumSubframes + 1;
	int rate                  = _sampleRate;

	for (int i = 0; i < numPossibleBlockSizes; ++i) {
		int subframeLen = _samplesPerFrame >> i;
		int band = 1;
		_sfbOffsets[i][0] = 0;
		for (int x = 0;
		     x < MAX_BANDS - 1 && _sfbOffsets[i][band - 1] < subframeLen;
		     ++x) {
			int offset = (subframeLen * 2 * critical_freq[x]) / rate + 2;
			offset &= ~3;
			if (offset > _sfbOffsets[i][band - 1])
				_sfbOffsets[i][band++] = offset;
			if (offset >= subframeLen)
				break;
		}
		_sfbOffsets[i][band - 1] = subframeLen;
		_numSfb[i]               = band - 1;
		if (_numSfb[i] <= 0) {
			warning("WMAProCodec: num_sfb invalid");
			_numSfb[i] = 0;
		}
	}

	for (int i = 0; i < numPossibleBlockSizes; ++i) {
		for (int b = 0; b < _numSfb[i]; ++b) {
			int offset = ((_sfbOffsets[i][b] + _sfbOffsets[i][b + 1] - 1) << i) >> 1;
			for (int x = 0; x < numPossibleBlockSizes; ++x) {
				int v = 0;
				while (_sfbOffsets[x][v + 1] << x < offset) {
					++v;
					if (v >= MAX_BANDS) break;
				}
				_sfOffsets[i][x][b] = v;
			}
		}
	}
}

// 13-bit (8192-sample) sine window.  ScummVM's Math::getSineWindow()
// only ships windows for bits in [5,12]; WMA Pro's maximum block size is
// 13 bits so we provide our own.  Built once at first use.
static float g_wmaproSineWindow13[1 << 13];
static bool  g_wmaproSineWindow13Built = false;

static const float *wmaproSineWindowForBits(int bits) {
	if (bits == 13) {
		if (!g_wmaproSineWindow13Built) {
			const int n = 1 << 13;
			for (int k = 0; k < n; ++k)
				g_wmaproSineWindow13[k] = sinf((k + 0.5f) * (float)M_PI / (2.0f * n));
			g_wmaproSineWindow13Built = true;
		}
		return g_wmaproSineWindow13;
	}
	return Math::getSineWindow(bits);
}

void WMAProCodec::initMdctsAndWindows() {
	// MDCTs: inverse, scale = 1.0 / (2 ^ (block_min_bits + i - 1))
	//                       / (2 ^ (bits_per_sample - 1))
	for (int i = 0; i < (int)WMAPRO_BLOCK_SIZES; ++i) {
		double scale = 1.0 / (double)(1 << (WMAPRO_BLOCK_MIN_BITS + i - 1));
		scale /= (double)(1LL << (_bitsPerSample - 1));
		_mdct[i] = new Math::MDCT(WMAPRO_BLOCK_MIN_BITS + i, true, scale);
	}

	// Sine windows.  ff_sine_windows[winIdx] in FFmpeg maps to ScummVM's
	// Math::getSineWindow(bits) for sizes up to 12 bits; wmaproSineWindowForBits
	// handles the 13-bit (8192-sample) extension we need.
	for (int i = 0; i < (int)WMAPRO_BLOCK_SIZES; ++i) {
		int winBits = WMAPRO_BLOCK_MAX_BITS - i;
		_windows[WMAPRO_BLOCK_SIZES - i - 1] = wmaproSineWindowForBits(winBits);
	}
}

void WMAProCodec::initVlcTables() {
	// Shape A: T table[N][2] = { {symbol, length}, ... }.
	// FFmpeg call passes &table[0][1] for lengths (stride 2) and
	// &table[0][0] for symbols (stride 2, size 1).  xlat may add an
	// offset to each symbol.

	_sfVlc = buildCanonicalVlc(
		&scale_table[0][1], 2,
		&scale_table[0][0], 2, 1,
		HUFF_SCALE_SIZE, -60);

	_sfRlVlc = buildCanonicalVlc(
		&scale_rl_table[0][1], 2,
		&scale_rl_table[0][0], 2, 1,
		HUFF_SCALE_RL_SIZE, 0);

	_coefVlc[1] = buildCanonicalVlc(
		&coef1_table[0][1], 2,
		&coef1_table[0][0], 2, 1,
		HUFF_COEF1_SIZE, 0);

	_vec2Vlc = buildCanonicalVlc(
		&vec2_table[0][1], 2,
		&vec2_table[0][0], 2, 1,
		HUFF_VEC2_SIZE, -1);

	_vec1Vlc = buildCanonicalVlc(
		&vec1_table[0][1], 2,
		&vec1_table[0][0], 2, 1,
		HUFF_VEC1_SIZE, 0);

	// Shape B: separate uint8 lens[] + uint16 syms[].

	_coefVlc[0] = buildCanonicalVlc(
		coef0_lens, 1,
		coef0_syms, 2, 2,
		HUFF_COEF0_SIZE, 0);

	_vec4Vlc = buildCanonicalVlc(
		vec4_lens, 1,
		vec4_syms, 2, 2,
		HUFF_VEC4_SIZE, -1);

	if (!_sfVlc || !_sfRlVlc || !_coefVlc[0] || !_coefVlc[1] ||
	    !_vec4Vlc || !_vec2Vlc || !_vec1Vlc) {
		warning("WMAProCodec: VLC table init failed");
	}
}

AudioStream *WMAProCodec::decodeFrame(Common::SeekableReadStream &data) {
	if (!_initialized)
		return nullptr;

	int size = (int)data.size();
	if (size <= 0) return nullptr;

	byte *buf = new byte[size];
	data.read(buf, size);

	static bool firstCall = true;
	if (firstCall) {
		firstCall = false;
		warning("WMAProCodec cfg: rate=%u ch=%u blockAlign=%u",
		        _sampleRate, _channels, _blockAlign);
		warning("WMAProCodec parsed: decodeFlags=0x%x samplesPerFrame=%u "
		        "maxNumSubframes=%u minSamplesPerSubframe=%u "
		        "bitsPerSample=%u log2FrameSize=%u lenPrefix=%u",
		        _decodeFlags, _samplesPerFrame, _maxNumSubframes,
		        _minSamplesPerSubframe, _bitsPerSample, _log2FrameSize,
		        _lenPrefix);
		warning("WMAProCodec packet[0..15]: %02x %02x %02x %02x %02x %02x %02x %02x "
		        "%02x %02x %02x %02x %02x %02x %02x %02x  size=%d",
		        size > 0 ? buf[0] : 0, size > 1 ? buf[1] : 0,
		        size > 2 ? buf[2] : 0, size > 3 ? buf[3] : 0,
		        size > 4 ? buf[4] : 0, size > 5 ? buf[5] : 0,
		        size > 6 ? buf[6] : 0, size > 7 ? buf[7] : 0,
		        size > 8 ? buf[8] : 0, size > 9 ? buf[9] : 0,
		        size > 10 ? buf[10] : 0, size > 11 ? buf[11] : 0,
		        size > 12 ? buf[12] : 0, size > 13 ? buf[13] : 0,
		        size > 14 ? buf[14] : 0, size > 15 ? buf[15] : 0,
		        size);
	}

	int outCap   = _samplesPerFrame * _nbChannels;
	int16 *outPCM = new int16[outCap];

	int produced = decodePacket(buf, size, outPCM);
	delete[] buf;

	if (produced <= 0) {
		delete[] outPCM;
		return nullptr;
	}

	int byteSize = produced * _nbChannels * 2;
	byte flags = Audio::FLAG_16BITS | Audio::FLAG_LITTLE_ENDIAN;
	if (_nbChannels == 2) flags |= Audio::FLAG_STEREO;

	Common::MemoryReadStream *pcmStream =
		new Common::MemoryReadStream((const byte *)outPCM, byteSize,
		                              DisposeAfterUse::YES);
	return Audio::makeRawStream(pcmStream, _sampleRate, flags,
	                            DisposeAfterUse::YES);
}

// --------------------------------------------------------------------------
// Decoder helpers (stubs).  Bodies translated in follow-up commits.
// --------------------------------------------------------------------------

int WMAProCodec::decodeSubframeLength(int offset) {
	// From FFmpeg's decode_subframe_length.

	// Only one length possible -- no bits to read.
	if (offset == _samplesPerFrame - _minSamplesPerSubframe)
		return _minSamplesPerSubframe;

	if ((int)_bsBits - (int)_bs->pos() < 1)
		return -1;

	int frameLenShift = 0;
	if (_maxSubframeLenBit) {
		if (_bs->getBit())
			frameLenShift = 1 + _bs->getBits(_subframeLenBits - 1);
	} else {
		frameLenShift = _bs->getBits(_subframeLenBits);
	}

	int subframeLen = _samplesPerFrame >> frameLenShift;

	if (subframeLen < _minSamplesPerSubframe || subframeLen > _samplesPerFrame) {
		warning("WMAProCodec: broken frame: subframe_len %d", subframeLen);
		return -1;
	}
	return subframeLen;
}

int WMAProCodec::decodeTilehdr() {
	// From FFmpeg's decode_tilehdr.  Decides how the frame's samples
	// are split into subframes per channel.

	uint16 numSamples[WMAPRO_MAX_CHANNELS]    = { 0 };
	uint8  containsSubframe[WMAPRO_MAX_CHANNELS] = { 0 };
	int    channelsForCurSubframe = _nbChannels;
	int    fixedChannelLayout = 0;
	int    minChannelLen = 0;

	for (int c = 0; c < _nbChannels; ++c)
		_channelCtx[c].num_subframes = 0;

	if (_maxNumSubframes == 1 || _bs->getBit())
		fixedChannelLayout = 1;

	do {
		// Which channels contain this subframe?
		for (int c = 0; c < _nbChannels; ++c) {
			if (numSamples[c] == minChannelLen) {
				if (fixedChannelLayout || channelsForCurSubframe == 1 ||
				    (minChannelLen == _samplesPerFrame - _minSamplesPerSubframe))
					containsSubframe[c] = 1;
				else
					containsSubframe[c] = _bs->getBit();
			} else {
				containsSubframe[c] = 0;
			}
		}

		int subframeLen = decodeSubframeLength(minChannelLen);
		if (subframeLen <= 0)
			return -1;

		minChannelLen += subframeLen;
		for (int c = 0; c < _nbChannels; ++c) {
			ChannelCtx *chan = &_channelCtx[c];

			if (containsSubframe[c]) {
				if (chan->num_subframes >= MAX_SUBFRAMES) {
					warning("WMAProCodec: broken frame: num subframes > 31");
					return -1;
				}
				chan->subframe_len[chan->num_subframes] = subframeLen;
				numSamples[c] += subframeLen;
				++chan->num_subframes;
				if (numSamples[c] > _samplesPerFrame) {
					warning("WMAProCodec: broken frame: channel len > samples_per_frame");
					return -1;
				}
			} else if (numSamples[c] <= minChannelLen) {
				if (numSamples[c] < minChannelLen) {
					channelsForCurSubframe = 0;
					minChannelLen = numSamples[c];
				}
				++channelsForCurSubframe;
			}
		}
	} while (minChannelLen < _samplesPerFrame);

	for (int c = 0; c < _nbChannels; ++c) {
		int offset = 0;
		for (int i = 0; i < _channelCtx[c].num_subframes; ++i) {
			_channelCtx[c].subframe_offset[i] = offset;
			offset += _channelCtx[c].subframe_len[i];
		}
	}

	return 0;
}

void WMAProCodec::decodeDecorrelationMatrix(ChannelGroup *cg) {
	// From FFmpeg's decode_decorrelation_matrix.
	int offset = 0;
	int8 rotationOffset[WMAPRO_MAX_CHANNELS * WMAPRO_MAX_CHANNELS] = { 0 };

	memset(cg->decorrelation_matrix, 0,
	       _nbChannels * _nbChannels * sizeof(*cg->decorrelation_matrix));

	int pairs = cg->num_channels * (cg->num_channels - 1) >> 1;
	for (int i = 0; i < pairs; ++i)
		rotationOffset[i] = (int8)_bs->getBits(6);

	for (int i = 0; i < cg->num_channels; ++i)
		cg->decorrelation_matrix[cg->num_channels * i + i] =
			_bs->getBit() ? 1.0f : -1.0f;

	for (int i = 1; i < cg->num_channels; ++i) {
		for (int x = 0; x < i; ++x) {
			for (int y = 0; y < i + 1; ++y) {
				float v1 = cg->decorrelation_matrix[x * cg->num_channels + y];
				float v2 = cg->decorrelation_matrix[i * cg->num_channels + y];
				int n = rotationOffset[offset + x];
				float sinv, cosv;
				if (n < 32) {
					sinv = _sin64[n];
					cosv = _sin64[32 - n];
				} else {
					sinv =  _sin64[64 - n];
					cosv = -_sin64[n  - 32];
				}
				cg->decorrelation_matrix[y + x * cg->num_channels] = v1 * sinv - v2 * cosv;
				cg->decorrelation_matrix[y + i * cg->num_channels] = v1 * cosv + v2 * sinv;
			}
		}
		offset += i;
	}
}

int WMAProCodec::decodeChannelTransform() {
	// From FFmpeg's decode_channel_transform.
	_numChgroups = 0;
	if (_nbChannels <= 1)
		return 0;

	int remainingChannels = _channelsForCurSubframe;

	if (_bs->getBit()) {
		warning("WMAProCodec: channel transform bit unsupported");
		return -1;
	}

	for (_numChgroups = 0;
	     remainingChannels && _numChgroups < _channelsForCurSubframe;
	     ++_numChgroups) {
		ChannelGroup *cg = &_chgroup[_numChgroups];
		float **channelData = cg->channel_data;
		cg->num_channels = 0;
		cg->transform    = 0;

		// Channel mask
		if (remainingChannels > 2) {
			for (int i = 0; i < _channelsForCurSubframe; ++i) {
				int chIdx = _channelIndexesForCurSubframe[i];
				if (!_channelCtx[chIdx].grouped && _bs->getBit()) {
					++cg->num_channels;
					_channelCtx[chIdx].grouped = 1;
					*channelData++ = _channelCtx[chIdx].coeffs;
				}
			}
		} else {
			cg->num_channels = remainingChannels;
			for (int i = 0; i < _channelsForCurSubframe; ++i) {
				int chIdx = _channelIndexesForCurSubframe[i];
				if (!_channelCtx[chIdx].grouped)
					*channelData++ = _channelCtx[chIdx].coeffs;
				_channelCtx[chIdx].grouped = 1;
			}
		}

		// Transform type
		if (cg->num_channels == 2) {
			if (_bs->getBit()) {
				if (_bs->getBit()) {
					warning("WMAProCodec: unknown channel transform type");
					return -1;
				}
			} else {
				cg->transform = 1;
				if (_nbChannels == 2) {
					cg->decorrelation_matrix[0] =  1.0f;
					cg->decorrelation_matrix[1] = -1.0f;
					cg->decorrelation_matrix[2] =  1.0f;
					cg->decorrelation_matrix[3] =  1.0f;
				} else {
					cg->decorrelation_matrix[0] =  0.70703125f;
					cg->decorrelation_matrix[1] = -0.70703125f;
					cg->decorrelation_matrix[2] =  0.70703125f;
					cg->decorrelation_matrix[3] =  0.70703125f;
				}
			}
		} else if (cg->num_channels > 2) {
			if (_bs->getBit()) {
				cg->transform = 1;
				if (_bs->getBit()) {
					decodeDecorrelationMatrix(cg);
				} else {
					if (cg->num_channels > 6) {
						warning("WMAProCodec: coupled channels > 6 unsupported");
					} else {
						memcpy(cg->decorrelation_matrix,
						       default_decorrelation[cg->num_channels],
						       cg->num_channels * cg->num_channels *
						           sizeof(*cg->decorrelation_matrix));
					}
				}
			}
		}

		// Transform on/off per band
		if (cg->transform) {
			if (!_bs->getBit()) {
				for (int i = 0; i < _numBands; ++i)
					cg->transform_band[i] = (int8)_bs->getBit();
			} else {
				memset(cg->transform_band, 1, _numBands);
			}
		}

		remainingChannels -= cg->num_channels;
	}
	return 0;
}

int WMAProCodec::decodeCoeffs(int c) {
	// From FFmpeg's decode_coeffs.
	static const uint32 fvalTab[16] = {
		0x00000000u, 0x3f800000u, 0x40000000u, 0x40400000u,
		0x40800000u, 0x40a00000u, 0x40c00000u, 0x40e00000u,
		0x41000000u, 0x41100000u, 0x41200000u, 0x41300000u,
		0x41400000u, 0x41500000u, 0x41600000u, 0x41700000u
	};

	ChannelCtx *ci = &_channelCtx[c];
	int rlMode    = 0;
	int curCoeff  = 0;
	int numZeros  = 0;

	int vlctable = _bs->getBit();
	Common::Huffman<Common::BitStream8MSB> *vlc = _coefVlc[vlctable];
	const uint16 *run;
	const float  *level;
	if (vlctable) {
		run   = coef1_run;
		level = coef1_level;
	} else {
		run   = coef0_run;
		level = coef0_level;
	}

	// Vector-coded coefficients (4 at a time).
	while ((_transmitNumVecCoeffs || !rlMode) &&
	       (curCoeff + 3 < ci->num_vec_coeffs)) {
		uint32 vals[4];

		uint32 idx = _vec4Vlc->getSymbol(*_bs);

		if ((int)idx < 0) {
			for (int i = 0; i < 4; i += 2) {
				idx = _vec2Vlc->getSymbol(*_bs);
				if ((int)idx < 0) {
					uint32 v0 = _vec1Vlc->getSymbol(*_bs);
					if (v0 == HUFF_VEC1_SIZE - 1)
						v0 += wmaGetLargeVal(*_bs);
					uint32 v1 = _vec1Vlc->getSymbol(*_bs);
					if (v1 == HUFF_VEC1_SIZE - 1)
						v1 += wmaGetLargeVal(*_bs);
					float f0 = (float)v0;
					float f1 = (float)v1;
					memcpy(&vals[i], &f0, 4);
					memcpy(&vals[i + 1], &f1, 4);
				} else {
					vals[i]     = fvalTab[idx >> 4];
					vals[i + 1] = fvalTab[idx & 0xF];
				}
			}
		} else {
			vals[0] = fvalTab[idx >> 12];
			vals[1] = fvalTab[(idx >> 8) & 0xF];
			vals[2] = fvalTab[(idx >> 4) & 0xF];
			vals[3] = fvalTab[idx & 0xF];
		}

		for (int i = 0; i < 4; ++i) {
			if (vals[i]) {
				uint32 sign = _bs->getBit() ? 0u : 0xFFFFFFFFu;
				uint32 word = vals[i] ^ ((sign & 1u) << 31);
				memcpy(&ci->coeffs[curCoeff], &word, 4);
				numZeros = 0;
			} else {
				ci->coeffs[curCoeff] = 0;
				rlMode |= (++numZeros > _subframeLen >> 8);
			}
			++curCoeff;
		}
	}

	if (curCoeff < _subframeLen) {
		memset(&ci->coeffs[curCoeff], 0,
		       sizeof(*ci->coeffs) * (_subframeLen - curCoeff));
		int ret = wmaRunLevelDecode(*_bs, vlc, level, run, /*version=*/1,
		                            ci->coeffs, curCoeff, _subframeLen,
		                            _subframeLen, _subframeLen, _escLen);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int WMAProCodec::decodeScaleFactors() {
	// From FFmpeg's decode_scale_factors.

	for (int i = 0; i < _channelsForCurSubframe; ++i) {
		int c = _channelIndexesForCurSubframe[i];
		ChannelCtx *ch = &_channelCtx[c];

		ch->scale_factors = ch->saved_scale_factors[!ch->scale_factor_idx];
		int *sfEnd = ch->scale_factors + _numBands;

		if (ch->reuse_sf) {
			const int8 *sfOffsets = _sfOffsets[_tableIdx][ch->table_idx];
			for (int b = 0; b < _numBands; ++b)
				ch->scale_factors[b] =
					ch->saved_scale_factors[ch->scale_factor_idx][*sfOffsets++];
		}

		if (!ch->cur_subframe || _bs->getBit()) {
			if (!ch->reuse_sf) {
				int val;
				ch->scale_factor_step = _bs->getBits(2) + 1;
				val = 45 / ch->scale_factor_step;
				for (int *sf = ch->scale_factors; sf < sfEnd; ++sf) {
					val += (int32)_sfVlc->getSymbol(*_bs);
					*sf = val;
				}
			} else {
				for (int b = 0; b < _numBands; ++b) {
					uint32 idx = _sfRlVlc->getSymbol(*_bs);

					int val, sign, skip;
					if (!idx) {
						uint32 code = _bs->getBits(14);
						val  = (int)(code >> 6);
						sign = (int)(code & 1) - 1;
						skip = (int)((code & 0x3f) >> 1);
					} else if (idx == 1) {
						break;
					} else {
						skip = scale_rl_run[idx];
						val  = scale_rl_level[idx];
						sign = _bs->getBit() - 1;
					}

					b += skip;
					if (b >= _numBands) {
						warning("WMAProCodec: invalid scale factor coding");
						return -1;
					}
					ch->scale_factors[b] += (val ^ sign) - sign;
				}
			}

			ch->scale_factor_idx = !ch->scale_factor_idx;
			ch->table_idx        = _tableIdx;
			ch->reuse_sf         = 1;
		}

		ch->max_scale_factor = ch->scale_factors[0];
		for (int *sf = ch->scale_factors + 1; sf < sfEnd; ++sf) {
			if (*sf > ch->max_scale_factor)
				ch->max_scale_factor = *sf;
		}
	}

	return 0;
}

void WMAProCodec::inverseChannelTransform() {
	// From FFmpeg's inverse_channel_transform.
	for (int i = 0; i < _numChgroups; ++i) {
		ChannelGroup *cg = &_chgroup[i];
		if (!cg->transform)
			continue;

		float data[WMAPRO_MAX_CHANNELS];
		const int numChannels = cg->num_channels;
		float **chData = cg->channel_data;
		float **chEnd  = chData + numChannels;
		const int8 *tb = cg->transform_band;

		for (int16 *sfb = _curSfbOffsets;
		     sfb < _curSfbOffsets + _numBands; ++sfb) {
			if (*tb++ == 1) {
				int yEnd = MIN((int)sfb[1], (int)_subframeLen);
				for (int y = sfb[0]; y < yEnd; ++y) {
					const float *mat = cg->decorrelation_matrix;
					const float *dataEnd = data + numChannels;
					float *dataPtr = data;
					for (float **ch = chData; ch < chEnd; ++ch)
						*dataPtr++ = (*ch)[y];
					for (float **ch = chData; ch < chEnd; ++ch) {
						float sum = 0;
						dataPtr = data;
						while (dataPtr < dataEnd)
							sum += *dataPtr++ * *mat++;
						(*ch)[y] = sum;
					}
				}
			} else if (_nbChannels == 2) {
				int len = MIN((int)sfb[1], (int)_subframeLen) - sfb[0];
				const float mul = 181.0f / 128.0f;
				for (int k = 0; k < len; ++k) {
					cg->channel_data[0][sfb[0] + k] *= mul;
					cg->channel_data[1][sfb[0] + k] *= mul;
				}
			}
		}
	}
}

void WMAProCodec::wmaproWindow() {
	// From FFmpeg's wmapro_window.
	for (int i = 0; i < _channelsForCurSubframe; ++i) {
		int c = _channelIndexesForCurSubframe[i];
		int winlen = _channelCtx[c].prev_block_len;
		float *start = _channelCtx[c].coeffs - (winlen >> 1);

		if (_subframeLen < winlen) {
			start += (winlen - _subframeLen) >> 1;
			winlen = _subframeLen;
		}

		const float *window = _windows[wmaproLog2(winlen) - WMAPRO_BLOCK_MIN_BITS];
		int half = winlen >> 1;

		// In-place vector_fmul_window: dst = (src0 reversed * win0) - (src1 * win1)
		// FFmpeg's version operates on the same buffer (start is both src and dst).
		// The transformation crosses-multiplies a left and right half via the window.
		for (int k = 0; k < half; ++k) {
			float s0 = start[half - 1 - k];
			float s1 = start[half + k];
			float w0 = window[k];
			float w1 = window[half * 2 - 1 - k];
			start[k]                  = s0 * w1 - s1 * w0;
			start[2 * half - 1 - k]   = s0 * w0 + s1 * w1;
		}

		_channelCtx[c].prev_block_len = _subframeLen;
	}
}

int WMAProCodec::decodeSubframe() {
	// From FFmpeg's decode_subframe.
	int offset = _samplesPerFrame;
	int subframeLen = _samplesPerFrame;
	int totalSamples = _samplesPerFrame * _nbChannels;
	int transmitCoeffs = 0;

	_subframeOffset = _bs->pos();

	for (int i = 0; i < _nbChannels; ++i) {
		_channelCtx[i].grouped = 0;
		if (offset > _channelCtx[i].decoded_samples) {
			offset       = _channelCtx[i].decoded_samples;
			subframeLen  = _channelCtx[i].subframe_len[_channelCtx[i].cur_subframe];
		}
	}

	_channelsForCurSubframe = 0;
	for (int i = 0; i < _nbChannels; ++i) {
		int curSf = _channelCtx[i].cur_subframe;
		totalSamples -= _channelCtx[i].decoded_samples;
		if (offset == _channelCtx[i].decoded_samples &&
		    subframeLen == _channelCtx[i].subframe_len[curSf]) {
			totalSamples -= _channelCtx[i].subframe_len[curSf];
			_channelCtx[i].decoded_samples += _channelCtx[i].subframe_len[curSf];
			_channelIndexesForCurSubframe[_channelsForCurSubframe] = i;
			++_channelsForCurSubframe;
		}
	}

	if (!totalSamples)
		_parsedAllSubframes = 1;

	_tableIdx       = wmaproLog2(_samplesPerFrame / subframeLen);
	_numBands       = _numSfb[_tableIdx];
	_curSfbOffsets  = _sfbOffsets[_tableIdx];
	int curSubwooferCutoff = _subwooferCutoffs[_tableIdx];

	offset += _samplesPerFrame >> 1;

	for (int i = 0; i < _channelsForCurSubframe; ++i) {
		int c = _channelIndexesForCurSubframe[i];
		_channelCtx[c].coeffs = &_channelCtx[c].out[offset];
	}

	_subframeLen = subframeLen;
	_escLen      = wmaproLog2(_subframeLen - 1) + 1;

	// Skip extended header
	if (_bs->getBit()) {
		int numFillBits = _bs->getBits(2);
		if (!numFillBits) {
			int len = _bs->getBits(4);
			numFillBits = (len ? _bs->getBits(len) : 0) + 1;
		}
		if (numFillBits > 0) {
			if (_bs->pos() + numFillBits > _bsBits) {
				warning("WMAProCodec: invalid number of fill bits");
				return -1;
			}
			_bs->skip(numFillBits);
		}
	}

	if (_bs->getBit()) {
		warning("WMAProCodec: reserved bit set, unsupported");
		return -1;
	}

	if (decodeChannelTransform() < 0)
		return -1;

	for (int i = 0; i < _channelsForCurSubframe; ++i) {
		int c = _channelIndexesForCurSubframe[i];
		_channelCtx[c].transmit_coefs = _bs->getBit();
		if (_channelCtx[c].transmit_coefs)
			transmitCoeffs = 1;
	}

	if (transmitCoeffs) {
		int step;
		int quantStep = 90 * _bitsPerSample >> 4;

		_transmitNumVecCoeffs = _bs->getBit();
		if (_transmitNumVecCoeffs) {
			int numBits = wmaproLog2((_subframeLen + 3) / 4) + 1;
			for (int i = 0; i < _channelsForCurSubframe; ++i) {
				int c = _channelIndexesForCurSubframe[i];
				int numVecCoeffs = _bs->getBits(numBits) << 2;
				if (numVecCoeffs > _subframeLen) {
					warning("WMAProCodec: num_vec_coeffs %d too large", numVecCoeffs);
					return -1;
				}
				_channelCtx[c].num_vec_coeffs = numVecCoeffs;
			}
		} else {
			for (int i = 0; i < _channelsForCurSubframe; ++i) {
				int c = _channelIndexesForCurSubframe[i];
				_channelCtx[c].num_vec_coeffs = _subframeLen;
			}
		}

		// Sign-extended 6-bit value.
		int raw = _bs->getBits(6);
		step = (int)((uint32)raw << 26) >> 26;
		quantStep += step;
		if (step == -32 || step == 31) {
			const int sign = (step == 31) - 1;
			int quant = 0;
			while ((int)_bs->pos() + 5 < (int)_bsBits &&
			       (step = _bs->getBits(5)) == 31) {
				quant += 31;
			}
			quantStep += ((quant + step) ^ sign) - sign;
		}

		if (_channelsForCurSubframe == 1) {
			_channelCtx[_channelIndexesForCurSubframe[0]].quant_step = quantStep;
		} else {
			int modifierLen = _bs->getBits(3);
			for (int i = 0; i < _channelsForCurSubframe; ++i) {
				int c = _channelIndexesForCurSubframe[i];
				_channelCtx[c].quant_step = quantStep;
				if (_bs->getBit()) {
					if (modifierLen)
						_channelCtx[c].quant_step += _bs->getBits(modifierLen) + 1;
					else
						++_channelCtx[c].quant_step;
				}
			}
		}

		if (decodeScaleFactors() < 0)
			return -1;
	}

	for (int i = 0; i < _channelsForCurSubframe; ++i) {
		int c = _channelIndexesForCurSubframe[i];
		if (_channelCtx[c].transmit_coefs &&
		    (int)_bs->pos() < (int)_bsBits) {
			decodeCoeffs(c);
		} else {
			memset(_channelCtx[c].coeffs, 0,
			       sizeof(*_channelCtx[c].coeffs) * subframeLen);
		}
	}

	if (transmitCoeffs) {
		Math::MDCT *mdct = _mdct[wmaproLog2(subframeLen) - WMAPRO_BLOCK_MIN_BITS];
		inverseChannelTransform();
		for (int i = 0; i < _channelsForCurSubframe; ++i) {
			int c = _channelIndexesForCurSubframe[i];
			const int *sf = _channelCtx[c].scale_factors;

			if (c == _lfeChannel)
				memset(&_tmpBuf[curSubwooferCutoff], 0,
				       sizeof(*_tmpBuf) * (subframeLen - curSubwooferCutoff));

			for (int b = 0; b < _numBands; ++b) {
				int end = MIN((int)_curSfbOffsets[b + 1], (int)_subframeLen);
				int exp = _channelCtx[c].quant_step -
				          (_channelCtx[c].max_scale_factor - *sf++) *
				              _channelCtx[c].scale_factor_step;
				float quant = powf(10.0f, exp / 20.0f);
				int start = _curSfbOffsets[b];
				for (int k = start; k < end; ++k)
					_tmpBuf[k] = _channelCtx[c].coeffs[k] * quant;
			}

			mdct->calcIMDCT(_channelCtx[c].coeffs, _tmpBuf);
		}
	}

	wmaproWindow();

	for (int i = 0; i < _channelsForCurSubframe; ++i) {
		int c = _channelIndexesForCurSubframe[i];
		if (_channelCtx[c].cur_subframe >= _channelCtx[c].num_subframes) {
			warning("WMAProCodec: broken subframe");
			return -1;
		}
		++_channelCtx[c].cur_subframe;
	}

	return 0;
}

int WMAProCodec::decodeFrameInternal(int16 *output) {
	// From FFmpeg's decode_frame.
	int len = 0;

	if (_lenPrefix)
		len = _bs->getBits(_log2FrameSize);

	if (decodeTilehdr() < 0) {
		_packetLoss = 1;
		return 0;
	}

	if (_nbChannels > 1 && _bs->getBit()) {
		if (_bs->getBit()) {
			for (int i = 0; i < _nbChannels * _nbChannels; ++i)
				_bs->skip(4);
		}
	}

	if (_dynamicRangeCompression) {
		_drcGain = _bs->getBits(8);
	}

	if (_bs->getBit()) {
		if (_bs->getBit())
			_trimStart = _bs->getBits(wmaproLog2(_samplesPerFrame * 2));
		if (_bs->getBit())
			_trimEnd = _bs->getBits(wmaproLog2(_samplesPerFrame * 2));
	} else {
		_trimStart = _trimEnd = 0;
	}

	_parsedAllSubframes = 0;
	for (int i = 0; i < _nbChannels; ++i) {
		_channelCtx[i].decoded_samples = 0;
		_channelCtx[i].cur_subframe    = 0;
		_channelCtx[i].reuse_sf        = 0;
	}

	while (!_parsedAllSubframes) {
		if (decodeSubframe() < 0) {
			_packetLoss = 1;
			return 0;
		}
	}

	// Interleave channels and convert float -> int16.
	int gotSamples = 0;
	if (!_skipFrame) {
		for (int s = 0; s < _samplesPerFrame; ++s) {
			for (int c = 0; c < _nbChannels; ++c) {
				float f = _channelCtx[c].out[s];
				int v = (int)(f * 32768.0f);
				if (v > 32767) v = 32767;
				else if (v < -32768) v = -32768;
				output[s * _nbChannels + c] = (int16)v;
			}
		}
		gotSamples = _samplesPerFrame;
	} else {
		_skipFrame = 0;
	}

	// Shift IMDCT overlap buffer for next frame.
	for (int c = 0; c < _nbChannels; ++c) {
		memcpy(&_channelCtx[c].out[0],
		       &_channelCtx[c].out[_samplesPerFrame],
		       (size_t)_samplesPerFrame * sizeof(_channelCtx[c].out[0]) >> 1);
	}

	if (_lenPrefix) {
		int consumed = (int)_bs->pos() - _frameOffset;
		int skip = len - consumed - 1;
		if (skip < 0) {
			// xWMA in XACT may not actually emit a length prefix even
			// though decode_flags says so; or we have a parse bug
			// upstream.  Don't bail -- we still got samples out of
			// decodeSubframe.  Log once for diagnostics.
			static bool warned = false;
			if (!warned) {
				warning("WMAProCodec: lenPrefix mismatch len=%d consumed=%d (returning samples anyway)",
				        len, consumed);
				warned = true;
			}
		} else if (skip > 0) {
			_bs->skip(skip);
		}
	} else {
		while ((int)_bs->pos() < (int)_numSavedBits && _bs->getBit() == 0) { }
	}

	int moreFrames = _bs->getBit();
	++_frameNum;
	(void)moreFrames;
	return gotSamples;
}

int WMAProCodec::decodePacket(const uint8 *data, int size, int16 *output) {
	// xWMA packet structure (per FFmpeg's decode_packet):
	//   - 4 bits: packet_sequence_number
	//   - 2 bits: skipped
	//   - log2_frame_size bits: num_bits_prev_frame
	//   - rest: frame bitstream (assuming num_bits_prev_frame == 0)
	//
	// We don't implement the cross-packet bit reservoir, so we only
	// handle the case where the frame starts cleanly in this packet
	// (num_bits_prev_frame == 0).
	if (!data || size <= 0)
		return 0;

	Common::MemoryReadStream ms(data, size, DisposeAfterUse::NO);
	Common::BitStream8MSB bs(ms);
	_bs           = &bs;
	_bsBits       = size * 8;
	_numSavedBits = _bsBits;
	_packetLoss   = 0;

	// Packet header.
	uint32 seqNum = _bs->getBits(4);
	_bs->skip(2);
	uint32 numBitsPrevFrame = _bs->getBits(_log2FrameSize);

	(void)seqNum;

	// num_bits_prev_frame is the count of bits at the start of this
	// packet that belong to the tail of the *previous* frame.  Since
	// we don't maintain a cross-packet bit reservoir, we skip those
	// bits and start decoding fresh from where they end -- a fresh
	// frame should begin at exactly that point.
	if (numBitsPrevFrame > 0) {
		if ((int)_bs->pos() + (int)numBitsPrevFrame > (int)_bsBits) {
			_bs = nullptr;
			return 0;
		}
		_bs->skip(numBitsPrevFrame);
	}

	_frameOffset = (int)_bs->pos();
	int produced = decodeFrameInternal(output);
	_bs = nullptr;
	return produced;
}

} // End of namespace Audio
