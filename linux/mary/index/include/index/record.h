/* A file's record, as threadd deposits it (thread/local.h `deposit`): text files
 * are chunked and embedded; images, audio, video and other documents get one line
 * naming them. Every record carries the file's path, kind, size, mtime and content
 * hash (so parity can be checked without reading the file again), and the graph
 * entities the family declares: the file, its folders, and the `in` edges between
 * them. Names are paths relative to the home, so two notes.txt in two folders are two
 * nodes. */
#ifndef MARY_INDEX_RECORD_H
#define MARY_INDEX_RECORD_H

#include <stddef.h>
#include <stdint.h>

struct json_object;

#define IX_GROUP_PREFIX "files-"
#define IX_GROUP_LABEL "Files"

/* SHA-256 of the file's bytes, lowercase hex. 0, or -errno. */
int ix_hash_file(const char *path, char out[65]);
/* `path` relative to `root` ("Documents/notes.txt"), or the whole path when it is not under root. */
const char *ix_relative(const char *root, const char *path);
/* The deposit request for `path` (absolute, under root) for `owner`. NULL with -errno
 * in *error when the file cannot be read. */
struct json_object *ix_record(const char *root, const char *owner, const char *path, int *error);
/* One entry of a parity page: {path, size, mtime_ms, hash}. */
struct json_object *ix_parity_entry(const char *path, int64_t size, int64_t mtime_ms, const char *hash);

#endif
