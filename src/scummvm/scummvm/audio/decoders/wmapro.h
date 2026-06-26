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

// Based on FFmpeg's libavcodec/wmaprodec.c (LGPL).  This is a port of
// the WMA Pro / xWMA decoder for ScummVM, used to decode the .xwb wave
// banks shipped with XACT-based games (Monkey Island 1/2 SE and others).

#ifndef AUDIO_DECODERS_WMAPRO_H
#define AUDIO_DECODERS_WMAPRO_H

#include "common/scummsys.h"
#include "common/bitstream.h"
#include "audio/decoders/codec.h"

namespace Common {
class SeekableReadStream;
template<class BITSTREAM> class Huffman;
}

namespace Math {
class MDCT;
}

namespace Audio {

class AudioStream;

/**
 * WMA Pro (xWMA) decoder.
 *
 * Class structure mirrors FFmpeg's libavcodec/wmaprodec.c so that
 * future syncs from upstream are tractable.
 *
 * Translation status: state + init scaffolding in place; the decoder
 * helpers (decodeFrameInternal, decodeSubframe, decodeCoeffs,
 * decodeScaleFactors, decodeChannelTransform, decodeTilehdr, decodeSubframeLength,
 * decodeDecorrelationMatrix) are stubs that return error until the
 * port body lands.  Until then, decodeFrame() returns nullptr and the
 * SoundSE caller silently skips xWMA cues.
 */
class WMAProCodec : public Codec {
public:
	WMAProCodec(uint32 sampleRate, uint8 channels, uint32 bitRate,
	            uint32 blockAlign,
	            Common::SeekableReadStream *extraData = nullptr);
	~WMAProCodec() override;

	AudioStream *decodeFrame(Common::SeekableReadStream &data) override;

private:
	// --- Constants (mirrors FFmpeg #defines) -------------------------------

	enum {
		WMAPRO_MAX_CHANNELS    = 8,
		MAX_SUBFRAMES          = 32,
		MAX_BANDS              = 29,
		MAX_FRAMESIZE          = 32768,
		WMAPRO_BLOCK_MIN_BITS  = 6,
		WMAPRO_BLOCK_MAX_BITS  = 13,
		WMAPRO_BLOCK_MIN_SIZE  = 1 << WMAPRO_BLOCK_MIN_BITS,
		WMAPRO_BLOCK_MAX_SIZE  = 1 << WMAPRO_BLOCK_MAX_BITS,
		WMAPRO_BLOCK_SIZES     = WMAPRO_BLOCK_MAX_BITS - WMAPRO_BLOCK_MIN_BITS + 1
	};

	// --- Per-channel decode state (was WMAProChannelCtx) -------------------

	struct ChannelCtx {
		int16  prev_block_len;
		uint8  transmit_coefs;
		uint8  num_subframes;
		uint16 subframe_len[MAX_SUBFRAMES];
		uint16 subframe_offset[MAX_SUBFRAMES];
		uint8  cur_subframe;
		uint16 decoded_samples;
		uint8  grouped;
		int    quant_step;
		int8   reuse_sf;
		int8   scale_factor_step;
		int    max_scale_factor;
		int    saved_scale_factors[2][MAX_BANDS];
		int8   scale_factor_idx;
		int   *scale_factors;
		uint8  table_idx;
		float *coeffs;
		uint16 num_vec_coeffs;
		float  out[WMAPRO_BLOCK_MAX_SIZE + WMAPRO_BLOCK_MAX_SIZE / 2];
	};

	// --- Channel-group state (was WMAProChannelGrp) ------------------------

	struct ChannelGroup {
		uint8  num_channels;
		int8   transform;
		int8   transform_band[MAX_BANDS];
		float  decorrelation_matrix[WMAPRO_MAX_CHANNELS * WMAPRO_MAX_CHANNELS];
		float *channel_data[WMAPRO_MAX_CHANNELS];
	};

	typedef Common::Huffman<Common::BitStream8MSB> HuffmanDecoder;

	// --- Setup-time state --------------------------------------------------

	uint32 _sampleRate;
	uint8  _channels;
	uint32 _bitRate;
	uint32 _blockAlign;

