# zfrag

zfrag packs a large, mostly-read data set into one file that is small, can be read from the middle,
and can be mounted. An archive is a tar stream cut into independent zstd frames, followed by an
index, so a reader decodes only the frames a file lives in.

* **Small.** At the same compressor (zstd level 9) a zfrag archive is within about 1 to 4% of a plain
  `.tar.zst` of the same list, and competitive with DwarFS (comparison below), while it can be read from the middle.
* **Plain tools can read it.** `tar -xf`, `unzstd -c | tar -x` and `zstd -d` work without zfrag.
* **Random access.** `zfrag extract` decodes only the frames of the files you ask for, and
  `zfrag mount` presents the archive as a read-only directory through FUSE. If the archive holds a
  `cdcstore` repository or an `mjpegstore` video set, the mount also shows the files it holds, or one
  `.mkv` per clip (see Mounting).
* **Discs.** An archive can be cut into numbered parts (for DVD+Rs) and every command reads the parts
  as one archive.

Archives are named `NAME.tar.zfrag`. `pack` insists on the suffix: the file is readable by zstd but
is not an ordinary `.tar.zst`. An archive is padded to a multiple of 4096 bytes (a skippable frame before
the trailer, which every zstd tool skips) so that it can sit on a loop device or a disc image whole;
`--no-pad` turns that off, and `zfrag pad ARCHIVE` pads an older archive by appending to it, rewriting
nothing. Archives made by this version need this version or later to be read by zfrag itself.

## Numbers

Frame size is the main lever. Compressed size as a percentage of the original, three 480p anime episodes
(zstd level 9):

| frame | 1M | 2M | 4M | 8M | 16M | 32M |
|---|---|---|---|---|---|---|
| size | 26.08% | 22.91% | 21.28% | 20.46% | 20.06% | 19.87% |

`zfrag packtest` measures this curve (and decode speed) for your own data; on a single episode its
prediction for 32M frames was within 0.4% of the real archive. 32M is the default and the right
choice for an archive; 16M is the better choice for an archive you play from.

### Compared with other formats

The corpus is two `cdcstore` repositories (a DOS collection and a set of disc images), so the data is already cut into chunks and the containers have to find the repeats themselves, plus a set of `mjpegstore` video frames. Same machine, and one list shared by the first three rows (`sortof-cdcsort` for the DOS and ISO sets, `sortof-palscore` for the video frames), so only the container differs. DwarFS gets the list with `--order=none` because its own similarity ordering came out larger than a MinHash list. Settings are listed so the numbers can be repeated. All three were compressed at zstd level 9; zfrag was packed with `--level 9` and `--frame 8M` (its default level is 19, and its default frame is 32M).

| | DOS collection (622,981 files) | ISO collection (73,102 files) | Video frames (2,864,534,832 raw) |
|---|---|---|---|
| `.tar.zst`, zstd -9 | 6,103,466,864 | 718,117,377 | 1,466,112,346 |
| DwarFS, same list, `--order=none` | 6,082,046,543 | 721,100,313 | 1,496,915,559 |
| `.tar.zfrag`, zstd -9, 8 MiB frames | 6,168,965,120 | 725,430,272 | 1,520,611,328 |
| `.tar.zst`, unsorted list | 6,662,700,321 | 921,588,375 | 1,467,364,846 |
| DwarFS, its own ordering | not run | 763,996,856 | 1,746,018,966 |

Sizes in bytes. The first three rows use the same list: `sortof-cdcsort` for the DOS and ISO sets, `sortof-palscore` for the video frames. "Unsorted list" is the same files in the order they came (chunk order for the two `cdcstore` sets).

#### zfrag

**Pros:**
- Can be extracted with plain `zstd` (and `tar`) on any system, with or without zfrag installed.
- A built-in mounter that knows `cdcstore` and `mjpegstore` sets, serving the archive's files and the set's contents from one process, with no second FUSE layer.
- Very competitive archive sizes.

**Cons:**
- A new project.
- It needs an input file list.
- It can be used for general data, but it is built with `cdcstore` and `mjpegstore` in mind.

#### tar.zst

**Pros:**
- Extraction tools come with most distributions (`tar`, `zstd`).
- An established toolset that people already know.
- With a good list order ("chaining") the sizes are as good as any format here: smallest of the three on the video and ISO sets.

**Cons:**
- Nothing in it can be read without extracting first: no mounting, no reading a single file from the middle.

#### DwarFS

**Pros:**
- An established toolset.
- Good sizes, and better when given a sort file.
- Mounting and deduplication are built in.
- Reasonably fast to build.
- Everything is in a single container file.

