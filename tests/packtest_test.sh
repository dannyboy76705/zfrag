#!/bin/sh
# Tests for `zfrag packtest`: sampling, and that the list is run as written (nothing is
# reordered, dropped or scanned for duplicates).
#   usage: sh tests/packtest_test.sh [path/to/zfrag]      (default ./build/zfrag)
# Needs only a POSIX shell, awk, head, stat (GNU) and xargs.
#
# Part 1 builds a small fake component clip (y, u and v directories), asks awk
# to work out independently which windows the sampler must take, and compares
# the window and file counts and the number of frames at several frame sizes
# (the sample is one stream, so frames = ceil(sampled bytes / frame size)).
# Part 2 uses files of exact sizes, so the byte totals are exact.

Z=${1:-./build/zfrag}
[ -x "$Z" ] || { echo "no executable $Z" >&2; exit 2; }
Z=$(cd "$(dirname "$Z")" && pwd)/$(basename "$Z")      # the tests change directory, so the path must be absolute

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
cd "$T" || exit 2

fails=0
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }

# ---------------------------------------------------------------------------
# Part 1: sampling. 10 files per second, so "2 s of every 20 s" is 20 files of 200.
# ---------------------------------------------------------------------------
mkdir -p set/ep1/y set/ep1/u set/ep1/v
printf 'resolution=32x16\nsampling=420\nframerate=10:1\nbitdepth=8\naspect=1:1\n' > set/specs.txt
i=1
while [ "$i" -le 650 ]; do
  n=$(printf '%07d' "$i")
  head -c $((16384 + i % 7)) /dev/urandom > "set/ep1/y/yuv-$n.y"   # sizes differ a little
  head -c $((4096 + i % 5)) /dev/urandom > "set/ep1/u/yuv-$n.u"
  head -c $((4096 + i % 3)) /dev/urandom > "set/ep1/v/yuv-$n.v"
  i=$((i + 1))
