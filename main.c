/* SPDX-License-Identifier: MIT */
/* zfrag - subcommand dispatch. More subcommands (create, extract, verify,
 * stat, mount) will be added here as they are written. */
#include <stdio.h>
#include <string.h>

int zfrag_packtest(int argc, char **argv);
int zfrag_pack(int argc, char **argv);
int zfrag_cat(int argc, char **argv);
int zfrag_ls(int argc, char **argv);
int zfrag_extract(int argc, char **argv);
int zfrag_verify(int argc, char **argv);
int zfrag_split(int argc, char **argv);
int zfrag_pad(int argc, char **argv);
#ifdef ZFRAG_WITH_FUSE
int zfrag_mount(int argc, char **argv);
#endif

static void usage(void)
{
	fprintf(stderr,
"usage: zfrag <command> [options]\n"
"\n"
"commands:\n"
"  pack      write an archive of exactly the files in a list (tar in zstd frames, with an index)\n"
"  cat       write an archive's tar stream to stdout\n"
"  ls        list the files in an archive\n"
"  extract   extract some or all files, decoding only the frames they are in\n"
"  verify    check an archive, and optionally compare it with the files on disk\n"
"  split     cut an existing archive into numbered parts (for discs), without recompressing\n"
"  pad       pad an older archive to a multiple of 4096 bytes so it can sit whole on a loop device\n"
"  mount     mount an archive read-only as a directory tree (needs libfuse3); a cdcstore\n"
"            repository inside it also appears as the files it holds\n"
"  packtest  measure compression ratio and decode speed per frame size\n"
"\n"
"A file list is one path per line, packed in the order given:\n"
"  find /path/to/source -type f | sort -V > my_files.txt\n"
"\n"
"To leave leading directories out of the stored names, use --strip-components (like tar): from list paths\n"
"series1/ep1/... and series1/ep2/...,\n"
"  zfrag pack -T list.txt -o series1.tar.zfrag --strip-components 1\n"
"stores ep1/... and ep2/.... (Options of each command: zfrag COMMAND --help.)\n"
"\nAn archive in parts (pack --split, or zfrag split) is read as one by every command: give its\n"
"normal name (ARCHIVE.tar.zfrag) or any part (ARCHIVE.tar.zfrag.002) with the parts beside it.\n");
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "pack"))
		return zfrag_pack(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "cat"))
		return zfrag_cat(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "ls"))
		return zfrag_ls(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "extract"))
		return zfrag_extract(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "verify"))
		return zfrag_verify(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "split"))
		return zfrag_split(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "pad"))
		return zfrag_pad(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "mount")) {
#ifdef ZFRAG_WITH_FUSE
		return zfrag_mount(argc - 1, argv + 1);
#else
		fprintf(stderr, "zfrag: this build has no mount support: libfuse3 was not found when it was built.\n"
				"       Install libfuse3-dev and pkg-config, then run make again.\n");
		return 1;
#endif
	}
	if (argc >= 2 && !strcmp(argv[1], "packtest"))
		return zfrag_packtest(argc - 1, argv + 1);
	usage();
	return 2;
}