**Cons:**
- In some cases it is bigger (see the table).
- Mounting goes through FUSE. Serving an `mjpegstore` or `cdcstore` set from a DwarFS mount with `mjpegmount` or `cdcstore-mount` puts a second FUSE layer on top, which slows operations down.

## Build

Needs a C compiler, make, pkg-config, libzstd and libxxhash (development packages). `zfrag mount`
is built in only if libfuse3 is found.

```
make                      # build/zfrag
make check-all            # every test suite
make install PREFIX=/usr  # or DESTDIR=/tmp/stage
```

`make install` also installs the man pages (for example `man zfrag`).

Other build options are in the header of the `Makefile`. For a clang build with LTO, see
[docs/clang-build.txt](docs/clang-build.txt) (it also says why `-march=native` can bite on an older PC).

## Quick start

```
find /path/to/source -type f | sort -V > my_files.txt     # one path per line, packed in this order
# or, with the sortof-suite:  sortof-natural /path/to/source > my_files.txt
zfrag pack -T my_files.txt -o set.tar.zfrag
zfrag ls set.tar.zfrag
zfrag extract -C out set.tar.zfrag game2/                 # a file, or a directory ending in /
zfrag verify set.tar.zfrag
zfrag mount set.tar.zfrag /mnt/set                        # read-only; unmount with fusermount3 -u /mnt/set
```

Every command prints its usage with `--help`.

### The list is the whole interface

`pack -T LIST` stores exactly the files in LIST, in the order written, and nothing else. Which files,
and in which order, is your choice; the order matters for compression, because similar files near
each other compress better. zfrag does not make lists itself. The `sortof-suite` tools do, one job each:

```
sortof-natural   DIR... > list.txt    # folder by folder, f2 before f10
sortof-extension DIR... > list.txt    # by extension: all .exe together, then all .dat, ...
sortof-minhash   DIR... > list.txt    # similarity order (MinHash), for frames and other near-duplicates
sortof-cdcsort   LIST|DIR... > list.txt   # keeps the order, but puts identical and near-identical files together
sortof-palscore  DIR...  -o list.txt  # scene order for video frames
```

Each takes any mix of directories, files and `-` (a list on stdin), so a list can be made from some folders and a
file with no sandbox directory built first. The natural orders do not depend on the locale or on the order the
disk lists files in, so the same files give the same list on every machine. Without the suite,
`find DIR -type f | sort -V` gives a natural order, and `zfrag pack -0` takes a NUL-separated list for file names that
contain a newline.

`pack` never reorders the list. Putting identical files first is a choice about the list, so it belongs in the tool
that makes it: `sortof-cdcsort -s 0` moves files with identical content behind the first of their set, and by default
(`-s 50`) also moves files that are at least 50% like an earlier file. Whether that helps depends on the data. It
helped a lot on disc images and a little on a DOS collection (see the table). On video frames the order hardly matters
to zstd: on three episodes, natural order gave 4,001,413,059 bytes and `sortof-palscore` with duplicate chaining
3,995,899,573, only 0.14% less (0.01% on one episode). Duplicates are where the order does matter: with three copies
of one episode, natural order gave 4,398,631,674 bytes, palscore alone 3,819,174,102, and with duplicate chaining
(`-D`) 1,492,883,124, because chaining puts a frame's twin one file away, inside any zstd window.

`--strip-components N` removes leading directories from the stored names, like tar. With list paths
`dir/game1/...` and `N=1` the archive holds `game1/...`, and mounting it at `dir` puts every file back
at `dir/game1/...`. `verify -T LIST --strip-components N` checks that the files on disk match.

### Assets: audio, subtitles, cover art

The files that go with a set but are not in its list (a soundtrack, subtitles, cover art) are added with one folder:

```
zfrag pack -T list.txt -o set.tar.zfrag --strip-components 1 --assets ~/ep1-extras
```

The files directly in that folder (no dot files, nothing below it) are stored as `assets/NAME` after everything in the list,
whatever the folder is called on disk, in natural order. The archive then holds the set and `assets/` side by side, and
`zfrag mount` shows `assets/` under `native/`, next to the video in `virtual/`, with no extra cost. The folder is relative
to `-C` like the list, a missing folder is an error, an empty one a warning, and a file the list already names is not added
twice.

### Keep your list

Keep the list file with the archive. Recompressing later (for storage, or in natural order for playback) then
needs no new sort, and building the order again can be slow: `sortof-minhash` and `sortof-cdcsort` read every file.

## Knobs and buttons

Two settings decide most of what an archive is like. Both are options of `zfrag pack`; the defaults are the ones to use for an archive.

```
zfrag pack -T list.txt -o set.tar.zfrag --frame 32M --level 19
```

