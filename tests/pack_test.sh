#!/bin/sh
# Tests for `zfrag pack` and the read side (cat, ls, extract, verify).
#   usage: sh tests/pack_test.sh [path/to/zfrag]      (default ./build/zfrag)
# Needs a POSIX shell, tar, diff, cmp, dd, head, stat (GNU) and a C compiler with libzstd
# (for the plain-decoder check; if `zstd` is installed that is used instead). Everything
# happens in a temporary directory that is removed on exit.
#
# The system tar is the judge: the archive's stream must read and extract with it, and the
# result must be byte-identical to the source.

Z=${1:-./build/zfrag}
[ -x "$Z" ] || { echo "no executable $Z" >&2; exit 2; }
Z=$(cd "$(dirname "$Z")" && pwd)/$(basename "$Z")
HERE=$(cd "$(dirname "$0")" && pwd)

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
cd "$T" || exit 2

fails=0
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }

# a plain decoder with no zfrag code in it: `zstd -dc` if there is one, else a small stand-in
if command -v zstd >/dev/null 2>&1; then
  plain() { zstd -dc "$1"; }; plainname="zstd -dc"
elif ${CC:-cc} ${PLAIN_CFLAGS:-} -O2 -o plain_decode "$HERE/plain_decode.c" ${PLAIN_LIBS:--lzstd} 2>/dev/null; then
  plain() { ./plain_decode "$1"; }; plainname="the streaming decoder stand-in"
else
  plain() { return 1; }; plainname=""
fi

# ---- the source tree: awkward names, a long path, duplicates, odd modes and times ----
mkdir -p src/games/doom src/games/keen src/docs "src/with space"
longdir=src/$(printf 'verylongdirectoryname%.0s' 1 2 3 4 5 6 7)/$(printf 'x%.0s' 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30)
mkdir -p "$longdir"
head -c 700000 /dev/urandom > src/games/doom/doom.wad
cp src/games/doom/doom.wad src/games/keen/doom-copy.wad
head -c 123456 /dev/urandom > src/games/keen/keen.exe; chmod 755 src/games/keen/keen.exe
head -c 5 /dev/urandom > src/docs/tiny.txt
: > src/docs/empty.txt
printf 'hello\n' > "src/with space/a b.txt"
printf 'unicode\n' > "$(printf 'src/docs/caf\303\251.txt')"
head -c 300000 /dev/urandom > "$longdir/deep.bin"
seq 1 200000 > src/docs/numbers.txt
touch -d '2001-02-03 04:05:06 UTC' src/docs/tiny.txt src/games/keen/keen.exe
find src -type f | sort > list.txt
nfiles=$(wc -l < list.txt)

"$Z" pack -T list.txt -o a.tar.zfrag --frame 64K --level 3 -j 2 -q && ok "pack ran" || fail "pack failed"
[ ! -e a.tar.zfrag.tmp ] && ok "no temporary file left after a successful pack" || fail "a.tar.zfrag.tmp left behind"

# 1. the real tar reads and extracts it
n=$("$Z" cat a.tar.zfrag | tar -tf - | wc -l)
[ "$n" -eq "$nfiles" ] && ok "tar lists all $nfiles files" || fail "tar lists $n files, expected $nfiles"
rm -rf o1 && mkdir o1 && "$Z" cat a.tar.zfrag | tar -xf - -C o1 && diff -r src o1/src > /dev/null \
  && ok "tar extracts a tree identical to the source (long path, spaces, empty file)" || fail "tar extraction differs"
[ "$(stat -c %a o1/src/games/keen/keen.exe)" = "755" ] && ok "permissions kept" || fail "permissions lost"
[ "$(stat -c %Y o1/src/docs/tiny.txt)" = "$(stat -c %Y src/docs/tiny.txt)" ] && ok "modification times kept" || fail "mtime lost"
"$Z" cat a.tar.zfrag | tar -tvf - | head -1 | grep -q ' 0/0 ' && ok "ownership is stored as 0/0" || fail "ownership not 0/0"

