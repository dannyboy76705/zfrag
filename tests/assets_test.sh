#!/bin/sh
# Tests for `zfrag pack --assets DIR`: the files directly in DIR are stored as assets/NAME after everything in the list.
#   usage: sh tests/assets_test.sh [path/to/zfrag]            (default ./build/zfrag)

Z=${1:-./build/zfrag}
[ -x "$Z" ] || { echo "no executable $Z" >&2; exit 2; }
Z=$(cd "$(dirname "$Z")" && pwd)/$(basename "$Z")
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
cd "$T" || exit 2
fails=0
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }

mkdir -p set/ep1 stuff/sub other
head -c 5000 /dev/urandom > set/ep1/a.bin; head -c 3000 /dev/urandom > set/ep1/b.bin; echo spec > set/specs.txt
echo srt > stuff/en.srt; head -c 4000 /dev/urandom > stuff/track2.mp3; head -c 4000 /dev/urandom > stuff/track10.mp3
echo h > stuff/.hidden; echo deep > stuff/sub/x; ln -s en.srt stuff/link.srt
printf 'set/specs.txt\nset/ep1/a.bin\nset/ep1/b.bin\n' > list.txt

"$Z" pack -T list.txt -o a.tar.zfrag --strip-components 1 --assets stuff 2> err.txt
grep -q '3 asset files' err.txt && ok "the files directly in the folder are added (not dot files, not below it, not a link)" || fail "asset count: $(cat err.txt)"
"$Z" cat a.tar.zfrag | tar tf - > names.txt
[ "$(tail -3 names.txt | tr '\n' ' ')" = "assets/en.srt assets/track2.mp3 assets/track10.mp3 " ] && ok "they come last, as assets/NAME, in natural order (track2 before track10), whatever the folder is called" || fail "order: $(cat names.txt | tr '\n' ' ')"
[ "$(head -3 names.txt | tr '\n' ' ')" = "specs.txt ep1/a.bin ep1/b.bin " ] && ok "the list is untouched and strip-components applies to it only" || fail "list part: $(head -3 names.txt | tr '\n' ' ')"
"$Z" verify a.tar.zfrag > /dev/null && mkdir ex && "$Z" extract -C ex a.tar.zfrag assets/ > /dev/null && cmp -s ex/assets/track10.mp3 stuff/track10.mp3 && cmp -s ex/assets/en.srt stuff/en.srt \
  && ok "the archive verifies and the assets extract identical" || fail "assets content"

# relative to -C
rm -f c.tar.zfrag; "$Z" pack -q -C . -T list.txt -o c.tar.zfrag --assets stuff && "$Z" cat c.tar.zfrag | tar tf - | grep -q '^assets/en.srt$' && ok "a folder is relative to -C like the list" || fail "-C"

# errors and warnings
"$Z" pack -q -T list.txt -o m.tar.zfrag --assets nothere 2> err.txt; [ $? -ne 0 ] && grep -q 'nothere' err.txt && [ ! -e m.tar.zfrag ] && ok "a folder that is not there is an error and nothing is written" || fail "missing folder"
"$Z" pack -q -T list.txt -o e.tar.zfrag --assets other 2> err.txt; [ $? -eq 0 ] && grep -q 'holds no files' err.txt && ok "an empty folder is a warning, the archive is still made" || fail "empty folder"
touch stuff/file; "$Z" pack -q -T list.txt -o f.tar.zfrag --assets stuff/file 2> err.txt; [ $? -ne 0 ] && ok "a file is not a folder" || fail "file given"

# a file the list already holds is not added twice
printf 'set/specs.txt\nstuff/en.srt\n' > l2.txt; "$Z" pack -T l2.txt -o d.tar.zfrag --assets stuff 2> err.txt
"$Z" cat d.tar.zfrag | tar tf - | sort | uniq -d | grep -q . && fail "a name twice" || ok "a file the list holds itself is not added a second time under assets/"

[ "$fails" -eq 0 ] && echo "all tests passed" || { echo "$fails failed"; exit 1; }
