/*
 * wmapro_test -- standalone PC unit test for the ScummVM WMA Pro decoder.
 *
 * Pulls one xWMA entry out of an XACT XWB wave bank, runs it through
 * our Audio::WMAProCodec, and writes the decoded PCM to a .raw file
 * suitable for diffing against ffmpeg's reference output.
 *
 * Usage:
 *   wmapro_test <wave_bank.xwb> <entry_index> <out.raw>
 *
 * Companion script: ./regress.sh produces both ours and ffmpeg's
 * outputs and diffs them.
 */

// Forbid-list bypass: the test program uses real fopen/fwrite/etc
// for input/output.  Must be before any ScummVM header.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <vector>
#include <string>

#include "common/scummsys.h"
#include "common/memstream.h"
#include "common/stream.h"
#include "audio/audiostream.h"
#include "audio/decoders/wmapro.h"

// --- XWB segment IDs (subset, from FFmpeg) ---
enum {
	kXWBSegmentBankData       = 0,
	kXWBSegmentEntryMetaData  = 1,
	kXWBSegmentSeekTables     = 2,
	kXWBSegmentEntryNames     = 3,
	kXWBSegmentEntryWaveData  = 4
};

struct XwbSegment {
	uint32_t offset;
	uint32_t length;
};

struct XwbEntry {
	uint32_t flagsAndDuration;
	uint32_t format;
	uint32_t offsetInWaveData;   // relative to kXWBSegmentEntryWaveData
	uint32_t length;
	uint32_t loopOffset;
	uint32_t loopLength;

	// Unpacked format fields
	int codec;     // 0=PCM 1=XMA 2=ADPCM 3=WMA
	int channels;
	int rate;
	int align;
	int bits;
};

static uint32_t readLE32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t readBE32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static std::vector<uint8_t> readFile(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return {}; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> buf(sz);
	if (fread(buf.data(), 1, sz, f) != (size_t)sz) {
		perror("fread");
		buf.clear();
	}
	fclose(f);
	return buf;
}