# 2. plain streaming decode, no zfrag code
if [ -n "$plainname" ]; then
  rm -rf o2 && mkdir o2 && plain a.tar.zfrag | tar -xf - -C o2 && diff -r src o2/src > /dev/null \
    && ok "plain decode ($plainname) reads the archive; index and trailer are skipped" || fail "plain decode failed"
  plain a.tar.zfrag > p.tar; "$Z" cat a.tar.zfrag > z.tar; cmp -s p.tar z.tar && ok "plain decode equals zfrag cat" || fail "plain decode differs from cat"
else
  echo "skip - no zstd command and no compiler/libzstd for the stand-in: plain decode not tested"
fi

# 3. zfrag extract: everything, one file, a directory
rm -rf o3 && mkdir o3 && "$Z" extract -q -C o3 a.tar.zfrag && diff -r src o3/src > /dev/null && ok "zfrag extract of everything is identical" || fail "extract differs"
rm -rf o4 && mkdir o4 && "$Z" extract -q -C o4 a.tar.zfrag src/games/keen/keen.exe src/docs/ \
  && cmp -s o4/src/games/keen/keen.exe src/games/keen/keen.exe && [ "$(find o4 -type f | wc -l)" -eq 5 ] \
  && [ "$(stat -c %a o4/src/games/keen/keen.exe)" = "755" ] && ok "extract of one file and one directory prefix (5 files, mode kept)" || fail "partial extract wrong"
"$Z" extract -q -C o4 a.tar.zfrag no/such/file 2>/dev/null && fail "extract of a missing name succeeded" || ok "extract of a missing name fails"

# 4. verify and ls
"$Z" verify -q a.tar.zfrag -T list.txt && ok "verify passes, and the archive matches the files on disk" || fail "verify failed"
[ "$("$Z" ls a.tar.zfrag | wc -l)" -eq "$nfiles" ] && ok "ls lists $nfiles files" || fail "ls count wrong"

# 5. determinism and frame sizes
"$Z" pack -T list.txt -o j1.tar.zfrag --frame 64K --level 3 -j 1 -q; "$Z" pack -T list.txt -o j4.tar.zfrag --frame 64K --level 3 -j 4 -q
cmp -s j1.tar.zfrag j4.tar.zfrag && cmp -s j1.tar.zfrag a.tar.zfrag && ok "the archive is byte-identical for -j 1, 2 and 4" || fail "output depends on -j"
for f in 64K 1M 32M; do
  "$Z" pack -T list.txt -o f.tar.zfrag --frame $f --level 3 --force -q && "$Z" verify -q f.tar.zfrag -T list.txt \
    && "$Z" cat f.tar.zfrag | tar -tf - > /dev/null && ok "frame size $f round-trips" || fail "frame size $f failed"
done
"$Z" pack -T list.txt -o x.tar.zfrag --frame 33M -q 2>/dev/null && fail "--frame 33M accepted" || ok "--frame 33M refused (cap is 32M)"
"$Z" pack -T list.txt -o x.tar.zfrag --frame 1K -q 2>/dev/null && fail "--frame 1K accepted" || ok "--frame 1K refused (minimum is 64K)"

# 6. order: the list as written, always; zfrag does not reorder, sort or move duplicates
# (GNU tar escapes non-ASCII bytes when listing unless told to print names literally)
"$Z" cat a.tar.zfrag | tar --quoting-style=literal -tf - | diff - list.txt > /dev/null && ok "the archive holds the files in exactly the order of the list (duplicates included, none moved)" || fail "archive order differs from the list"
"$Z" pack -T list.txt -o x.tar.zfrag --no-dups -q 2>/dev/null && fail "--no-dups is still accepted" || ok "--no-dups is gone: an unknown option, refused"
# the u32 at offset 12 of the index header records the zstd level (a.tar.zfrag was packed with --level 3)
ioff=$(tail -c 40 a.tar.zfrag | od -An -tu8 -j16 -N8 | tr -d ' ')
[ "$(od -An -tu4 -j$((ioff + 20)) -N4 a.tar.zfrag | tr -d ' ')" = 3 ] && ok "the index records the level (3)" || fail "the index does not record the level"
"$Z" verify a.tar.zfrag 2>&1 | grep -q ', level 3' && ok "verify reports the level" || fail "verify does not report the level"

