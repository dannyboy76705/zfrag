#!/bin/sh
# Tests for the logic in mount.c (the libfuse3 callbacks and the command line), without
# FUSE: tests/glue_harness.c includes mount.c, compiled against tests/fuse_stub/fuse.h (a minimal
# stand-in for libfuse3's header), and calls the callbacks directly. It cannot test the real kernel
# interface; it tests everything the callbacks decide.
#   usage: sh tests/glue_test.sh [path/to/zfrag]            (default ./build/zfrag)
# Set HARNESS_CFLAGS / HARNESS_LIBS if libzstd and libxxhash are not in the default places.

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

${CC:-cc} -std=gnu99 -O2 -pthread -DMJPEGMOUNT_AS_LIBRARY -I"$ROOT" -c -o mjpegmount.o "$ROOT/mjpegmount.c" 2> cc0.err || { cat cc0.err; echo "cannot build mjpegmount.c"; exit 2; }
${CC:-cc} -std=gnu99 -Wall -Wextra -O2 -pthread ${HARNESS_CFLAGS:-} -DFUSE_USE_VERSION=31 -I"$HERE/fuse_stub" -I"$ROOT" \
  -o glue_harness "$HERE/glue_harness.c" "$ROOT/archive.c" "$ROOT/fcache.c" "$ROOT/tree.c" "$ROOT/tar.c" "$ROOT/cdcview.c" "$ROOT/mkvview.c" mjpegmount.o "$ROOT/threads.c" \
  ${HARNESS_LIBS:--lzstd -lxxhash} 2> cc.err || { cat cc.err; echo "cannot build tests/glue_harness.c"; exit 2; }
if [ -s cc.err ]; then echo "FAIL - mount.c compiles with warnings:"; cat cc.err; fails=$((fails + 1)); else ok "mount.c compiles with -Wall -Wextra and no warnings"; fi
G=./glue_harness

# ---- a source tree ----
mkdir -p src/odd/a src/games/doom src/games/keen src/docs src/many "src/with space"
printf '1' > src/odd/a-1; printf '2' > src/odd/a.txt; printf '3' > src/odd/a/b; printf '5' > "src/odd/a b"
head -c 700000 /dev/urandom > src/games/doom/doom.wad
head -c 123456 /dev/urandom > src/games/keen/keen.exe; chmod 755 src/games/keen/keen.exe
: > src/docs/empty.txt; seq 1 20000 > src/docs/numbers.txt; printf 'x\n' > "src/with space/a b.txt"
i=1; while [ $i -le 120 ]; do head -c $((50 + i)) /dev/urandom > "src/many/f$(printf %03d $i)"; i=$((i + 1)); done
touch -d '2001-02-03 04:05:06 UTC' src/games/keen/keen.exe
find src -type f | sort > list.txt
"$Z" pack -T list.txt -o g.tar.zfrag --frame 64K --level 3 -q || { echo "cannot make the test archive"; exit 2; }

# 1. getattr: every directory's link count, every file's mode, size and mtime, owner = the mounter
bad=0
find src -type d | LC_ALL=C sort > dirs.txt
while IFS= read -r d; do
  want="mode=555 nlink=$(( $(find "$d" -mindepth 1 -maxdepth 1 -type d | wc -l) + 2 ))"
  got=$($G g.tar.zfrag getattr "/$d")
  case "$got" in "dir $want size=0 blocks=0 uid=4242 gid=4343 "*) ;; *) fail "getattr /$d: $got (wanted $want)"; bad=1 ;; esac
done < dirs.txt
getattr_root=$($G g.tar.zfrag getattr /)
echo "$getattr_root" | grep -q "^dir mode=555 nlink=3 " && [ "$bad" -eq 0 ] && ok "directories are 0555 with nlink = 2 + subdirectories ($(wc -l < dirs.txt) checked, root has 1 subdirectory)" || fail "root getattr: $getattr_root"
bad=0
while read -r f; do
  size=$(stat -c %s "$f"); mode=$(stat -c %a "$f"); mt=$(stat -c %Y "$f"); blocks=$(( (size + 511) / 512 ))
  got=$($G g.tar.zfrag getattr "/$f")
  [ "$got" = "file mode=$mode nlink=1 size=$size blocks=$blocks uid=4242 gid=4343 mtime=$mt" ] || { fail "getattr /$f: $got"; bad=1; break; }
done < list.txt
[ "$bad" -eq 0 ] && ok "all $(wc -l < list.txt) files report their stored mode, size, mtime (and the mounting user as owner)"
[ "$($G g.tar.zfrag getattr /src/games/keen/keen.exe | sed 's/.*mode=\([0-9]*\) .*/\1/')" = "755" ] && ok "the stored mode is kept as it is (755 is not turned into 555), so repacking from a mount keeps modes" || fail "mode was altered"
[ "$($G g.tar.zfrag getattr /src/nope)" = "-ENOENT" ] && [ "$($G g.tar.zfrag getattr /src/odd/a-1/x)" = "-ENOENT" ] && ok "getattr of a missing path is -ENOENT" || fail "getattr missing path"

