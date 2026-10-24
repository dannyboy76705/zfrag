/* SPDX-License-Identifier: MIT */
/* assets.h - `zfrag pack --assets DIR`: the files that belong with a set (audio, subtitles, cover art, ...) go in the
 * archive in a folder named assets/, added after everything the list names.
 *
 * DIR is any folder; its files (the ones directly in it: no dot files, nothing below it) are stored as assets/NAME,
 * whatever DIR is called on disk. The archive then has the set (pictures, specs.txt) and assets/ side by side, and
 * `zfrag mount` shows assets/ in native/, readable at no cost next to the video in virtual/. DIR is relative to -C,
 * like the list. The files go in natural order. */
#ifndef ZFRAG_ASSETS_H
#define ZFRAG_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#define ASSETS_NAME "assets"

/* paths: the list, a malloc'd array of n strings (grown here). *alias: made here, n + added entries; NULL for the list's
 * own, and the name to store for an asset (the list's path is where it is read from). dir: the folder the list's paths are
 * relative to, or NULL. The strings added live in one block returned in *pool, which the caller frees after the list is
 * done with. Returns the new count, or (size_t)-1 after saying why (a folder that is not there is an error; an empty one
 * is a warning). *nadded and *bytes say what was added. */
size_t assets_add(char ***paths, char ***alias, size_t n, const char *dir, const char *folder,
		  char **pool, size_t *nadded, uint64_t *bytes);

#endif
