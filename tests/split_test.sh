#!/bin/sh
# Tests for split archives: `pack --split`, `zfrag split`, every command reading the parts as one
# archive, the naming, and every way a set of parts can be wrong (missing, short, swapped, mixed).
#   usage: sh tests/split_test.sh [path/to/zfrag]            (default ./build/zfrag)
# Needs a C compiler with libzstd and libxxhash for the tree check (HARNESS_CFLAGS / HARNESS_LIBS if
# they are elsewhere), GNU split, od, truncate, tar. Everything is in a temporary directory.

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

H=
${CC:-cc} -std=gnu99 -O2 -pthread ${HARNESS_CFLAGS:-} -I"$ROOT" -o tree_harness \
  "$HERE/tree_harness.c" "$ROOT/archive.c" "$ROOT/fcache.c" "$ROOT/tree.c" "$ROOT/tar.c" \
  ${HARNESS_LIBS:--lzstd -lxxhash} 2> cc.err && H=./tree_harness

# ---- a source tree, and the archive it makes as one file ----
mkdir -p dir/game1 dir/game2/data dir/odd/a dir/many "dir/with space"
head -c 900000 /dev/urandom > dir/game1/big.dat; cp dir/game1/big.dat dir/game2/big-copy.dat
head -c 123456 /dev/urandom > dir/game2/run.exe; chmod 755 dir/game2/run.exe
printf '3' > dir/odd/a/b; printf 'x\n' > "dir/with space/a b.txt"; : > dir/game2/data/empty
i=1; while [ $i -le 150 ]; do head -c $((100 + i * 5)) /dev/urandom > "dir/many/f$(printf %03d $i)"; i=$((i + 1)); done
find dir -type f | sort > list.txt
nfiles=$(wc -l < list.txt)
OPT="--frame 64K --level 3 --strip-components 1"
"$Z" pack -T list.txt -o one.tar.zfrag $OPT -q || { echo "cannot make the test archive"; exit 2; }
S=$(stat -c %s one.tar.zfrag)
LIM=$((S / 3))
"$Z" cat one.tar.zfrag > one.tar

# 1. pack --split
mkdir a
"$Z" pack -T list.txt -o a/set.tar.zfrag $OPT --split $LIM -q && ok "pack --split ran" || fail "pack --split failed"
nparts=$(ls a | wc -l)
[ "$nparts" -ge 3 ] && ls a | grep -qv '^set\.tar\.zfrag\.[0-9][0-9][0-9]$' && fail "unexpected file names: $(ls a)" || ok "$nparts parts, named set.tar.zfrag.001, .002, ... (nothing else, no temporary files)"
ls -l a | awk -v L=$LIM 'NR > 1 && ($5 > L || $5 == 0) { bad = 1 } END { exit bad }' && ok "no part is empty or larger than the limit ($LIM)" || fail "a part is empty or over the limit"
cat a/set.tar.zfrag.* > a.joined; cmp -s a.joined one.tar.zfrag && ok "the parts joined with cat are byte-identical to the single-file archive" || fail "joined parts differ from the single-file archive"
[ ! -e a/set.tar.zfrag ] && ok "no single file is written beside the parts" || fail "a single file was written"

# 2. every name reaches the same archive; every command reads it
last=$(ls a | tail -1)
for n in set.tar.zfrag set.tar.zfrag.001 set.tar.zfrag.002 "$last"; do
  c=$("$Z" ls "a/$n" 2>&1 | wc -l)
  "$Z" cat "a/$n" > got.tar 2>/dev/null
  [ "$c" -eq "$nfiles" ] && cmp -s got.tar one.tar && continue
  fail "name $n: $c files, or the stream differs"; bad=1