# 2. readdir: ".", "..", then the entries in name order, for every directory
bad=0
while IFS= read -r d; do
  { printf '.\n..\n'; find "$d" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort; } > want.rd
  $G g.tar.zfrag readdir "/$d" > got.rd
  cmp -s want.rd got.rd || { fail "readdir /$d differs: $(diff want.rd got.rd | head -2)"; bad=1; }
done < dirs.txt
[ "$bad" -eq 0 ] && ok "readdir gives . and .. then the entries in name order, in every directory"
[ "$($G g.tar.zfrag readdir /src/odd/a-1)" = "-ENOTDIR" ] && [ "$($G g.tar.zfrag readdir /nope)" = "-ENOENT" ] && ok "readdir of a file is -ENOTDIR, of a missing path -ENOENT" || fail "readdir errors"

# 3. open: read-only only, no directories
$G g.tar.zfrag open /src/odd/a-1 | grep -q '^0 keep_cache=1 fh=set$' && ok "open for reading succeeds, sets the handle, and lets the kernel keep its cache (the archive never changes)" || fail "open: $($G g.tar.zfrag open /src/odd/a-1)"
[ "$($G g.tar.zfrag open /src/odd/a-1 wr)" = "-EACCES" ] && ok "open for writing is refused (-EACCES)" || fail "write open not refused"
[ "$($G g.tar.zfrag open /src)" = "-EISDIR" ] && [ "$($G g.tar.zfrag open /nope)" = "-ENOENT" ] && ok "open of a directory is -EISDIR, of a missing path -ENOENT" || fail "open errors"

