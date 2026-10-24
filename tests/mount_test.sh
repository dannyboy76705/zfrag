#!/bin/sh
# Tests for the mount's model: the directory tree built from an archive, reads through the
# decoded-frame cache, archives packed with --strip-components, and many threads at once.
# No FUSE and no root are needed: tests/tree_harness.c drives the same functions the mount calls.
#   usage: sh tests/mount_test.sh [path/to/zfrag]            (default ./build/zfrag)
# Needs a C compiler with libzstd and libxxhash (set HARNESS_CFLAGS / HARNESS_LIBS if they are not
# in the default places), tar, cmp, find (GNU), stat (GNU). Everything is in a temporary directory.

Z=${1:-./build/zfrag}
[ -x "$Z" ] || { echo "no executable $Z" >&2; exit 2; }
Z=$(cd "$(dirname "$Z")" && pwd)/$(basename "$Z")
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
cd "$T" || exit 2

fails=0
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }

${CC:-cc} -std=gnu99 -O2 -pthread ${HARNESS_CFLAGS:-} -I"$ROOT" -o tree_harness \
  "$HERE/tree_harness.c" "$ROOT/archive.c" "$ROOT/fcache.c" "$ROOT/tree.c" "$ROOT/tar.c" \
  ${HARNESS_LIBS:--lzstd -lxxhash} 2> cc.err || { cat cc.err; echo "cannot build tests/tree_harness.c"; exit 2; }
H=./tree_harness
TMO=; command -v timeout >/dev/null 2>&1 && TMO="timeout 180"

# ---- a source tree with the awkward cases ----
mkdir -p src/odd/a src/docs src/games/doom src/games/keen src/many "src/with space"
printf '1' > src/odd/a-1; printf '2' > src/odd/a.txt; printf '3' > src/odd/a/b; printf '4' > src/odd/a0; printf '5' > "src/odd/a b"
head -c 700000 /dev/urandom > src/games/doom/doom.wad
cp src/games/doom/doom.wad src/games/keen/doom-copy.wad
head -c 123456 /dev/urandom > src/games/keen/keen.exe; chmod 755 src/games/keen/keen.exe
head -c 5 /dev/urandom > src/docs/tiny.txt; : > src/docs/empty.txt; seq 1 200000 > src/docs/numbers.txt
printf 'unicode\n' > "$(printf 'src/docs/caf\303\251.txt')"
printf 'hello\n' > "src/with space/a b.txt"
longdir=src/$(printf 'verylongdirectoryname%.0s' 1 2 3 4 5 6 7)/$(printf 'x%.0s' 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30)
mkdir -p "$longdir"; head -c 300000 /dev/urandom > "$longdir/deep.bin"
i=1; while [ $i -le 300 ]; do head -c $((100 + i * 7)) /dev/urandom > "src/many/f$(printf %03d $i)"; i=$((i + 1)); done
touch -d '2001-02-03 04:05:06 UTC' src/docs/tiny.txt src/games/keen/keen.exe
find src -type f | sort > list.txt
nfiles=$(wc -l < list.txt)

sort -r list.txt > rev.txt
"$Z" pack -T list.txt -o tf.tar.zfrag --frame 64K --level 3 -q && "$Z" pack -T rev.txt -o tn.tar.zfrag --frame 64K --level 3 -q \
  && "$Z" pack -T list.txt -o ts.tar.zfrag --frame 64K --level 3 --strip-components 1 -q || { echo "cannot make the test archives"; exit 2; }

want_walk() { while read -r f; do stat -c '%s %a %Y' "$f" | tr -d '\n'; printf ' /%s\n' "$f"; done < list.txt | LC_ALL=C sort; }
want_walk > want.walk

# 1. the whole tree: every file, with its size, mode and mtime
$H tf.tar.zfrag walk | LC_ALL=C sort > got.walk
cmp -s want.walk got.walk && ok "walk lists all $nfiles files with the right size, mode and mtime " || fail "walk differs: $(diff want.walk got.walk | head -3)"
$H tn.tar.zfrag walk | LC_ALL=C sort | cmp -s - want.walk && ok "the same tree from an archive packed from the reversed list (stream order does not matter)" || fail "the reversed-list archive gives a different tree"
[ "$($H tf.tar.zfrag stats | sed 's/.*files=\([0-9]*\) .*/\1/')" = "$nfiles" ] && ok "stats count $nfiles files" || fail "file count: $($H tf.tar.zfrag stats)"

