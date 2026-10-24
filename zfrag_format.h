/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * zfrag_format.h - the archive format.
 *
 * An archive is a tar stream cut into independent zstd frames of one fixed
 * uncompressed size (the last one may be shorter), followed by an index in a
 * zstd skippable frame and a small trailer, also a skippable frame:
 *
 *   [zstd frame 0][zstd frame 1] ... [zstd frame N-1][index frame][trailer frame]
 *
 * Plain `zstd -d` skips the skippable frames and yields the tar stream, so
 * `zstd -dc set.tar.zfrag | tar x` works with no zfrag installed (-c, because the
 * zstd command line tool does not know the .zfrag suffix and needs to be told where
 * to write). A reader that
 * knows the format reads the trailer (always the last ZF_TRAILER_SIZE bytes),
 * which points at the index, which gives every frame's offset (a frame is found
 * by arithmetic: stream offset / frame size) and every file's position.
 *
 * Every frame is compressed with the window set to the frame size, so a frame
 * never needs more than ZF_MAX_FRAME_RAW bytes of output buffer.
 *
 * All integers are little-endian. Tables are fixed-size, 8-byte aligned and
 * searchable in place, so a reader needs no setup work (and a kernel reader
 * needs no allocation beyond the index itself).
 */
#ifndef ZFRAG_FORMAT_H
#define ZFRAG_FORMAT_H

/* Smallest and largest uncompressed frame. The cap bounds the output buffer
 * every decode needs (one per concurrent reader, which is what matters in the
 * kernel), and with it the zstd window: a frame of up to 2^25 bytes is
 * compressed with windowLog <= 25, well under zstd's default decoder limit of
 * 2^27, so plain `zstd -d` needs no special flags. */
#define ZF_MIN_FRAME_RAW    (64u * 1024u)
#define ZF_MAX_FRAME_RAW    (32u * 1024u * 1024u)
#define ZF_MAX_WINDOW_LOG   25
#define ZF_DEFAULT_FRAME    ZF_MAX_FRAME_RAW

/* zstd skippable frames: a 4-byte magic in 0x184D2A50..0x184D2A5F, a 4-byte
 * payload length, then the payload. */
#define ZF_SKIP_INDEX       0x184D2A5Eu
#define ZF_SKIP_TRAILER     0x184D2A5Fu
#define ZF_SKIP_PAD         0x184D2A5Du     /* padding: the payload is zeros, see below */

/* The trailer is the very last frame: 8 bytes of frame header and a 32-byte
 * payload, so a reader finds it at (file size - ZF_TRAILER_SIZE).
 *   payload: "ZFRAGv01" (8) | index frame offset u64 | index payload length u64 |
 *            xxh64 of the index payload u64 */
#define ZF_TRAILER_MAGIC    "ZFRAGv01"
#define ZF_TRAILER_PAYLOAD  32
#define ZF_TRAILER_SIZE     (8 + ZF_TRAILER_PAYLOAD)

/* Padding. A block device holds whole sectors, and a loop device over a file drops a partial last
 * sector, which would drop the trailer with it. So the size of the whole archive is made a multiple
 * of ZF_ALIGN by a padding frame (ZF_SKIP_PAD, zeros) between the index frame and the trailer, which
 * leaves the trailer the last 40 bytes. A reader therefore accepts, between the end of the index
 * frame and the trailer, any number of whole skippable frames: the padding frame, and the old
 * trailer too in an archive that `zfrag pad` has padded by appending. A reader that skips skippable
 * frames (zstd does) needs to know nothing about it. */
#define ZF_ALIGN            4096u

/* The index payload:
 *   header (ZF_IDX_HDR bytes)
 *      0  "ZFIDX001"
 *      8  u32 version (1)       12  u32 zstd level it was packed with
 *                              (informational; 0 = not recorded; readers must not depend on it)
 *     16  u64 frame size        24  u64 stream length (the tar stream, bytes)
 *     32  u64 number of frames  40  u64 number of files
 *     48  u64 length of the names pool   56  u64 reserved (0)
 *   frame table: (frames + 1) u64: the file offset of each zstd frame, then
 *                the offset just past the last one
 *   file table: files * ZF_REC_SIZE bytes, sorted by name (bytewise)
 *   names pool: the names, concatenated, not NUL-terminated
 *
 * A file record:
 *      0  u64 data_off  where the file's data starts in the tar stream
 *      8  u64 size
 *     16  i64 mtime
 *     24  u32 mode      28  u32 hdr_len  (the header blocks start at data_off - hdr_len)
 *     32  u32 name_off  36  u32 name_len */
#define ZF_INDEX_MAGIC      "ZFIDX001"
#define ZF_INDEX_VERSION    1
#define ZF_IDX_HDR          64
#define ZF_REC_SIZE         40

#define ZF_TAR_BLOCK        512

/* The name of an archive. It is deliberately not .tar.zst: the file is readable by
 * zstd, but it is not an ordinary tar.zst (it ends with an index and cannot be
 * recognised or mounted by tools that expect a plain one), and the name keeps the
 * two apart. `zfrag pack` requires it. */
#define ZF_SUFFIX           ".tar.zfrag"

#endif /* ZFRAG_FORMAT_H */