static bool parseXwb(const std::vector<uint8_t> &xwb, std::vector<XwbEntry> &entries,
                     int wantedIndex, XwbSegment &waveSeg) {
	if (xwb.size() < 52) {
		fprintf(stderr, "XWB too small\n");
		return false;
	}

	// XWB header (WBND signature, version, etc.)
	uint32_t magic = readBE32(xwb.data());
	if (magic != 0x57424E44) {  // 'WBND'
		fprintf(stderr, "Not a WBND wave bank (magic=0x%08x)\n", magic);
		return false;
	}
	uint32_t version = readLE32(xwb.data() + 4);
	(void)version;
	// Header version follows but FFmpeg's XACT code skips ahead

	// FFmpeg's indexXWBFile reads 5 segments starting at offset 12
	// (after magic + version + header_version).
	XwbSegment segments[5] = {};
	for (int i = 0; i < 5; ++i) {
		segments[i].offset = readLE32(xwb.data() + 12 + i * 8);
		segments[i].length = readLE32(xwb.data() + 16 + i * 8);
	}

	// Bank-data segment: flags, entry count, name (64 bytes), entrySize
	const uint8_t *bd = xwb.data() + segments[kXWBSegmentBankData].offset;
	uint32_t flags        = readLE32(bd + 0);
	uint32_t entryCount   = readLE32(bd + 4);
	uint32_t entrySize    = readLE32(bd + 4 + 64);
	(void)entrySize;
	if (flags & 0x00020000) {
		fprintf(stderr, "XWB compact format not supported\n");
		return false;
	}

	// Entry metadata: 24 bytes per entry (flagsAndDur, format, offset, length, loopOff, loopLen)
	const uint8_t *em = xwb.data() + segments[kXWBSegmentEntryMetaData].offset;
	entries.resize(entryCount);
	for (uint32_t i = 0; i < entryCount; ++i) {
		const uint8_t *e = em + i * 24;
		entries[i].flagsAndDuration = readLE32(e + 0);
		entries[i].format           = readLE32(e + 4);
		entries[i].offsetInWaveData = readLE32(e + 8);
		entries[i].length           = readLE32(e + 12);
		entries[i].loopOffset       = readLE32(e + 16);
		entries[i].loopLength       = readLE32(e + 20);

		uint32_t fmt = entries[i].format;
		entries[i].codec    = fmt & 0x3;
		entries[i].channels = (fmt >> 2) & 0x7;
		entries[i].rate     = (fmt >> 5) & 0x3FFFF;
		entries[i].align    = (fmt >> 23) & 0xFF;
		entries[i].bits     = (fmt >> 31) & 0x1;
	}

	waveSeg = segments[kXWBSegmentEntryWaveData];

	if (wantedIndex >= (int)entryCount) {
		fprintf(stderr, "wanted index %d but bank has only %u entries\n",
		        wantedIndex, entryCount);
		return false;
	}

	fprintf(stderr, "[xwb] entries=%u\n", entryCount);
	for (uint32_t i = 0; i < entryCount && i < 8; ++i) {
		fprintf(stderr, "  [%u] codec=%d ch=%d rate=%d align=%d bits=%d "
		                "offset=%u length=%u\n",
		        i, entries[i].codec, entries[i].channels, entries[i].rate,
		        entries[i].align, entries[i].bits,
		        entries[i].offsetInWaveData, entries[i].length);
	}

	return true;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <xwb> <entry_index> <out.raw>\n", argv[0]);
		return 1;
	}
	const char *xwbPath = argv[1];
	int entryIdx = atoi(argv[2]);
	const char *outPath = argv[3];

	auto xwb = readFile(xwbPath);
	if (xwb.empty()) return 2;

	std::vector<XwbEntry> entries;
	XwbSegment waveSeg;
	if (!parseXwb(xwb, entries, entryIdx, waveSeg)) return 3;

	const XwbEntry &e = entries[entryIdx];
	if (e.codec != 3) {
		fprintf(stderr, "entry %d codec is %d, not WMA\n", entryIdx, e.codec);
		return 4;
	}

	const uint8_t *data = xwb.data() + waveSeg.offset + e.offsetInWaveData;
	int dataSize = (int)e.length;
	fprintf(stderr, "[xwb] entry %d data: offset=%u length=%d\n",
	        entryIdx, waveSeg.offset + e.offsetInWaveData, dataSize);

	// Hand the data to our decoder.
	Common::MemoryReadStream stream(data, dataSize, DisposeAfterUse::NO);

	Audio::WMAProCodec *codec =
		new Audio::WMAProCodec((uint32)e.rate,
		                       (uint8)(e.channels ? e.channels : 1),
		                       /*bitRate=*/0,
		                       /*blockAlign=*/(uint32)e.align);

	Audio::AudioStream *as = codec->decodeFrame(stream);
	if (!as) {
		fprintf(stderr, "decoder returned nullptr (decode failed)\n");
		delete codec;
		return 5;
	}

	// Read all samples out of the returned stream.
	FILE *out = fopen(outPath, "wb");
	if (!out) { perror(outPath); delete as; delete codec; return 6; }

	int16_t buf[4096];
	int total = 0;
	int isStereo = as->isStereo() ? 2 : 1;
	int rate     = as->getRate();
	while (!as->endOfData()) {
		int n = as->readBuffer(buf, 4096);
		if (n <= 0) break;
		fwrite(buf, sizeof(int16_t), n, out);
		total += n;
	}
	fclose(out);

	fprintf(stderr, "[ok] wrote %d samples (%.3f sec) of %d-ch %d Hz s16le to %s\n",
	        total, (double)total / isStereo / rate, isStereo, rate, outPath);

	delete as;
	delete codec;
	return 0;
}