done
{ echo set/specs.txt
  for p in y u v; do ls set/ep1/$p/* | sort; done; } > list.txt

W=20; E=200

# what the sampler must pick from list.txt as written: expect.txt has one line per
# window ("run/window files bytes"); nexp and totexp are the window count and bytes
make_expect() {
  tr '\n' '\0' < list.txt | xargs -0 stat -c '%s %n' > sizes.txt
  awk -v W=$W -v E=$E '
    { size = $1; path = $2; d = path; sub(/[^\/]*$/, "", d)
      if (d != pd) { run++; pd = d; idx = 0 }
      if (idx % E < W) { key = run "/" int(idx / E); if (!(key in seen)) { seen[key] = 1; order[++nw] = key }
                         bytes[key] += size; files[key]++ }
      idx++ }
    END { for (i = 1; i <= nw; i++) print order[i], files[order[i]], bytes[order[i]] }' sizes.txt > expect.txt
  nexp=$(wc -l < expect.txt)
  totexp=$(awk '{ s += $3 } END { print s }' expect.txt)
  awk '{ print $2, $3 }' expect.txt > want.txt
}
make_expect

"$Z" packtest -T list.txt --sample --level 3 --frames 64K,128K,256K -q > out.txt || fail "packtest --sample ran"

# 1. the right files are taken: window count and file count, from the banner
nfiles_exp=$(awk '{ s += $2 } END { print s }' expect.txt)
tot=$(wc -l < list.txt)
grep -q ", $nexp windows, $nfiles_exp of $tot listed files$" out.txt \
  && ok "the sample takes $nexp windows, $nfiles_exp of $tot files" || fail "selection differs: $(grep '^sample:' out.txt)"

# 2. the sample is one stream, cut at the usual size: ceil(bytes / frame) frames
for F in 65536 131072 262144; do
  want=$(( (totexp + F - 1) / F ))
  case $F in 65536) lbl=64K ;; 131072) lbl=128K ;; *) lbl=256K ;; esac
  got=$(awk -v L=$lbl '$1 == L { print $2 }' out.txt)
  [ "$got" = "$want" ] && ok "frame count at $lbl is $want (one stream of $totexp bytes)" || fail "frames at $lbl: got $got, expected $want"
done

# 3. frames may span windows: the largest size has fewer frames than there are windows
got=$(awk '$1 == "256K" { print $2 }' out.txt)
[ "$got" -lt "$nexp" ] && ok "a 256K frame holds several windows ($got frames for $nexp windows)" || fail "frames did not span windows"
grep -q 'understated' out.txt && fail "a window flag is still printed" || ok "no window flags"

# 4. totals do not depend on the number of jobs
a=$("$Z" packtest -T list.txt --sample --level 3 --frames 64K,128K -q | awk '$1 == "64K" || $1 == "128K" { print $1, $2, $5, $6 }')
b=$("$Z" packtest -T list.txt --sample --level 3 --frames 64K,128K -j 4 -q | awk '$1 == "64K" || $1 == "128K" { print $1, $2, $5, $6 }')
[ -n "$a" ] && [ "$a" = "$b" ] && ok "-j 4 gives the same totals as -j 1" || fail "totals depend on -j"

# 5. units: seconds and files describe the same sample
norm() { sed -n 's/ ([^)]*)//; /^sample:/p; /^ *64K /p' | awk '/^sample:/ { print; next } { print $1, $2, $3, $4, $5, $6 }'; }
a=$("$Z" packtest -T list.txt --window 20f --every 200f --level 3 --frames 64K -q | norm)
b=$("$Z" packtest -T list.txt --window 2 --every 20 --level 3 --frames 64K -q | norm)
[ -n "$a" ] && [ "$a" = "$b" ] && ok "20f/200f equals 2 s/20 s at 10 files per second" || fail "units disagree"

# 6. refusals
"$Z" packtest -T list.txt --window 30 --every 20 --level 3 -q 2>/dev/null && fail "window longer than period accepted" || ok "window longer than period refused"
grep -v 'specs.txt' list.txt > nospecs.txt
"$Z" packtest -T nospecs.txt --sample --level 3 -q 2>/dev/null && fail "ran without a frame rate" || ok "no frame rate refused"
"$Z" packtest -T nospecs.txt --sample --fps 10 --level 3 --frames 64K -q > /dev/null 2>&1 && ok "--fps supplies the frame rate" || fail "--fps rejected"

# ---------------------------------------------------------------------------
# Part 2: the list is run as written. a1 a2 b c1 c2 c3: 3 MiB are copies of other files, and
# packtest neither moves nor drops them nor says anything about them (that is sortof's job).
# ---------------------------------------------------------------------------
mkdir -p dd
head -c 1048576 /dev/urandom > dd/a1; cp dd/a1 dd/a2
head -c 1048576 /dev/urandom > dd/b
head -c 1048576 /dev/urandom > dd/c1; cp dd/c1 dd/c2; cp dd/c1 dd/c3
ls dd/* > dl.txt

"$Z" packtest -T dl.txt --level 3 --frames 4M -q > d1.txt
[ "$(awk '$1 == "4M" { print $3, $4 }' d1.txt)" = "6.00 MiB" ] && ok "every file stays in the stream (input 6.00 MiB)" || fail "files were dropped"
head -1 d1.txt | grep -q '6 files listed, every file kept in the stream, in the order listed' && ok "the banner says the list is run in the order listed" || fail "banner wrong: $(head -1 d1.txt)"
grep -qiE 'duplicate|front' d1.txt && fail "the output still talks about duplicates or front-loading" || ok "no duplicate scan, no front-loading in the output"
"$Z" packtest -T dl.txt --level 3 --frames 4M --no-dups -q > /dev/null 2>&1 && fail "--no-dups is still accepted" || ok "--no-dups is gone: an unknown option, refused"

# another order of the same files (a1 b c1 a2 c2 c3) holds the same bytes and runs to the end
printf 'dd/a1\ndd/b\ndd/c1\ndd/a2\ndd/c2\ndd/c3\n' > dl2.txt
"$Z" packtest -T dl2.txt --level 3 --frames 4M -q > d2.txt
[ "$(awk '$1 == "4M" { print $3, $4 }' d2.txt)" = "6.00 MiB" ] && ok "another order of the same files runs the same bytes" || fail "reordered list lost files"

# ---------------------------------------------------------------------------
# Part 3: `zfrag packtest` writes nothing, and the name of the list does not matter
# ---------------------------------------------------------------------------
ls -A | sort > ls.before
"$Z" packtest -T dl.txt --level 3 --frames 1M -q | cat > /dev/null
"$Z" packtest -T list.txt --sample --level 3 --frames 64K -q | cat > /dev/null
ls -A | sort > ls.after
new=$(comm -13 ls.before ls.after | grep -v '^ls.after$\|^ls.before$' | tr '\n' ' ')
[ -z "$new" ] && ok "no files appear beside the list" || fail "stray files created: $new"

cp dl.txt sortof-dl.txt
"$Z" packtest -T sortof-dl.txt --level 3 --frames 4M -q < /dev/null > sim.out 2> sim.err
[ -s sim.out ] && [ ! -s sim.err ] && ok "a list called sortof-dl.txt runs at once: its name is not questioned" || fail "sortof-dl.txt was questioned or failed: $(head -c 100 sim.err)"

# ---------------------------------------------------------------------------
# Part 4: sampling takes the list as it lies, copies and all. Put copies into the Part 1 clip
# (the same paths, so list.txt still lists it), then compare with what awk expects.
# ---------------------------------------------------------------------------
cp set/ep1/y/yuv-0000050.y set/ep1/y/yuv-0000300.y
cp set/ep1/y/yuv-0000051.y set/ep1/y/yuv-0000301.y
cp set/ep1/y/yuv-0000052.y set/ep1/y/yuv-0000302.y
make_expect

nfiles_exp=$(awk '{ s += $2 } END { print s }' expect.txt)
tot=$(wc -l < list.txt)
"$Z" packtest -T list.txt --sample --level 3 --frames 64K -q > sdp.txt
grep -q ", $nexp windows, $nfiles_exp of $tot listed files$" sdp.txt \
  && ok "--sample with copies in the list: $nexp windows, $nfiles_exp files, exactly what the list holds" || fail "selection with copies differs: $(grep '^sample:' sdp.txt)"
grep -qiE 'duplicate|front|kept whole' sdp.txt && fail "sample output still talks about duplicates or front-loading" || ok "sample output has no duplicate or front-loading lines"

if [ "$fails" -eq 0 ]; then echo "all tests passed"; exit 0; fi
echo "$fails test(s) failed"
exit 1