done
[ -z "$bad" ] && ok "the normal name, the first part, a middle part and the last part all open the same archive (ls and cat agree with the single file)"
"$Z" verify -q a/set.tar.zfrag -T list.txt --strip-components 1 && ok "verify compares the split archive with the files on disk" || fail "verify of the split archive failed"
rm -rf x1 && "$Z" extract -q -C x1 a/set.tar.zfrag game2/ && cmp -s x1/game2/run.exe dir/game2/run.exe && ok "extract of a directory works across parts" || fail "extract from parts"
"$Z" verify a/set.tar.zfrag | grep -q "in $nparts parts" && ok "verify says how many parts it read" || fail "verify does not mention the part count"
if [ -n "$H" ]; then
  $H a/set.tar.zfrag walk | LC_ALL=C sort > p.walk; $H one.tar.zfrag walk | LC_ALL=C sort > s.walk
  cmp -s p.walk s.walk && ok "the directory tree (the mount's model) of the parts equals that of the single file" || fail "tree of the parts differs"
  $H a/set.tar.zfrag read /game1/big.dat 0 all | cmp -s - dir/game1/big.dat && ok "a file read through the tree across parts is byte-identical" || fail "tree read across parts"
fi

# 3. zfrag split gives exactly the parts pack --split gives
mkdir b && cp one.tar.zfrag b/set.tar.zfrag && "$Z" split --size $LIM b/set.tar.zfrag -q
bad=; for f in a/set.tar.zfrag.*; do cmp -s "$f" "b/$(basename "$f")" || bad=1; done
[ -z "$bad" ] && [ "$(ls b | wc -l)" -eq $((nparts + 1)) ] && ok "zfrag split makes the same parts as pack --split, byte for byte (naming is the same)" || fail "split and pack --split differ"
[ -f b/set.tar.zfrag ] && cmp -s b/set.tar.zfrag one.tar.zfrag && ok "the original archive is left in place" || fail "the original was touched"
"$Z" ls b/set.tar.zfrag > /dev/null && ok "(with the original beside its parts, the single file is the one that is read)" || fail "single file not read"

