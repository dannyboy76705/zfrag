#!/bin/sh
# Tests for the mjpegstore half of `zfrag mount` (mkvview.c, and mjpegmount.c compiled as a library): an archive that
# holds an mjpegstore set shows native/ and virtual/, and virtual/ has one <clip>.mkv per clip, byte for byte what
# `mjpegstore -s` writes (but for the writing-app name in the header, as with mjpegmount). Needs mjpegstore (to make the pictures and the reference stream); without it the suite is skipped.
#   usage: MJPEGSTORE=/path/to/mjpegstore sh tests/mkv_test.sh [path/to/zfrag]

Z=${1:-./build/zfrag}
[ -x "$Z" ] || { echo "no executable $Z" >&2; exit 2; }
Z=$(cd "$(dirname "$Z")" && pwd)/$(basename "$Z")
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
if [ -z "$MJPEGSTORE" ] || [ ! -x "$MJPEGSTORE" ]; then
  echo "skip - MJPEGSTORE (the mjpegstore program) is not set: mkv view not tested"
  echo "all tests passed"
  exit 0
fi
MJPEGSTORE=$(cd "$(dirname "$MJPEGSTORE")" && pwd)/$(basename "$MJPEGSTORE")

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
cd "$T" || exit 2
fails=0
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }

${CC:-cc} -std=gnu99 -O2 -pthread -DMJPEGMOUNT_AS_LIBRARY -I"$ROOT" -c -o mjpegmount.o "$ROOT/mjpegmount.c" 2> cc0.err || { cat cc0.err; echo "cannot build mjpegmount.c"; exit 2; }
if [ -s cc0.err ]; then fail "mjpegmount.c as a library compiles with warnings: $(cat cc0.err)"; else ok "mjpegmount.c compiles as a library"; fi
${CC:-cc} -std=gnu99 -Wall -Wextra -O2 -pthread ${HARNESS_CFLAGS:-} -DFUSE_USE_VERSION=31 -I"$HERE/fuse_stub" -I"$ROOT" \
  -o glue_harness "$HERE/glue_harness.c" "$ROOT/archive.c" "$ROOT/fcache.c" "$ROOT/tree.c" "$ROOT/tar.c" "$ROOT/cdcview.c" "$ROOT/mkvview.c" mjpegmount.o "$ROOT/threads.c" \
  ${HARNESS_LIBS:--lzstd -lxxhash} 2> cc.err || { cat cc.err; echo "cannot build tests/glue_harness.c"; exit 2; }
G=./glue_harness