| option | default | what it does |
|---|---|---|
| `--frame SIZE` | 32M | Uncompressed bytes per frame (64K to 32M). Bigger frames compress better (table above). |
| `--level N` | 19 | zstd level. Higher is smaller and slower to pack; reading is about as fast at any level. |

**The bigger the frame, the harder it is to seek.** A frame is the smallest unit zfrag can decode, so reading one file
or one picture decodes the whole frame it lives in, up to 32 MiB. Big frames are right for an archive that is stored and
read rarely. They are not recommended for an archive you play from or browse: use 16M (or smaller) there, or keep the 32M archive for
storage and repack a playback copy in natural order. A larger mount cache (`-o cache=MIB`) helps random access,
but it cannot make a big frame cheaper to decode.

Smaller buttons:

* `-j N` frames compressed at a time (default 4). The archive is the same for any N; memory is N frames.
* `--split SIZE` parts for discs (see Discs).
* `--assets DIR` a folder of audio, subtitles or cover art, stored as `assets/NAME`.
* `--strip-components N` and `-C DIR` change the stored names, not the data.
* `--no-pad` leaves out the 4096-byte padding.
* Mounting: `-o threads=N` and `-o cache=MIB`.

`zfrag packtest` measures the size and decode speed for each frame size on your own data before you pack.

## Reading an archive without zfrag

```
tar -xf set.tar.zfrag
unzstd -c set.tar.zfrag | tar -xf -
zstd -d set.tar.zfrag -o set.tar
```

A bare `zstd -d set.tar.zfrag` refuses, because zstd does not know the `.zfrag` suffix; give it `-o`.

## Discs: archives in parts

```
zfrag pack -T my_files.txt -o set.tar.zfrag --split 4300M     # set.tar.zfrag.001, .002, ...
zfrag split --size 4300M set.tar.zfrag                        # the same, for an archive that exists
```

Parts end between frames, so nothing is recompressed, and `cat set.tar.zfrag.0*` gives back the same
bytes a single file would hold. Every command reads the parts as one archive: give the normal name
or any one part, with the parts beside it. To mount, copy the parts back or link each disc's part into
one working directory. The tools only check that all parts are present (a missing one is named, with a
reminder that its disc may not be mounted); what each disc holds is up to you.

For example, with three discs, each holding one part, mounted at `/media/disc1`, `/media/disc2` and `/media/disc3`:

```
mkdir ~/set
ln -s /media/disc1/set.tar.zfrag.001 /media/disc2/set.tar.zfrag.002 /media/disc3/set.tar.zfrag.003 ~/set/
zfrag mount ~/set/set.tar.zfrag /mnt/set       # or name any one part: ~/set/set.tar.zfrag.002
fusermount3 -u /mnt/set
```

Or, with the parts copied to one folder on disk (needs room for the whole archive, but the discs can be put away):

```
mkdir ~/set
cp /media/disc1/set.tar.zfrag.001 /media/disc2/set.tar.zfrag.002 /media/disc3/set.tar.zfrag.003 ~/set/
zfrag mount ~/set/set.tar.zfrag /mnt/set
fusermount3 -u /mnt/set
```

The mount message lists the parts it found ("3 parts, set.tar.zfrag.001 .. set.tar.zfrag.003"). Every part must be present while it is mounted, so all the discs stay in their drives, or are copied to disk, for as long as you use it; a missing part is named in the error.

A part larger than 4 GiB (4300M is 4.2 GiB) needs a disc format that allows files that large: UDF, or
ISO 9660 level 3. Plain ISO 9660 stops just under 4 GiB; use a smaller `--split` for that, such as 4000M.

## Mounting

```
zfrag mount [-f] [-d] [-o OPTIONS] ARCHIVE MOUNTPOINT
```

The default view is the archive's own directory tree, read-only. Options after `-o`:

* `threads=N`: frames decoded at once. The default is two per CPU, at most 16. Each decoding thread
  works on one frame and needs one frame-sized buffer, and nothing else sets the memory cap, so memory
  follows the CPUs: a dual-core machine uses 4 threads, and about 128 MiB at 32M frames.
* `cache=MIB`: decoded data kept in memory. The default is one frame per thread, never fewer than 4
  frames. A larger cache helps random access and several readers.

### A cdcstore repository inside an archive

`cdcstore` splits files into chunk files (`chunks/bNNNNN/cNNNNNNNNN`) plus an index (`cdcstore.idx`), and
leaves sorting and compression to other tools. Pack the sorted chunk list into a zfrag archive with the
index first (`cdcstore-list -k` prints `cdcstore.idx` before the chunk files, and the `sortof-*` tools keep it
first without sorting or reading it; `-p PATH` pins any other file the same way):