# 2. directory listings, in name order, for every directory (including 'many', 300 entries)
find src -type d | LC_ALL=C sort > dirs.txt
bad=0
while IFS= read -r d; do
  find "$d" -mindepth 1 -maxdepth 1 -printf '%f\t%y\t%s\n' | LC_ALL=C sort -t "$(printf '\t')" -k1,1 \
    | awk -F'\t' '{ if ($2 == "d") print "d " $1; else print "f " $1 " " $3 }' > want.ls
  $H tf.tar.zfrag ls "/$d" > got.ls
  cmp -s want.ls got.ls || { fail "listing of /$d differs: $(diff want.ls got.ls | head -2)"; bad=1; }
done < dirs.txt
[ "$bad" -eq 0 ] && ok "every directory lists exactly its entries in name order ($(wc -l < dirs.txt) directories, including one with spaces)"
$H tf.tar.zfrag ls /src/odd | tr '\n' '|' | grep -q '^d a|f a b 1|f a-1 1|f a.txt 1|f a0 1|$' && ok "names that sort around '/' (a b, a-1, a.txt, a/ , a0) are listed and found" || fail "odd names: $($H tf.tar.zfrag ls /src/odd | tr '\n' '|')"
[ "$($H tf.tar.zfrag read /src/odd/a/b 0 all)" = "3" ] && ok "a file inside a directory that shares a prefix with sibling files is found" || fail "/src/odd/a/b not readable"

# 3. stat: directories, files, and what is not there
$H tf.tar.zfrag stat / | grep -q '^dir 555 ' && $H tf.tar.zfrag stat /src/games | grep -q '^dir 555 ' && ok "directories are 0555" || fail "directory stat: $($H tf.tar.zfrag stat /src/games)"
[ "$($H tf.tar.zfrag stat /src/games/keen/keen.exe)" = "file 755 123456 $(stat -c %Y src/games/keen/keen.exe)" ] && ok "a file reports its stored mode, size and mtime" || fail "file stat: $($H tf.tar.zfrag stat /src/games/keen/keen.exe)"
[ "$($H tf.tar.zfrag stat /src/nope)" = "ENOENT" ] && [ "$($H tf.tar.zfrag stat /src/docs/tiny.txt/x)" = "ENOENT" ] && [ "$($H tf.tar.zfrag ls /src/docs/tiny.txt)" = "ENOTDIR" ] \
  && ok "a missing path is ENOENT, a path through a file too, and ls of a file is ENOTDIR" || fail "missing-path handling"

# 4. reads: every file whole, and ranges across frame boundaries (frames are 64 KiB)
bad=0
while read -r f; do
  $H tf.tar.zfrag read "/$f" 0 all > got.bin 2>/dev/null && cmp -s got.bin "$f" || { fail "content of $f differs"; bad=1; break; }
