#!/bin/sh
# Tests for the cdcstore half of `zfrag mount` (cdcview.c and its routing in mount.c): an archive that holds a
# cdcstore repository shows native/ and virtual/, and virtual/ serves the repository's files from the chunk
# files inside the archive. Needs the cdcstore programs (cdcstore-add and cdcstore-list); without them the
# suite is skipped.
#   usage: CDCSTORE=/path/to/cdcstore/programs sh tests/cdc_test.sh [path/to/zfrag]

Z=${1:-./build/zfrag}
[ -x "$Z" ] || { echo "no executable $Z" >&2; exit 2; }
Z=$(cd "$(dirname "$Z")" && pwd)/$(basename "$Z")
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
if [ -z "$CDCSTORE" ] || [ ! -x "$CDCSTORE/cdcstore-add" ] || [ ! -x "$CDCSTORE/cdcstore-list" ]; then
  echo "skip - CDCSTORE (the folder with cdcstore-add and cdcstore-list) is not set: cdc view not tested"
  echo "all tests passed"
  exit 0
fi
CDCSTORE=$(cd "$CDCSTORE" && pwd)

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
cd "$T" || exit 2
fails=0
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }

${CC:-cc} -std=gnu99 -O2 -pthread -DMJPEGMOUNT_AS_LIBRARY -I"$ROOT" -c -o mjpegmount.o "$ROOT/mjpegmount.c" 2> cc0.err || { cat cc0.err; echo "cannot build mjpegmount.c"; exit 2; }
${CC:-cc} -std=gnu99 -Wall -Wextra -O2 -pthread ${HARNESS_CFLAGS:-} -DFUSE_USE_VERSION=31 -I"$HERE/fuse_stub" -I"$ROOT" \
  -o glue_harness "$HERE/glue_harness.c" "$ROOT/archive.c" "$ROOT/fcache.c" "$ROOT/tree.c" "$ROOT/tar.c" "$ROOT/cdcview.c" "$ROOT/mkvview.c" mjpegmount.o "$ROOT/threads.c" \
  ${HARNESS_LIBS:--lzstd -lxxhash} 2> cc.err || { cat cc.err; echo "cannot build tests/glue_harness.c"; exit 2; }
G=./glue_harness

# ---- a source tree, stored in a repository ----
mkdir -p src/sub/deep src/games
head -c 300000 /dev/urandom > src/big.bin
head -c 100 /dev/urandom > src/small; chmod 640 src/small
: > src/empty
head -c 70000 /dev/urandom > src/sub/deep/mid
head -c 200000 /dev/urandom > src/games/pak0
ln -s big.bin src/lnk
touch -d '2001-02-03 04:05:06 UTC' src/small
"$CDCSTORE/cdcstore-add" -q -d repo src > /dev/null 2>&1 || { echo "cdcstore-add failed"; exit 2; }
{ echo repo/cdcstore.idx; "$CDCSTORE/cdcstore-list" -k repo | grep -vx repo/cdcstore.idx; } > list.txt   # the index first, once (newer cdcstore-list -k prints it itself)
"$Z" pack -q -T list.txt -o r.tar.zfrag || { echo "pack failed"; exit 2; }

# 1. the two halves
[ "$($G r.tar.zfrag readdir / | tr '\n' ' ')" = "native virtual " ] || [ "$($G r.tar.zfrag readdir / | tr '\n' ' ')" = ". .. native virtual " ] \
  && ok "the top has native and virtual" || fail "top: $($G r.tar.zfrag readdir /)"
$G r.tar.zfrag readdir /native/repo | grep -q '^cdcstore.idx$' && $G r.tar.zfrag readdir /native/repo | grep -q '^chunks$' \
  && ok "native/ holds the archive's own files (the index and chunks/)" || fail "native listing"
[ "$($G r.tar.zfrag readdir /virtual | grep -v '^\.' | tr '\n' ' ')" = "src " ] && ok "virtual/ holds the stored files, not the chunk files" || fail "virtual listing: $($G r.tar.zfrag readdir /virtual)"
[ "$($G r.tar.zfrag readdir /virtual/src | grep -v '^\.' | sort | tr '\n' ' ')" = "big.bin empty games lnk small sub " ] && ok "a folder lists its entries" || fail "src listing: $($G r.tar.zfrag readdir /virtual/src)"
[ "$($G r.tar.zfrag getattr /nope)" = "-ENOENT" ] && [ "$($G r.tar.zfrag getattr /virtual/src/nope)" = "-ENOENT" ] && [ "$($G r.tar.zfrag getattr /native/nope)" = "-ENOENT" ] \
  && ok "missing paths are -ENOENT in every half" || fail "ENOENT"