# 4. read: through the handle and by path, in ranges that cross frames, and past the end
bad=0
for r in "0 1" "65535 2" "65536 65536" "131071 70000" "699999 1" "699999 100" "700000 10" "700001 10" "0 700000" "350000 123456"; do
  off=${r% *}; len=${r#* }
  tail -c +$((off + 1)) src/games/doom/doom.wad | head -c $len > want.bin
  $G g.tar.zfrag read /src/games/doom/doom.wad $off $len > got1.bin
  $G g.tar.zfrag read /src/games/doom/doom.wad $off $len nofh > got2.bin
  cmp -s want.bin got1.bin && cmp -s want.bin got2.bin || { fail "read off=$off len=$len differs"; bad=1; }
done
[ "$bad" -eq 0 ] && ok "reads across frame boundaries, at the end and past it are right, by handle and by path"
$G g.tar.zfrag read /src/docs/empty.txt 0 100 | wc -c | grep -q '^0$' && ok "an empty file reads as empty" || fail "empty file"
bad=0
for f in src/games/keen/keen.exe src/docs/numbers.txt "src/with space/a b.txt" src/many/f001 src/many/f120; do
  $G g.tar.zfrag read "/$f" 0 10000000 > got.bin; cmp -s got.bin "$f" || { fail "whole read of $f differs"; bad=1; }
done
[ "$bad" -eq 0 ] && ok "whole files, including a name with a space, read back byte for byte"
[ "$($G g.tar.zfrag read /src 0 10)" = "-EISDIR" ] && ok "reading a directory is an error" || fail "read of a directory: $($G g.tar.zfrag read /src 0 10 | head -c 40)"

# 5. init: the kernel may remember everything for a long time
[ "$($G g.tar.zfrag init)" = "entry=3600 attr=3600 negative=3600 kernel_cache=1" ] && ok "init sets long entry/attr/negative timeouts and kernel_cache (nothing ever changes)" || fail "init: $($G g.tar.zfrag init)"

# 6. a plain tar.zst is refused
ioff=$(tail -c 40 g.tar.zfrag | od -An -tu8 -j16 -N8 | tr -d ' '); head -c "$ioff" g.tar.zfrag > plain.tar.zst
$G plain.tar.zst getattr / | grep -q '^OPEN FAILED: not a zfrag archive' && ok "a plain tar.zst is refused with 'not a zfrag archive'" || fail "plain tar.zst: $($G plain.tar.zst getattr / | head -1)"

# 7. the command line
a() { $G args "$@"; }
[ "$(a mount -f -o cache=512,allow_other g.tar.zfrag /mnt)" = "archive=g.tar.zfrag mnt=/mnt cache=512 fuse_o=allow_other flags=1 help=0" ] && ok "-f, -o cache=N,allow_other: ours consumed, the rest kept for libfuse" || fail "args: $(a mount -f -o cache=512,allow_other g.tar.zfrag /mnt)"
[ "$(a mount -ocache=8 g.tar.zfrag /mnt)" = "archive=g.tar.zfrag mnt=/mnt cache=8 fuse_o= flags=0 help=0" ] && ok "-ocache=8 (joined) works" || fail "joined -o: $(a mount -ocache=8 g.tar.zfrag /mnt)"
[ "$(a mount g.tar.zfrag /mnt | sed 's/.*cache=\([0-9a-z]*\) .*/\1/')" = "auto" ] && ok "the cache defaults to automatic (one frame per thread)" || fail "default cache: $(a mount g.tar.zfrag /mnt)"
[ "$(a mount -o ro,default_permissions g.tar.zfrag /mnt | sed 's/.*fuse_o=\([^ ]*\) .*/\1/')" = "default_permissions" ] && ok "-o ro is accepted (always on) and not passed twice" || fail "ro handling"
for bad in "-o rw" "-o cache=abc" "-o cache=" "-x" "-o"; do
  out=$(a mount $bad g.tar.zfrag /mnt); echo "$out" | grep -q '^ERROR:' || fail "'$bad' was accepted: $out"
done
a mount -o rw g.tar.zfrag /mnt | grep -q 'read-only' && ok "rw is refused (read-only), bad cache sizes and unknown options are refused" || fail "error texts"
a mount g.tar.zfrag | grep -q 'ERROR: an archive and a mount point are needed' && a mount a b c | grep -q 'too many arguments' && ok "a missing mount point and extra arguments are refused" || fail "argument count errors"
a mount --help | grep -q 'help=1' && ok "--help is recognised" || fail "--help"
[ "$($G fuseargs mount -f -o cache=8,allow_other,threads=4 g.tar.zfrag /mnt)" = "[zfrag][/mnt][-f][-o][ro,subtype=zfrag,fsname=g.tar.zfrag,allow_other,max_threads=4]" ] && ok "libfuse is called with the mount point, the flags and -o ro,subtype=zfrag,fsname=ARCHIVE,..." || fail "fuse args: $($G fuseargs mount -f -o cache=8,allow_other,threads=4 g.tar.zfrag /mnt)"
[ "$($G fuseargs mount -o threads=3 'we,ird.tar.zfrag' /mnt)" = "[zfrag][/mnt][-o][ro,subtype=zfrag,max_threads=3]" ] && ok "an archive name with a comma is left out of fsname (it would break the option list)" || fail "comma name: $($G fuseargs mount -o threads=3 'we,ird.tar.zfrag' /mnt)"


# 8. threads: two per CPU, at most 16, counted from the CPUs this process may use
for pair in "1 2" "2 4" "3 6" "4 8" "8 16" "9 16" "16 16" "64 16" "0 2"; do
  cpus=${pair% *}; want=${pair#* }
  [ "$($G threadsfor $cpus)" = "$want" ] || { fail "the rule for $cpus CPUs gave $($G threadsfor $cpus), expected $want"; badr=1; }
done
[ -z "$badr" ] && ok "the default rule: 1 CPU -> 2 threads, 2 -> 4, 4 -> 8, 8 or more -> 16 (capped)"
n=$(nproc); e=$((n * 2)); [ $e -gt 16 ] && e=16
[ "$($G defthreads)" = "cpus=$n threads=$e" ] && ok "on this machine ($n CPUs) the default is $e threads" || fail "defthreads: $($G defthreads), expected cpus=$n threads=$e"
if command -v taskset > /dev/null 2>&1; then
  [ "$(taskset -c 0 $G defthreads)" = "cpus=1 threads=2" ] && ok "the CPU count comes from the affinity mask: restricted to one CPU with taskset, the default is 2 threads" || fail "taskset: $(taskset -c 0 $G defthreads)"
else
  echo "skip - no taskset: the affinity-mask check was not run"
fi
[ "$($G threadflag mount g.tar.zfrag /mnt)" = "threads=$e" ] && [ "$($G threadflag mount -o threads=3 g.tar.zfrag /mnt)" = "threads=3" ] && [ "$($G threadflag mount -o threads=64 g.tar.zfrag /mnt)" = "threads=64" ] \
  && ok "threads=N overrides the default (1 to 64 are accepted; more than the default cap is the user's choice)" || fail "threads= option"
for bad in "threads=0" "threads=65" "threads=x" "threads="; do
  $G threadflag mount -o $bad g.tar.zfrag /mnt | grep -q '^ERROR: bad thread count' || fail "-o $bad was accepted"
done
ok "threads=0, threads=65, threads=x and an empty value are refused"
[ "$($G fuseargs mount -o max_threads=9 g.tar.zfrag /mnt)" = "[zfrag][/mnt][-o][ro,subtype=zfrag,fsname=g.tar.zfrag,max_threads=9]" ] && ok "a max_threads the user passes themselves goes to libfuse unchanged, and ours is not added beside it" || fail "user max_threads: $($G fuseargs mount -o max_threads=9 g.tar.zfrag /mnt)"
$G usage > usage.out; grep -q 'threads=N    frames decoded at once, 1 to 64 (default: 2 per CPU, at most 16: ' usage.out && grep -q 'one frame per thread, at least 4' usage.out \
  && ok "the help describes threads=N (with this machine's default) and the automatic cache" || fail "help text: $(grep -n 'threads\|cache=' usage.out | head -4)"

if [ "$fails" -eq 0 ]; then echo "all tests passed"; exit 0; fi
echo "$fails test(s) failed"
exit 1