done < list.txt
[ "$bad" -eq 0 ] && ok "all $nfiles files read back byte for byte"
bad=0
for r in "0 1" "65535 2" "65536 65536" "131071 70000" "699999 1" "699999 100" "700000 10" "700001 10" "0 700000" "350000 123456"; do
  off=${r% *}; len=${r#* }
  $H tf.tar.zfrag read /src/games/doom/doom.wad $off $len > got.bin
  tail -c +$((off + 1)) src/games/doom/doom.wad | head -c $len | cmp -s - got.bin || { fail "range off=$off len=$len differs"; bad=1; }
done
[ "$bad" -eq 0 ] && ok "ranges across frame boundaries, at the end of the file and past it are all right"
[ "$($H tf.tar.zfrag read /src/docs/empty.txt 0 all | wc -c)" -eq 0 ] && ok "an empty file reads as empty" || fail "empty file"
$H tf.tar.zfrag read /src/docs /dev/null 0 2>/dev/null | head -c0; $H tf.tar.zfrag read /src/docs 0 10 > /dev/null 2> rd.err && fail "read of a directory succeeded" || ok "reading a directory fails"
$H tf.tar.zfrag read /src/nope 0 10 > /dev/null 2> rd.err && fail "read of a missing file succeeded" || grep -q ENOENT rd.err && ok "reading a missing file is ENOENT"

# 5. an archive packed with --strip-components: the tree is the contents, so a mount at the
#    original directory name puts every file back where the list had it
sed 's#^src/##' list.txt > want.stripped.list
while read -r f; do stat -c '%s %a %Y' "src/$f" | tr -d '\n'; printf ' /%s\n' "$f"; done < want.stripped.list | LC_ALL=C sort > want.walk.s
$H ts.tar.zfrag walk | LC_ALL=C sort | cmp -s - want.walk.s && ok "a --strip-components 1 archive shows the contents of src/ at the top, with no prefix and no option" || fail "stripped tree differs"
[ "$($H ts.tar.zfrag stat /games/doom/doom.wad | cut -d' ' -f1)" = "file" ] && [ "$($H ts.tar.zfrag stat /src)" = "ENOENT" ] && ok "/games/doom/doom.wad exists and /src does not" || fail "stripped lookups"
$H ts.tar.zfrag read /games/keen/keen.exe 0 all | cmp -s - src/games/keen/keen.exe && ok "and its files read back correctly" || fail "stripped read"

# 6. a path listed twice shows once; a plain tar.zst is refused
{ cat list.txt; echo src/docs/tiny.txt; } > dup.list
"$Z" pack -T dup.list -o dup.tar.zfrag --frame 64K --level 3 -q 2>/dev/null
$H dup.tar.zfrag stats | grep -q 'hidden=1' && [ "$($H dup.tar.zfrag walk | grep -c ' /src/docs/tiny.txt$')" -eq 1 ] && ok "a path stored twice is shown once and counted as hidden" || fail "duplicate path: $($H dup.tar.zfrag stats)"
ioff=$(tail -c 40 tf.tar.zfrag | od -An -tu8 -j16 -N8 | tr -d ' '); head -c "$ioff" tf.tar.zfrag > plain.tar.zst
$H plain.tar.zst walk | grep -q '^OPEN FAILED: not a zfrag archive' && ok "a plain tar.zst is refused with 'not a zfrag archive'" || fail "plain tar.zst: $($H plain.tar.zst walk | head -1)"

# 7. the frame cache: with room for every frame, each frame is decoded exactly once, however the files are read
out=$($H -c 64 tf.tar.zfrag readall); misses=$(echo "$out" | sed -n 's/.*misses=\([0-9]*\) .*/\1/p'); frames=$(echo "$out" | sed -n 's/.*frames=\([0-9]*\) .*/\1/p')
[ -n "$misses" ] && [ "$misses" -eq "$frames" ] && ok "with room for every frame, each of the $frames frames is decoded once (reading every file)" || fail "cache misses $misses for $frames frames: $out"
out=$($H -c 0 tf.tar.zfrag readall); echo "$out" | grep -q "read $nfiles files" && echo "$out" | grep -q 'slots=2' && ok "with the minimum cache (2 frames) everything still reads correctly" || fail "minimum cache: $out"

# 8. many threads at once, with the minimum cache (constant eviction) and with a roomy one
for cfg in "0 1" "64 2" "0 3"; do
  mib=${cfg% *}; seed=${cfg#* }
  $TMO $H -c $mib tf.tar.zfrag stress 8 2500 $seed > stress.out 2>&1; rc=$?
  grep -q 'mismatches=0 errors=0' stress.out && [ $rc -eq 0 ] && ok "8 threads x 2500 random reads, cache $mib MiB: every read correct, no deadlock" || fail "stress (cache $mib MiB, rc $rc): $(cat stress.out | head -2)"
done


# 9. the automatic cache: one decoded frame per thread, never fewer than 4
for pair in "8 8" "2 4" "1 4" "16 16" "4 4"; do
  th=${pair% *}; want=${pair#* }
  got=$($H -c auto -t $th tf.tar.zfrag stats | sed 's/.*slots=\([0-9]*\) .*/\1/')
  [ "$got" = "$want" ] || { fail "-c auto with $th threads: $got slots, expected $want"; bada=1; }
done
[ -z "$bada" ] && ok "automatic cache: 8 threads -> 8 frames, 16 -> 16, and a floor of 4 frames for 1 or 2 threads (one component clip needs three)"
[ "$($H -c 64 -t 2 tf.tar.zfrag stats | sed 's/.*slots=\([0-9]*\) .*/\1/')" = "1024" ] && ok "an explicit cache size wins over the automatic one (64 MiB of 64K frames = 1024 slots)" || fail "explicit cache size"
for th in 1 2 8; do
  $TMO $H -c auto -t $th tf.tar.zfrag stress 8 1500 7 > stress.out 2>&1
  grep -q 'mismatches=0 errors=0' stress.out || { fail "stress with automatic cache and $th decoding threads: $(head -1 stress.out)"; badt=1; }
done
[ -z "$badt" ] && ok "8 reader threads on an automatic cache with 1, 2 and 8 decoding threads: every read correct, no deadlock (decoders are shared and waited for)"

if [ "$fails" -eq 0 ]; then echo "all tests passed"; exit 0; fi
echo "$fails test(s) failed"
exit 1
