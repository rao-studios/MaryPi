/* The files under the home that get a record: every regular file, except under a
 * directory or name that starts with a dot (.cache, .config, the Trash under
 * .local), on another filesystem than the home, through a symlink, or larger than
 * IX_FILE_MAX. Paths are absolute. */
#ifndef MARY_INDEX_WALK_H
#define MARY_INDEX_WALK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IX_FILE_MAX (32u << 20)

/* Return nonzero to stop the walk. */
typedef int (*ix_walk_fn)(const char *path, int64_t size, int64_t mtime_ms, void *user);

/* Whether `path` (absolute, under root) would be walked: not dotted, not too big. */
bool ix_walk_admits(const char *root, const char *path, int64_t size);
/* Walks `root` depth first, directories in name order. The number of files seen, or -errno
 * when root cannot be opened. */
long ix_walk(const char *root, ix_walk_fn fn, void *user);

#endif