# ---- a set of two clips: a synthetic y4m stored with mjpegstore -i ----
mkdir -p set
cat > set/specs.txt <<'EOS'
resolution=64x48
sampling=420
framerate=24000:1001
bitdepth=8
chromasiting=jpeg
aspect=1:1
quality=90
range=limited
EOS
python3 - <<'EOP'
w, h = 64, 48
for name, n, shift in (("a.y4m", 30, 0), ("b.y4m", 53, 7)):
    with open(name, "wb") as f:
        f.write(b"YUV4MPEG2 W64 H48 F24000:1001 Ip A1:1 C420jpeg\n")
        for i in range(n):
            f.write(b"FRAME\n")
            f.write(bytes((x * 3 + i * 2 + yy + shift) % 256 for yy in range(h) for x in range(w)))
            f.write(bytes((x + i) % 256 for yy in range(h // 2) for x in range(w // 2)))
            f.write(bytes((yy + i + shift) % 256 for yy in range(h // 2) for x in range(w // 2)))
EOP
{ "$MJPEGSTORE" -i a.y4m set/ep1 && "$MJPEGSTORE" -i b.y4m set/ep2; } > ingest.log 2>&1 || { echo "mjpegstore -i failed:"; tail -n 8 ingest.log; exit 2; }
"$MJPEGSTORE" -s set/ep1 > ep1.ref 2> /dev/null; "$MJPEGSTORE" -s set/ep2 > ep2.ref 2> /dev/null
{ echo set/specs.txt; find set -name '*.jpg' | sort -V; } > list.txt
"$Z" pack -q -T list.txt -o s.tar.zfrag --strip-components 1 --frame 64K || { echo "pack failed"; exit 2; }
# the only difference from mjpegmount's stream is the writing-app name ("mjpegstore" or "mjpegmount", same length) in the header
LC_ALL=C sed -i 's/mjpegstore/mjpegmount/g' ep1.ref ep2.ref
sz1=$(wc -c < ep1.ref); sz2=$(wc -c < ep2.ref)

# 1. the two halves
[ "$($G s.tar.zfrag readdir / | grep -v '^\.' | tr '\n' ' ')" = "native virtual " ] && ok "the top has native and virtual" || fail "top: $($G s.tar.zfrag readdir /)"
$G s.tar.zfrag readdir /native | grep -q '^specs.txt$' && $G s.tar.zfrag readdir /native | grep -q '^ep1$' && ok "native/ holds the archive's own files" || fail "native listing"
[ "$($G s.tar.zfrag readdir /virtual | grep -v '^\.' | tr '\n' ' ')" = "ep1.mkv ep2.mkv " ] && ok "virtual/ has one .mkv per clip, in natural order" || fail "virtual listing: $($G s.tar.zfrag readdir /virtual)"
$G s.tar.zfrag getattr /virtual/ep1.mkv | grep -q "^file mode=444 .* size=$sz1 " && $G s.tar.zfrag getattr /virtual/ep2.mkv | grep -q "^file mode=444 .* size=$sz2 " \
  && ok "each clip is a read-only file of exactly the stream's size ($sz1, $sz2)" || fail "sizes: $($G s.tar.zfrag getattr /virtual/ep1.mkv)"
$G s.tar.zfrag getattr /virtual | grep -q '^dir ' && [ "$($G s.tar.zfrag getattr /virtual/nope.mkv)" = "-ENOENT" ] && [ "$($G s.tar.zfrag getattr /virtual/ep1)" = "-ENOENT" ] \
  && ok "virtual/ is a folder; a missing clip is -ENOENT" || fail "virtual getattr"
[ "$($G s.tar.zfrag readdir /virtual/ep1.mkv)" = "-ENOTDIR" ] && ok "readdir of a clip is -ENOTDIR" || fail "ENOTDIR"
[ "$($G s.tar.zfrag open /virtual/ep1.mkv wr)" = "-EACCES" ] && ok "writing is refused" || fail "write open"

# 2. contents: byte for byte what mjpegstore -s writes
$G s.tar.zfrag read /virtual/ep1.mkv 0 $((sz1 + 100)) > got1.mkv; $G s.tar.zfrag read /virtual/ep2.mkv 0 $((sz2 + 100)) > got2.mkv
cmp -s ep1.ref got1.mkv && cmp -s ep2.ref got2.mkv && ok "both clips read back identical to mjpegstore -s (apart from the app name)" || fail "stream differs"
for spec in "0 1" "1 700" "4000 9000" "$((sz1 - 50)) 200" "$((sz1 - 1)) 5" "$sz1 10"; do
  set -- $spec
  tail -c +$(($1 + 1)) ep1.ref | head -c "$2" > want.bin
  $G s.tar.zfrag read /virtual/ep1.mkv "$1" "$2" > got.bin; cmp -s want.bin got.bin || fail "range $1+$2 differs"
  $G s.tar.zfrag read /virtual/ep1.mkv "$1" "$2" nofh > got.bin; cmp -s want.bin got.bin || fail "range $1+$2 differs without a handle"
done
ok "ranges over the header, between pictures, at the end and past it are right"

# 3. native is untouched, and a plain archive is unchanged
$G s.tar.zfrag read /native/specs.txt 0 1000 | cmp -s - set/specs.txt && ok "native/ files read back as stored" || fail "native read"
printf 'x' > plain.txt; echo plain.txt > plain.lst; "$Z" pack -q -T plain.lst -o plain.tar.zfrag
[ "$($G plain.tar.zfrag readdir / | grep -v '^\.' | tr '\n' ' ')" = "plain.txt " ] && ok "an archive with no set has its files at the top" || fail "plain archive"

# 4. a picture missing from the archive: the clip is refused or its reads fail, never wrong bytes
grep -v 'frame-0000010.jpg' list.txt > part.txt; "$Z" pack -q -T part.txt -o part.tar.zfrag --strip-components 1 --frame 64K
$G part.tar.zfrag read /virtual/ep1.mkv 0 $((sz1 + 100)) > got.mkv 2> /dev/null
cmp -s ep1.ref got.mkv && fail "a missing picture went unnoticed" || ok "a missing picture does not give the full, correct stream"

# 5. a set with no usable specs.txt is refused with a reason
grep -v 'specs.txt' list.txt > nospecs.txt; "$Z" pack -q -T nospecs.txt -o nospecs.tar.zfrag --strip-components 1
[ "$($G nospecs.tar.zfrag readdir / | grep -v '^\.' | tr '\n' ' ')" != "native virtual " ] && ok "frames without a specs.txt are not taken for a set" || fail "no specs.txt"

[ "$fails" -eq 0 ] && echo "all tests passed" || { echo "$fails failed"; exit 1; }