# 7. damaged archives are refused
head -c 1000000 a.tar.zfrag > cut.tar.zfrag; "$Z" ls cut.tar.zfrag 2>/dev/null && fail "a truncated archive was accepted" || ok "truncated archive refused"
cp a.tar.zfrag b1.tar.zfrag; printf '\377' | dd of=b1.tar.zfrag bs=1 seek=200000 conv=notrunc 2>/dev/null
"$Z" verify -q b1.tar.zfrag 2>/dev/null && fail "a flipped byte in a frame went unnoticed" || ok "a flipped byte in a frame is caught (frame checksum)"
# (an unpadded archive: 300 bytes before the end is inside the index, with no padding in the way)
"$Z" pack -T list.txt -o b2.tar.zfrag --frame 64K --level 3 -j 2 --no-pad -q; sz0=$(stat -c %s b2.tar.zfrag); printf '\377' | dd of=b2.tar.zfrag bs=1 seek=$((sz0 - 300)) conv=notrunc 2>/dev/null
"$Z" verify -q b2.tar.zfrag 2>/dev/null && fail "a flipped byte in the index went unnoticed" || ok "a flipped byte in the index is caught"
"$Z" ls list.txt 2>/dev/null && fail "a text file was accepted as an archive" || ok "a non-archive is refused"
# a normal tar.zst: just concatenated frames and no index (here: a zfrag archive cut at its index)
ioff=$(tail -c 40 a.tar.zfrag | od -An -tu8 -j16 -N8 | tr -d ' '); head -c "$ioff" a.tar.zfrag > plain.tar.zst
for cmd in ls verify cat; do
  "$Z" $cmd plain.tar.zst > /dev/null 2> plain.err && fail "zfrag $cmd accepted a plain tar.zst" || \
    { grep -q '^zfrag: not a zfrag archive' plain.err && ok "zfrag $cmd on a plain tar.zst says: not a zfrag archive" || fail "zfrag $cmd gave the wrong message: $(cat plain.err)"; }
done

# 8. the list is the whole interface; odd entries are skipped loudly, never silently
ln -s doom.wad src/games/doom/link.wad
{ cat list.txt; echo src/docs; echo src/games/doom/link.wad; echo "src/../etc/passwd"; echo nosuchfile; } > odd.txt
"$Z" pack -T odd.txt -o odd.tar.zfrag --frame 64K --level 3 > odd.out 2> odd.err; rc=$?
[ "$rc" -eq 1 ] && [ "$("$Z" ls odd.tar.zfrag | wc -l)" -eq "$nfiles" ] && "$Z" verify -q odd.tar.zfrag && ok "4 odd entries skipped, exit status 1, the archive is complete and valid" || fail "odd entries mishandled (exit $rc)"
grep -q 'skipping src/games/doom/link.wad' odd.err && grep -q 'unsafe name' odd.err && ok "each skipped entry is named on stderr" || fail "skips not reported"
grep -qi 'duplicate' odd.out && fail "the summary still talks about duplicates" || ok "the summary says nothing about duplicates (that is sortof's job)"

# 8b. the archive name: .tar.zfrag, never a plain-looking tar.zst
for bad in x.tar.zst x.zst x.tar x; do
  "$Z" pack -T list.txt -o $bad -q 2> name.err && fail "pack accepted the name $bad" || \
    { grep -q 'must end in .tar.zfrag' name.err && grep -q 'Did you mean x.tar.zfrag' name.err && ok "pack refuses -o $bad and suggests x.tar.zfrag" || fail "wrong message for -o $bad: $(cat name.err)"; }