# 4. -d: another directory, and every spelling of 'here'
mkdir -p w/work w/work2 w/arch && cp one.tar.zfrag w/arch/set.tar.zfrag
( cd w/work && "$Z" split --size $LIM -d ./ ../arch/set.tar.zfrag -q ) && [ "$(ls w/work | wc -l)" -eq "$nparts" ] && ok "-d ./ writes the parts into the current directory" || fail "-d ./"
rm -f w/work/*; ( cd w/work && "$Z" split --size $LIM -d . ../arch/set.tar.zfrag -q ) && [ "$(ls w/work | wc -l)" -eq "$nparts" ] && ok "-d . works too" || fail "-d ."
"$Z" split --size $LIM -d w/work2// w/arch/set.tar.zfrag -q && [ "$(ls w/work2 | wc -l)" -eq "$nparts" ] && ok "-d dir// (trailing slashes) works" || fail "-d dir//"
"$Z" split --size $LIM -d w/nope w/arch/set.tar.zfrag 2> d.err; [ $? -ne 0 ] && grep -q 'No such file or directory' d.err && ok "a -d directory that does not exist is refused (and not created)" || fail "missing -d dir"
[ ! -e w/nope ] || fail "the missing directory was created"
"$Z" split --size $LIM w/arch/set.tar.zfrag -q && [ "$(ls w/arch | wc -l)" -eq $((nparts + 1)) ] && ok "without -d the parts go beside the archive" || fail "no -d"

# 5. the rules about what is already there
cp -r w/work2 w/work3
"$Z" split --size $LIM -d w/work3 w/arch/set.tar.zfrag -q 2> r.err && fail "split over existing parts without --force succeeded" || { grep -q 'use --force' r.err && ok "existing parts of the same name are not replaced without --force" || fail "no --force message: $(cat r.err)"; }
"$Z" split --size $((S * 2 / 3)) -d w/work3 --force w/arch/set.tar.zfrag -q && [ "$(ls w/work3 | wc -l)" -eq 2 ] && ok "--force replaces them, and removes leftover parts of an older, longer series (4 parts become 2)" || fail "--force with a shorter series: $(ls w/work3 | tr '\n' ' ')"
"$Z" split --size 300000 w/work2/set.tar.zfrag 2> o.err; [ $? -ne 0 ] && grep -q "one of the archive's own parts" o.err && ok "writing over the archive's own parts is refused" || fail "own-parts overwrite not refused: $(cat o.err)"
[ "$(ls w/work2 | wc -l)" -eq "$nparts" ] && ok "and nothing was changed" || fail "the refused run changed the parts"
rm -rf w/work4 && mkdir w/work4 && "$Z" split --size $((LIM * 2)) -d w/work4 w/work2/set.tar.zfrag -q && cat w/work4/set.tar.zfrag.* | cmp -s - one.tar.zfrag && ok "a split archive can be re-split to another size (read as one, cut again)" || fail "re-split"

# 6. pack --split over what is there
mkdir pk && cp one.tar.zfrag pk/set.tar.zfrag && cp a/set.tar.zfrag.001 pk/set.tar.zfrag.001 && cp a/set.tar.zfrag.001 pk/set.tar.zfrag.009
"$Z" pack -T list.txt -o pk/set.tar.zfrag $OPT --split $LIM -q 2> pk.err && fail "pack --split over an existing file and parts without --force succeeded" || ok "pack --split refuses to run over an existing archive or parts without --force"
"$Z" pack -T list.txt -o pk/set.tar.zfrag $OPT --split $LIM --force -q && [ ! -e pk/set.tar.zfrag ] && [ "$(ls pk | wc -l)" -eq "$nparts" ] && cat pk/set.tar.zfrag.* | cmp -s - one.tar.zfrag \
  && ok "--force replaces the single file and every stale part (a leftover .009 would have been joined on)" || fail "pack --split --force: $(ls pk | tr '\n' ' ')"

# 7. sizes that cannot work
"$Z" pack -T list.txt -o t1.tar.zfrag $OPT --split 1000 -q 2> t.err; [ $? -eq 2 ] && grep -q 'too small' t.err && ok "--split smaller than a frame is refused (exit 2)" || fail "tiny --split"
"$Z" pack -T list.txt -o t2.tar.zfrag $OPT --split abc -q 2>/dev/null; [ $? -eq 2 ] && ok "--split abc is refused" || fail "bad --split value"
"$Z" split --size 1000 one.tar.zfrag -d w/work2 --force 2> t.err; [ $? -ne 0 ] && grep -q 'alone is' t.err && ok "split --size smaller than one frame is refused, naming the frame" || fail "tiny split --size: $(cat t.err)"
ls t1.tar.zfrag* t2.tar.zfrag* 2>/dev/null | head -1 | grep -q . && fail "files left behind by refused runs" || ok "refused runs leave no files"

# 8. the index and trailer are a unit: if they do not fit, they get a part of their own
ioff=$(tail -c 40 one.tar.zfrag | od -An -tu8 -j16 -N8 | tr -d ' '); unit=$((S - ioff)); X=$((S - unit))     # the unit: index frame, padding and trailer, from the index to the end
mkdir ix && "$Z" pack -T list.txt -o ix/set.tar.zfrag $OPT --split $X -q
[ "$(ls ix | wc -l)" -eq 2 ] && [ "$(stat -c %s ix/set.tar.zfrag.002)" -eq "$unit" ] && ok "when the index does not fit beside the frames it gets its own part ($unit bytes)" || fail "index-only part: $(ls -l ix | awk 'NR>1 {print $5}' | tr '\n' ' ')"
cat ix/set.tar.zfrag.* | cmp -s - one.tar.zfrag && "$Z" verify -q ix/set.tar.zfrag -T list.txt --strip-components 1 && ok "and that archive reads and verifies" || fail "index-in-own-part archive"
mkdir ix2 && cp one.tar.zfrag ix2/set.tar.zfrag && "$Z" split --size $X ix2/set.tar.zfrag -d ix2/ -q 2>/dev/null; rm -rf ix2; mkdir ix2 && cp one.tar.zfrag ix2/set.tar.zfrag && mkdir ix2/out && "$Z" split --size $X -d ix2/out ix2/set.tar.zfrag -q
cmp -s ix2/out/set.tar.zfrag.002 ix/set.tar.zfrag.002 && cmp -s ix2/out/set.tar.zfrag.001 ix/set.tar.zfrag.001 && ok "zfrag split makes the same two parts" || fail "split index-only part differs"

# 9. checking that all the parts are present and right
rm -rf e && cp -r a e
rm e/set.tar.zfrag.002; "$Z" ls e/set.tar.zfrag 2> m.err > /dev/null; [ $? -ne 0 ] && grep -q 'part set.tar.zfrag.002 of set.tar.zfrag is missing' m.err && ok "a missing middle part is named: $(cut -c1-110 m.err)" || fail "missing middle part: $(cat m.err)"
rm -rf e && cp -r a e && rm e/set.tar.zfrag.001; "$Z" ls e/set.tar.zfrag 2> m.err > /dev/null; [ $? -ne 0 ] && grep -q 'first part of set.tar.zfrag is missing' m.err && ok "a missing first part is named" || fail "missing first part: $(cat m.err)"
rm -rf e && cp -r a e && rm "e/$last"; "$Z" ls e/set.tar.zfrag 2> m.err > /dev/null; [ $? -ne 0 ] && grep -q 'one may be missing' m.err && ok "a missing last part is caught (no trailer at the end) and the message says parts may be missing" || fail "missing last part: $(cat m.err)"
rm -rf e && cp -r a e && truncate -s -10 e/set.tar.zfrag.002; "$Z" ls e/set.tar.zfrag 2> m.err > /dev/null; [ $? -ne 0 ] && grep -q 'does not match the file size' m.err && ok "a part that is a few bytes short (an interrupted copy) is caught: the total no longer matches the trailer" || fail "short part: $(cat m.err)"
rm -rf e && cp -r a e && mv e/set.tar.zfrag.002 e/tmp && mv e/set.tar.zfrag.003 e/set.tar.zfrag.002 && mv e/tmp e/set.tar.zfrag.003
"$Z" verify -q e/set.tar.zfrag 2> m.err > /dev/null && fail "swapped parts passed verify" || ok "(what the parts contain is verify's job: with parts 002 and 003 swapped, every size and number is right, and verify fails)"
rm -rf e && cp -r a e && printf 'xx' > e/set.tar.zfrag.aa && cp e/set.tar.zfrag.aa e/set.tar.zfrag.ab; "$Z" ls e/set.tar.zfrag 2> m.err > /dev/null; [ $? -ne 0 ] && grep -q 'more than one series' m.err && ok "two series in one directory (.001... and .aa...) are refused when named by the plain name" || fail "two series: $(cat m.err)"
"$Z" ls e/set.tar.zfrag.001 > /dev/null 2>&1 && ok "...and naming a part says which series" || fail "naming a part did not resolve the ambiguity"

# 10. parts from other places: discs are symlinks in a working directory
mkdir -p disc1 disc2 disc3 wd; cp a/set.tar.zfrag.001 disc1/; cp a/set.tar.zfrag.002 disc2/; i=3; for f in a/set.tar.zfrag.*; do case "$f" in *.001|*.002) ;; *) cp "$f" disc3/ ;; esac; done
ln -s "$PWD/disc1/set.tar.zfrag.001" wd/set.tar.zfrag.001; ln -s "$PWD/disc2/set.tar.zfrag.002" wd/set.tar.zfrag.002; for f in disc3/*; do ln -s "$PWD/$f" "wd/$(basename "$f")"; done
"$Z" ls wd/set.tar.zfrag | wc -l | grep -q "^$nfiles$" && cat_ok=1; "$Z" cat wd/set.tar.zfrag | cmp -s - one.tar && ok "parts symlinked from three 'discs' into one working directory read as one archive" || fail "symlinked parts"
rm -rf disc2; "$Z" ls wd/set.tar.zfrag 2> m.err > /dev/null; [ $? -ne 0 ] && grep -q 'cannot open part .*002.*(is its disc mounted?)' m.err && ok "a disc that is not mounted (a dangling symlink) is named, with a hint" || fail "dangling symlink: $(cat m.err)"

# 11. the names split makes: .00, .000, .001 and letters are all read; a plain file ignores stray parts
for spec in "-d -a 2" "-d -a 3" "-d -a 3 --numeric-suffixes=1" ""; do
  rm -rf s && mkdir s && split -b $LIM $spec one.tar.zfrag s/set.tar.zfrag. && "$Z" cat s/set.tar.zfrag | cmp -s - one.tar && continue
  fail "split ${spec:-(letters)}: the series is not read: $("$Z" ls s/set.tar.zfrag 2>&1 | head -1)"; badn=1
done
[ -z "$badn" ] && ok "parts made by plain split (.aa, .ab), split -d (.00 and .000) and --numeric-suffixes=1 (.001) are all read, with no help"
rm -rf s && mkdir s && cp one.tar.zfrag s/set.tar.zfrag && cp a/set.tar.zfrag.001 s/ && cp a/set.tar.zfrag.002 s/ && "$Z" cat s/set.tar.zfrag | cmp -s - one.tar && ok "a plain archive file is read as itself even if stray .001/.002 files sit beside it" || fail "stray parts shadowed a plain file"
rm -rf s && mkdir s && cp a/set.tar.zfrag.001 s/backup.001 && cp a/set.tar.zfrag.002 s/backup.002 && "$Z" ls s/backup.001 2>&1 | grep -q 'not a zfrag archive' && ok "files that are not named *.tar.zfrag.NNN are never treated as a series" || fail "unrelated numbered files treated as parts"

# 12. an interrupted split run leaves nothing
head -c 40000000 /dev/urandom > big.bin; echo big.bin > big.txt; mkdir ib
"$Z" pack -T big.txt -o ib/set.tar.zfrag --level 22 --frame 64K --split 5M -j 1 -q & pid=$!
i=0; while [ $i -lt 60 ] && [ -z "$(ls ib 2>/dev/null)" ]; do sleep 0.1; i=$((i + 1)); done
# (kill succeeds on a process that has already finished but is not yet reaped, so the exit status
# decides: 130 means it really was interrupted; anything else means it finished first)
if kill -INT $pid 2>/dev/null; then
  wait $pid 2>/dev/null; st=$?
  if [ "$st" -eq 130 ]; then
    [ -z "$(ls ib)" ] && ok "an interrupted pack --split leaves no parts and no temporary files" || fail "interrupted pack --split left: $(ls ib | tr '\n' ' ')"
  else
    echo "skip - pack --split finished before it could be interrupted (exit $st)"
  fi
else
  wait $pid 2>/dev/null; echo "skip - pack --split finished before it could be interrupted"
fi

# 9. padding and parts: it is the size of the WHOLE archive that is made a multiple of 4096, and pad touches only the last part
mkdir pq && "$Z" pack -T list.txt -o pq/set.tar.zfrag $OPT --split $LIM -q
tot=$(cat pq/set.tar.zfrag.* | wc -c)
[ $((tot % 4096)) -eq 0 ] && [ "$(ls pq | wc -l)" -gt 1 ] && ok "an archive packed in parts is a multiple of 4096 bytes in total ($tot over $(ls pq | wc -l) parts), so joined it can sit on a loop device" || fail "split pack total $tot, parts: $(ls pq | wc -l)"
mkdir pp && "$Z" pack -T list.txt -o pp/set.tar.zfrag $OPT --no-pad --split $LIM -q && cp -r pp pp0
before=$(cat pp0/set.tar.zfrag.* | wc -c)
if [ $((before % 512)) -ne 0 ]; then
  "$Z" pad pp/set.tar.zfrag > /dev/null && after=$(cat pp/set.tar.zfrag.* | wc -c)
  [ $((after % 4096)) -eq 0 ] && ok "zfrag pad on an archive in parts makes the whole a multiple of 4096 ($before -> $after)" || fail "pad of a split archive: $before -> $after"
  last=$(ls pp0 | tail -1); bad=
  for f in $(ls pp0); do [ "$f" = "$last" ] && continue; cmp -s "pp0/$f" "pp/$f" || bad=1; done
  head -c "$(stat -c %s "pp0/$last")" "pp/$last" | cmp -s - "pp0/$last" && [ -z "$bad" ] && ok "only the last part changed, and only by appending" || fail "pad changed more than the end of the last part"
  "$Z" cat pp/set.tar.zfrag > pp.tar; "$Z" cat one.tar.zfrag > one.tar
  "$Z" verify -q pp/set.tar.zfrag -T list.txt --strip-components 1 && cmp -s pp.tar one.tar && ok "the padded archive in parts verifies against the files on disk and holds the same tar stream" || fail "verify or stream of the padded split archive"
else
  echo "skip - the unpadded split archive happens to be a multiple of 512 already; pad on parts was not exercised"
fi
if [ "$fails" -eq 0 ]; then echo "all tests passed"; exit 0; fi
echo "$fails test(s) failed"
exit 1
