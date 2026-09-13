/* What a file is, the way the desktop's Finder decides it (MaryUI lp_files_kind and
 * lp_files_is_text): by extension first, then by looking at the bytes. */
#ifndef MARY_INDEX_KIND_H
#define MARY_INDEX_KIND_H

#include <stdbool.h>
#include <stddef.h>

/* text | code | image | audio | video | pdf | document */
const char *ix_kind_of(const char *path);
/* True for text and code kinds, and for an unknown extension whose first 8 KB is
 * UTF-8 with no NUL (an empty file counts as text). */
bool ix_is_text(const char *path);
/* The name after the last '/'. */
const char *ix_basename(const char *path);

#endif