```
cdcstore-list -k REPO | sortof-cdcsort - > list.txt
zfrag pack -T list.txt -o set.tar.zfrag
```

When `zfrag mount` finds `cdcstore.idx` in the archive (at the top, or in a top-level folder) it serves
two halves from the one process and one frame cache, with no mount stacked on another:

```
MOUNTPOINT/native/    the archive's own files: cdcstore.idx and chunks/
MOUNTPOINT/virtual/   the files the repository holds, with their paths, modes, times and links
```

A read in `virtual/` finds the chunks the byte range touches and decodes only the frames they are in. An
archive with no repository has its files at the top of the mount, as before. All the chunk files must be
in the archive; a read of a file whose chunk is missing fails (EIO) instead of returning wrong bytes.
The index's checksum is not checked while mounting (the archive's frames carry their own); run
`cdcstore-verify` on an extracted repository to check one.

### An mjpegstore set inside an archive

A set made by `mjpegstore` (a `specs.txt` and one folder of JPEG pictures per clip) is found the same way, at
the top of the archive or in a top-level folder, and the mount again has two halves:

```
MOUNTPOINT/native/    the archive's own files: specs.txt and the pictures
MOUNTPOINT/virtual/   one <clip>.mkv per clip
```

Each `.mkv` is the Matroska stream `mjpegstore -s` writes, but readable at any offset, so a player can seek in
it without anything being exported first. (The writing-app name in the header says `mjpegmount`, as it does
when `mjpegmount` serves the same set from a disk.) The code that lays out the file is `mjpegmount.c`, the same
file the `mjpegmount` program is built from, compiled here as a library with the archive as the place the
pictures live (`mjpegmount.h`), so the two cannot disagree; only the pictures a read touches are decoded.
Progressive sets only, as in `mjpegmount`. A set needs its `specs.txt` in the archive: `mjpegstore`'s list
begins with it.

**Which order to play from.** In a MinHash-sorted archive, consecutive pictures are scattered across the archive, so
playing a clip decodes the same frames over and over: one sequential read of a 439 MiB file took 7 s that way and
0.3 s from a natural-order archive (default cache). An archive in `sortof-palscore` scene order keeps neighbouring
frames together, and a clip played from one with `mpv` (muxed with its audio, below) showed no stutter. If you pack with MinHash, keep that
archive for storage and recompress the set in natural order at 16M frames for playback.

Audio, subtitles and cover art packed with `--assets` sit next to the video, so a player or `ffmpeg` can use them from
the same mount:

```
ffmpeg -i /mnt/set/virtual/ep1.mkv -i /mnt/set/native/assets/ep1.mp3 -codec copy -f matroska - | mpv -
```

## Source layout

Everything is in one folder: `main.c` dispatches the commands (`pack.c`, `unpack.c`, `split.c`, `pad.c`,
`packtest.c`, `mount.c`, ...; `cdcview.c` and `mkvview.c` are the two virtual views, and `mjpegmount.c` is
the `mjpegmount` program's source, used here as a library), `zfrag_format.h` describes the archive format, `tests/` holds the test
suites and `docs/` the clang build notes.

## Questions and answers

**Why make this? Don't other solutions exist?**
I wanted a FUSE-based system that works tightly with `mjpegstore` and `cdcstore` for decent performance, and that could be extracted without this program. Nothing I tried quite fit the bill. See the comparison section for more.

**Why Zstandard? There's LZMA, zpaq, gzip and others.**
Mainly its feature set: decompression is much faster than compression (highly asymmetric), and zstd frames can be concatenated, which is what lets each frame be decoded on its own. Its license also fits well with mine.

## Thank you

* **[Debian](https://www.debian.org)**: for a solid distribution that does not move, which makes supporting a version for years instead of days possible.
* **The [zstd](https://github.com/facebook/zstd) team**: you made this project possible.
* **The [DwarFS](https://github.com/mhx/dwarfs) team**: a fantastic format that gave me a lot of ideas.
* **The [libfuse](https://github.com/libfuse/libfuse) team**: you made it possible to have virtual filesystems and whatnot, all in user space.

## License

MIT. Copyright (c) 2026 Daniel Lee Witzel. See `LICENSE`.

### Third-party libraries

zfrag links to libzstd (BSD-3-Clause, or GPLv2 at your choice), xxHash (BSD-2-Clause) and, for `zfrag mount`, libfuse3 (LGPL-2.1). It includes none of their code: it uses the copies installed on the system.

If you package or distribute binaries: with dynamic linking to the system libraries (as a Debian package does) nothing more is needed beyond shipping the `LICENSE` file. If you link a library statically or bundle it, you must also include that library's own license text, and for libfuse (LGPL) let users replace it with their own build.
