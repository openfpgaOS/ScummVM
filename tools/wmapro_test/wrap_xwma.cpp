// wrap_xwma -- given raw xWMA bytes from an XACT XWB entry, write an
// RIFF/XWMA file that ffmpeg can decode.
//
// We don't know nBlockAlign authoritatively for XACT XWB.  Pass it on
// the command line and we try it; if ffmpeg decodes cleanly the value
// is right.
//
// Usage:
//   wrap_xwma <in.raw> <out.xwm> <rate> <channels> <blockAlign> <avgBytesPerSec>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>

static void w32le(FILE *f, uint32_t v) {
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24)};
	fwrite(b, 1, 4, f);
}
static void w16le(FILE *f, uint16_t v) {
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v>>8)};
	fwrite(b, 1, 2, f);
}
static void wbytes(FILE *f, const char *s) {
	fwrite(s, 1, 4, f);
}

int main(int argc, char **argv) {
	if (argc < 7) {
		fprintf(stderr, "usage: %s <in.raw> <out.xwm> <rate> <channels> <blockAlign> <avgBytesPerSec>\n", argv[0]);
		return 1;
	}

	FILE *in = fopen(argv[1], "rb");
	if (!in) { perror(argv[1]); return 1; }
	fseek(in, 0, SEEK_END);
	long inSize = ftell(in);
	fseek(in, 0, SEEK_SET);
	std::vector<uint8_t> data(inSize);
	fread(data.data(), 1, inSize, in);
	fclose(in);

	int rate    = atoi(argv[3]);
	int ch      = atoi(argv[4]);
	int balign  = atoi(argv[5]);
	int bps     = atoi(argv[6]);

	// dpds: one packet seek-table entry per packet.  We don't have the
	// real values; ffmpeg uses them for seek but tolerates monotonic
	// guesses.  We'll fill linearly: packet N -> N * samplesPerPacket.
	int samplesPerPacket = 2048; // canonical WMA Pro frame
	int numPackets       = (int)((data.size() + balign - 1) / balign);
	std::vector<uint32_t> dpds(numPackets);
	for (int i = 0; i < numPackets; ++i)
		dpds[i] = (uint32_t)((i + 1) * samplesPerPacket * ch * 2);

	FILE *out = fopen(argv[2], "wb");
	if (!out) { perror(argv[2]); return 2; }

	// fmt chunk for WMA Pro (with KSDATAFORMAT_SUBTYPE_XWMA_AUDIO would
	// be WAVEFORMATEXTENSIBLE; simpler tag 0x0162 form is enough for ffmpeg).
	int cbSize    = 0;
	int fmtSize   = 18 + cbSize;
	int dpdsSize  = (int)dpds.size() * 4;
	int dataSize  = (int)data.size();
	int riffSize  = 4    // "XWMA"
	              + 8 + fmtSize
	              + 8 + dpdsSize
	              + 8 + dataSize;

	wbytes(out, "RIFF");
	w32le(out, riffSize);
	wbytes(out, "XWMA");

	wbytes(out, "fmt ");
	w32le(out, fmtSize);
	w16le(out, 0x0162);              // wFormatTag = WMA Pro
	w16le(out, (uint16_t)ch);
	w32le(out, (uint32_t)rate);
	w32le(out, (uint32_t)bps);
	w16le(out, (uint16_t)balign);
	w16le(out, 16);                  // wBitsPerSample
	w16le(out, 0);                   // cbSize

	wbytes(out, "dpds");
	w32le(out, dpdsSize);
	fwrite(dpds.data(), 4, dpds.size(), out);

	wbytes(out, "data");
	w32le(out, dataSize);
	fwrite(data.data(), 1, dataSize, out);

	fclose(out);

	fprintf(stderr, "wrote %s: rate=%d ch=%d balign=%d bps=%d packets=%d size=%d\n",
	        argv[2], rate, ch, balign, bps, numPackets, dataSize);
	return 0;
}