	bool   _initialized;          ///< extradata parsed successfully

	uint32 _decodeFlags;
	uint8  _lenPrefix;
	uint8  _dynamicRangeCompression;
	uint8  _bitsPerSample;
	uint16 _samplesPerFrame;
	uint16 _trimStart;
	uint16 _trimEnd;
	uint16 _log2FrameSize;
	int8   _lfeChannel;
	uint8  _maxNumSubframes;
	uint8  _subframeLenBits;
	uint8  _maxSubframeLenBit;
	uint16 _minSamplesPerSubframe;

	int8   _numSfb[WMAPRO_BLOCK_SIZES];
	int16  _sfbOffsets[WMAPRO_BLOCK_SIZES][MAX_BANDS];
	int8   _sfOffsets[WMAPRO_BLOCK_SIZES][WMAPRO_BLOCK_SIZES][MAX_BANDS];
	int16  _subwooferCutoffs[WMAPRO_BLOCK_SIZES];

	// --- DSP infrastructure -----------------------------------------------

	Math::MDCT  *_mdct[WMAPRO_BLOCK_SIZES];
	float        _tmpBuf[WMAPRO_BLOCK_MAX_SIZE];
	const float *_windows[WMAPRO_BLOCK_SIZES];

	HuffmanDecoder *_sfVlc;       ///< scale factor DPCM vlc
	HuffmanDecoder *_sfRlVlc;     ///< scale factor run length vlc
	HuffmanDecoder *_vec4Vlc;     ///< 4-coefficient vector vlc
	HuffmanDecoder *_vec2Vlc;     ///< 2-coefficient vector vlc
	HuffmanDecoder *_vec1Vlc;     ///< 1-coefficient vector vlc
	HuffmanDecoder *_coefVlc[2];  ///< coefficient run-length vlc
	float           _sin64[33];   ///< sine table for decorrelation

	// --- Packet / frame state ---------------------------------------------

	uint8  _frameData[MAX_FRAMESIZE + 64];     ///< compressed bit reservoir
	int    _numSavedBits;
	int    _frameOffset;
	int    _subframeOffset;
	uint8  _packetOffset;
	uint8  _packetSequenceNumber;
	uint8  _packetLoss;
	uint8  _packetDone;
	uint32 _frameNum;
	int    _bufBitSize;
	uint8  _drcGain;
	int8   _skipFrame;
	int8   _parsedAllSubframes;

	// --- Current bitstream (set per-decodeFrame, scratch) -----------------

	Common::BitStream8MSB *_bs;   ///< current GetBitContext (FFmpeg s->gb)
	uint32                 _bsBits;///< total bits available in _bs

	// --- Subframe / block state -------------------------------------------

	int16  _subframeLen;
	int8   _nbChannels;
	int8   _channelsForCurSubframe;
	int8   _channelIndexesForCurSubframe[WMAPRO_MAX_CHANNELS];
	int8   _numBands;
	int8   _transmitNumVecCoeffs;
	int16 *_curSfbOffsets;
	uint8  _tableIdx;
	int8   _escLen;
	uint8  _numChgroups;

	ChannelGroup _chgroup[WMAPRO_MAX_CHANNELS];
	ChannelCtx   _channelCtx[WMAPRO_MAX_CHANNELS];

	// --- Init helpers (decode_init in FFmpeg) -----------------------------

	bool initFromExtradata(Common::SeekableReadStream *extra);
	void initVlcTables();
	void initMdctsAndWindows();
	void initSfbOffsets();

	// --- Decoder helpers (decode_* in FFmpeg) -----------------------------
	// Stubs today; bodies translated in follow-up commits.

	int decodeSubframeLength(int offset);
	int decodeTilehdr();
	void decodeDecorrelationMatrix(ChannelGroup *cg);
	int decodeChannelTransform();
	int decodeCoeffs(int c);
	int decodeScaleFactors();
	void inverseChannelTransform();
	void wmaproWindow();
	int decodeSubframe();
	int decodeFrameInternal(int16 *output);
	int decodePacket(const uint8 *data, int size, int16 *output);
};

} // End of namespace Audio

#endif // AUDIO_DECODERS_WMAPRO_H
