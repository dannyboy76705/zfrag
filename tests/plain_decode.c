/* SPDX-License-Identifier: MIT */
/* Decodes a .zst file to stdout with zstd's streaming decoder alone: no zfrag code.
 * It stands in for `zstd -dc FILE` when the zstd command is not installed: it must
 * read the concatenated frames and skip the skippable ones (the index and trailer). */
#include <stdio.h>
#include <stdlib.h>
#include <zstd.h>

int main(int argc, char **argv)
{
	static char in[1 << 17], out[1 << 17];
	FILE *f;
	ZSTD_DCtx *d = ZSTD_createDCtx();
	size_t n, last = 0;

	if (argc != 2 || !(f = fopen(argv[1], "rb")) || !d) { fprintf(stderr, "usage: plain_decode FILE.zst\n"); return 2; }
	while ((n = fread(in, 1, sizeof in, f)) > 0) {
		ZSTD_inBuffer ib = { in, n, 0 };

		while (ib.pos < ib.size) {
			ZSTD_outBuffer ob = { out, sizeof out, 0 };

			last = ZSTD_decompressStream(d, &ob, &ib);
			if (ZSTD_isError(last)) { fprintf(stderr, "plain_decode: %s\n", ZSTD_getErrorName(last)); return 1; }
			if (ob.pos && fwrite(out, 1, ob.pos, stdout) != ob.pos) return 1;
		}
	}
	if (last != 0) { fprintf(stderr, "plain_decode: the stream ended inside a frame\n"); return 1; }
	return 0;
}