done
[ ! -e x.tar.zst ] && [ ! -e x.zst ] && [ ! -e x ] && [ ! -e x.tar ] && ok "a refused name creates no file" || fail "a refused name left a file"

# 9. the output is all or nothing
"$Z" pack -T list.txt -o a.tar.zfrag -q 2>/dev/null && fail "overwrote without --force" || ok "refuses to overwrite without --force"
"$Z" pack -T list.txt -o a.tar.zfrag -q --force && ok "--force overwrites" || fail "--force failed"
echo nosuchfile > none.txt; : > empty.txt
"$Z" pack -T none.txt -o none.tar.zfrag -q 2>/dev/null && fail "packed nothing but succeeded" || ok "a list with nothing packable fails"
"$Z" pack -T empty.txt -o none.tar.zfrag -q 2>/dev/null && fail "packed an empty list" || ok "an empty list fails"
[ ! -e none.tar.zfrag ] && [ ! -e none.tar.zfrag.tmp ] && ok "failed runs leave no file behind" || fail "a failed run left a file"
head -c 60000000 /dev/urandom > big.bin; echo big.bin > big.txt
"$Z" pack -T big.txt -o int.tar.zfrag --level 22 --frame 64K -j 1 -q & pid=$!
i=0; while [ $i -lt 40 ] && [ ! -e int.tar.zfrag.tmp ]; do sleep 0.1; i=$((i + 1)); done
# (kill succeeds on a process that has already finished but is not yet reaped, so the exit status
# decides: 130 means it really was interrupted; anything else means it finished first)
if kill -INT $pid 2>/dev/null; then
  wait $pid 2>/dev/null; st=$?
  if [ "$st" -eq 130 ]; then
    [ ! -e int.tar.zfrag.tmp ] && [ ! -e int.tar.zfrag ] && ok "an interrupted pack leaves no file behind" || fail "an interrupted pack left a file"
  else
    echo "skip - the pack finished before it could be interrupted (exit $st)"
  fi
else
  wait $pid 2>/dev/null; echo "skip - the pack finished before it could be interrupted"
fi


# 10. --strip-components: the archive holds what is inside a directory (dir/game1 -> game1)
"$Z" pack -T list.txt -o s1.tar.zfrag --frame 64K --level 3 --strip-components 1 -q && ok "pack --strip-components 1 ran" || fail "pack --strip-components failed"
"$Z" ls s1.tar.zfrag | cut -c14- | grep -q '^src/' && fail "a stored name still starts with src/" || ok "no stored name starts with src/ (the directory is folded away)"
rm -rf o9 && mkdir o9 && "$Z" cat s1.tar.zfrag | tar -xf - -C o9 && diff -r -x link.wad src o9 > /dev/null \
  && ok "extracting into a directory gives back exactly the contents of src/" || fail "stripped archive does not extract to the contents of src/"
"$Z" verify -q s1.tar.zfrag -T list.txt --strip-components 1 && ok "verify -T LIST --strip-components 1 compares the stripped archive with the same list" || fail "verify with --strip-components failed"
"$Z" verify -q s1.tar.zfrag -T list.txt 2>/dev/null && fail "verify without the option passed a stripped archive" || ok "verify without the option notices the names differ"
"$Z" cat s1.tar.zfrag | tar --quoting-style=literal -tf - > s1.names; sed 's|^src/||' list.txt | diff - s1.names > /dev/null && ok "the stored names are the listed names with the first component dropped, in list order" || fail "stripped names or order differ from the list"
"$Z" pack -T list.txt -o s0.tar.zfrag --frame 64K --level 3 --strip-components 0 -q && cmp -s s0.tar.zfrag j1.tar.zfrag && ok "--strip-components 0 is the default: same archive byte for byte" || fail "strip 0 differs from the default"