[ "$($G r.tar.zfrag readdir /virtual/src/big.bin)" = "-ENOTDIR" ] && ok "readdir of a file is -ENOTDIR" || fail "ENOTDIR"

# 2. attributes
$G r.tar.zfrag getattr /virtual/src/big.bin | grep -q '^file mode=444 .* size=300000 ' && ok "a file has its size and is read-only" || fail "getattr big.bin: $($G r.tar.zfrag getattr /virtual/src/big.bin)"
$G r.tar.zfrag getattr /virtual/src/small | grep -q 'mode=440 .* size=100 .* mtime=981173106$' && ok "mode (640 read-only: 440) and mtime are kept" || fail "getattr small: $($G r.tar.zfrag getattr /virtual/src/small)"
$G r.tar.zfrag getattr /virtual/src/empty | grep -q 'size=0 ' && ok "an empty file is empty" || fail "empty"
$G r.tar.zfrag getattr /virtual/src/sub/deep | grep -q '^dir ' && ok "a folder is a folder" || fail "dir"
[ "$($G r.tar.zfrag readlink /virtual/src/lnk)" = "big.bin" ] && ok "a link reads back its target" || fail "readlink: $($G r.tar.zfrag readlink /virtual/src/lnk)"
[ "$($G r.tar.zfrag readlink /virtual/src/big.bin)" = "-EINVAL" ] && ok "readlink of a file is -EINVAL" || fail "readlink of file"
[ "$($G r.tar.zfrag open /virtual/src/big.bin wr)" = "-EACCES" ] && ok "writing is refused" || fail "write open"

# 3. contents: whole files, ranges across chunk boundaries, the end
for f in big.bin small empty sub/deep/mid games/pak0; do
  sz=$(wc -c < "src/$f")
  $G r.tar.zfrag read "/virtual/src/$f" 0 $((sz + 10)) > got.bin
  cmp -s "src/$f" got.bin || fail "$f differs"
done
ok "every file reads back identical to the original"
for spec in "0 1" "100 70000" "16383 3" "65535 65538" "299990 100" "299999 5" "300000 10"; do
  set -- $spec
  tail -c +$(($1 + 1)) src/big.bin | head -c "$2" > want.bin
  $G r.tar.zfrag read /virtual/src/big.bin "$1" "$2" > got.bin
  cmp -s want.bin got.bin || fail "range $1+$2 differs"
  $G r.tar.zfrag read /virtual/src/big.bin "$1" "$2" nofh > got2.bin
  cmp -s want.bin got2.bin || fail "range $1+$2 differs without a handle"
done
ok "ranges across chunk boundaries and at the end are right, with and without a handle"

# 4. a repository at the top of the archive (--strip-components), and one chunk missing from the archive
"$Z" pack -q -T list.txt -o top.tar.zfrag --strip-components 1 || fail "pack with strip"
$G top.tar.zfrag read /virtual/src/small 0 100 > got.bin; cmp -s src/small got.bin && ok "a repository at the top of the archive is found too" || fail "top-level repository"
grep -v '/c000000001$' list.txt > part.txt
"$Z" pack -q -T part.txt -o part.tar.zfrag
bad=0; for f in big.bin small sub/deep/mid games/pak0; do
  $G part.tar.zfrag read "/virtual/src/$f" 0 1000000 > got.bin; cmp -s "src/$f" got.bin || bad=$((bad + 1)); done
[ "$bad" -ge 1 ] && ok "a chunk file missing from the archive makes reads of the file(s) it belongs to fail, not return wrong bytes" || fail "missing chunk went unnoticed"

# 5. an archive without a repository is unchanged: its files are at the top
printf 'x' > plain.txt; echo plain.txt > plain.lst
"$Z" pack -q -T plain.lst -o plain.tar.zfrag
[ "$($G plain.tar.zfrag readdir / | grep -v '^\.' | tr '\n' ' ')" = "plain.txt " ] && ok "an archive with no repository has its files at the top" || fail "plain archive"

# 6. a damaged index is refused with a reason
head -c 64 /dev/zero > badidx; printf 'CDCSTOR1' | dd of=badidx conv=notrunc 2>/dev/null
mkdir -p dmg; cp badidx dmg/cdcstore.idx; echo dmg/cdcstore.idx > dmg.lst; "$Z" pack -q -T dmg.lst -o dmg.tar.zfrag
$G dmg.tar.zfrag readdir / | grep -q 'CDC FAILED' && ok "a damaged index is refused with a reason" || fail "damaged index: $($G dmg.tar.zfrag readdir /)"

[ "$fails" -eq 0 ] && echo "all tests passed" || { echo "$fails failed"; exit 1; }
