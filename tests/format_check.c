/* SPDX-License-Identifier: MIT */
/* The format constants agree with each other. */
#include <stdio.h>
#include <string.h>
#include "zfrag_format.h"

static int fails;

#define CHECK(cond) do { \
	if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } \
} while (0)

int main(void)
{
	/* the cap is 32 MiB and its window is exactly as large */
	CHECK(ZF_MAX_FRAME_RAW == 32u * 1024u * 1024u);
	CHECK(((unsigned long long)1 << ZF_MAX_WINDOW_LOG) == ZF_MAX_FRAME_RAW);
	CHECK((ZF_MIN_FRAME_RAW & (ZF_MIN_FRAME_RAW - 1)) == 0);
	CHECK((ZF_MAX_FRAME_RAW & (ZF_MAX_FRAME_RAW - 1)) == 0);
	CHECK(ZF_MIN_FRAME_RAW < ZF_MAX_FRAME_RAW);
	CHECK(ZF_MAX_WINDOW_LOG <= 27);
	/* the skippable magics are in zstd's skippable range and differ */
	CHECK((ZF_SKIP_INDEX & 0xFFFFFFF0u) == 0x184D2A50u);
	CHECK((ZF_SKIP_TRAILER & 0xFFFFFFF0u) == 0x184D2A50u);
	CHECK(ZF_SKIP_INDEX != ZF_SKIP_TRAILER);
	CHECK((ZF_SKIP_PAD & 0xFFFFFFF0u) == 0x184D2A50u);              /* a skippable frame: zstd tools skip it */
	CHECK(ZF_SKIP_PAD != ZF_SKIP_INDEX && ZF_SKIP_PAD != ZF_SKIP_TRAILER);
	CHECK(ZF_ALIGN % 512 == 0 && (ZF_ALIGN & (ZF_ALIGN - 1)) == 0);  /* whole loop-device sectors, a power of two */
	/* magics have their stated lengths; tables stay 8-byte aligned */
	CHECK(strlen(ZF_TRAILER_MAGIC) == 8 && strlen(ZF_INDEX_MAGIC) == 8);
	CHECK(strncmp(ZF_SUFFIX, ".tar.", 5) == 0 && strcmp(ZF_SUFFIX, ".tar.zst") != 0);
	CHECK(ZF_TRAILER_SIZE == 40 && ZF_IDX_HDR % 8 == 0 && ZF_REC_SIZE % 8 == 0);

	if (fails) {
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("format ok\n");
	return 0;
}