"$Z" pack -T list.txt -o s3.tar.zfrag --frame 64K --level 3 --strip-components 3 > s3.out 2> s3.err; rc=$?
[ "$rc" -eq 1 ] && grep -q 'nothing is left of the name after --strip-components' s3.err && "$Z" verify -q s3.tar.zfrag && ok "names with too few components are skipped loudly (exit 1); the rest is a valid archive" || fail "strip 3: exit $rc / $(head -2 s3.err)"
"$Z" pack -T list.txt -o s9.tar.zfrag --strip-components 9 -q 2>/dev/null && fail "stripped every name away and still succeeded" || { [ ! -e s9.tar.zfrag ] && ok "stripping every name away fails and leaves no file" || fail "file left after total strip"; }
for bad in -1 65 x; do
  "$Z" pack -T list.txt -o sx.tar.zfrag --strip-components $bad -q 2>/dev/null; [ $? -eq 2 ] || fail "--strip-components $bad was not refused"
done
ok "--strip-components outside 0..64 is refused"

# two paths that become the same name are reported, not silently merged
mkdir -p c1/a c1/b; echo 1 > c1/a/x; echo 22 > c1/b/x; printf 'c1/a/x\nc1/b/x\n' > coll.txt
"$Z" pack -T coll.txt -o coll.tar.zfrag --frame 64K --level 3 --strip-components 2 -q 2> coll.err; rc=$?
[ "$rc" -eq 1 ] && grep -q '1 stored name appears more than once (first: x)' coll.err && ok "two paths that strip to the same name are reported (exit 1)" || fail "collision not reported: exit $rc / $(head -2 coll.err)"
printf 'c1/a/x\nc1/a/x\n' > twice.txt
"$Z" pack -T twice.txt -o twice.tar.zfrag --frame 64K --level 3 -q 2> twice.err; rc=$?
[ "$rc" -eq 1 ] && grep -q 'appears more than once' twice.err && ok "the same path listed twice is reported too" || fail "duplicate line not reported: exit $rc"

# every command answers --help with its usage on stdout and status 0 (it used to be taken for an archive name)
bad=
for c in ls cat extract verify; do
  out=$("$Z" $c --help 2>/dev/null); rc=$?
  [ $rc -eq 0 ] && printf '%s\n' "$out" | head -1 | grep -q "^usage: zfrag $c" || { fail "zfrag $c --help gave status $rc and: $(printf '%s' "$out" | head -1)"; bad=1; }
done
[ -z "$bad" ] && ok "ls, cat, extract and verify print their usage for --help (stdout, status 0)"
# ---- padding: a loop device drops a partial last sector, which would cut the trailer off ----
# so pack makes the archive a multiple of 4096 bytes with a padding frame before the trailer
sz=$(stat -c %s a.tar.zfrag)
[ $((sz % 4096)) -eq 0 ] && ok "pack makes the archive a multiple of 4096 bytes ($sz): a loop device cannot cut the trailer off" || fail "archive size $sz is not a multiple of 4096"
"$Z" pack -T list.txt -o np.tar.zfrag --frame 64K --level 3 -j 2 --no-pad -q || fail "pack --no-pad failed"
nsz=$(stat -c %s np.tar.zfrag)
if [ $((nsz % 512)) -ne 0 ]; then
  ok "--no-pad leaves the archive at its natural size ($nsz, not a multiple of 512)"
else
  echo "skip - this archive happens to be a multiple of 512 without padding; the unpadded-size check was not run"
fi
"$Z" verify -q np.tar.zfrag -T list.txt && "$Z" verify -q a.tar.zfrag -T list.txt && ok "padded and unpadded archives both verify against the files on disk" || fail "verify of padded/unpadded archives"
"$Z" cat np.tar.zfrag > np.tar; "$Z" cat a.tar.zfrag > pp.tar; cmp -s np.tar pp.tar && ok "the padded and unpadded archives hold the identical tar stream" || fail "padded and unpadded streams differ"
if [ -n "$plainname" ]; then
  plain a.tar.zfrag > pp2.tar; cmp -s pp2.tar np.tar && ok "plain decode ($plainname) skips the padding frame: same stream" || fail "plain decode of the padded archive differs"
fi
cp np.tar.zfrag fix.tar.zfrag
"$Z" pad fix.tar.zfrag > pad.out && fsz=$(stat -c %s fix.tar.zfrag) && [ $((fsz % 4096)) -eq 0 ] && ok "zfrag pad brings an unpadded archive to a multiple of 4096 ($nsz -> $fsz bytes)" || fail "pad: $(cat pad.out)"
head -c "$nsz" fix.tar.zfrag | cmp -s - np.tar.zfrag && ok "pad only appends: the first $nsz bytes are exactly the old archive" || fail "pad changed bytes of the old archive"
"$Z" verify -q fix.tar.zfrag -T list.txt && "$Z" cat fix.tar.zfrag | cmp -s - pp.tar && ok "the padded-by-pad archive verifies and holds the same tar stream" || fail "padded-by-pad archive differs"
"$Z" ls fix.tar.zfrag | wc -l | grep -qx "$nfiles" && ok "and lists all $nfiles files" || fail "ls of the padded-by-pad archive"
cp fix.tar.zfrag fix2.tar.zfrag; "$Z" pad fix2.tar.zfrag | grep -q 'nothing to do' && cmp -s fix.tar.zfrag fix2.tar.zfrag && ok "pad on an archive that is already a multiple of 4096 does nothing and leaves the file untouched" || fail "pad of an aligned archive"
"$Z" pad -q fix.tar.zfrag && [ -z "$("$Z" pad -q fix.tar.zfrag)" ] && ok "pad -q is quiet" || fail "pad -q printed something"
"$Z" pad > /dev/null 2>&1; [ $? -eq 2 ] && ok "pad with no archive is a usage error (status 2)" || fail "pad without an argument"
head -c 100 /dev/urandom > notanarchive.bin; "$Z" pad notanarchive.bin > /dev/null 2>&1; [ $? -ne 0 ] && [ "$(stat -c %s notanarchive.bin)" -eq 100 ] && ok "pad refuses a file that is not an archive and does not touch it" || fail "pad of a non-archive"
# what lies between the index and the trailer must be whole skippable frames: damage the first byte of
# the old trailer that pad left there (the high byte of its frame magic), and the archive is refused
cp fix.tar.zfrag bad.tar.zfrag; printf 'X' | dd of=bad.tar.zfrag bs=1 seek=$((nsz - 40 + 3)) conv=notrunc 2>/dev/null     # the magic's high byte: 0x18 becomes 0x58
"$Z" ls bad.tar.zfrag > /dev/null 2>&1; [ $? -ne 0 ] && ok "bytes between the index and the trailer that are not padding frames are refused" || fail "a damaged padding region was accepted"

# the top-level help mentions --strip-components (it is easy to forget)
"$Z" --help 2>&1 | grep -q -- '--strip-components' && "$Z" --help 2>&1 | grep -q 'stores ep1/' \
  && ok "zfrag --help explains --strip-components with an example" || fail "the top-level help does not mention --strip-components"
"$Z" > /dev/null 2> nohelp.txt; [ $? -ne 0 ] && grep -q -- '--strip-components' nohelp.txt && ok "and so does the usage printed when no command is given" || fail "no-command usage lacks the note"

# ---- the name of the list does not matter: a list called sortof-list.txt or MinHash.lst is just a list ----
cp list.txt sortof-list.txt
"$Z" pack -T sortof-list.txt -o sim1.tar.zfrag --frame 64K --level 3 -q < /dev/null 2> sim.err && [ ! -s sim.err ] && "$Z" verify -q sim1.tar.zfrag -T list.txt \
  && ok "a list called sortof-list.txt is packed at once, with no question and no output" || fail "sortof-list.txt was questioned or failed: $(head -c 120 sim.err)"
cmp -s sim1.tar.zfrag j1.tar.zfrag && ok "and the archive is byte for byte the one made from list.txt" || fail "the list's name changed the archive"

if [ "$fails" -eq 0 ]; then echo "all tests passed"; exit 0; fi
echo "$fails test(s) failed"
exit 1
