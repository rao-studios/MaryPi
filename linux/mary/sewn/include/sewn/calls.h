/* The calls ledger: every request sewnd sends out, one row each — never a body, never
 * the key. Rao's rule is that every network call on MaryOS lives in sewn and can be
 * inspected; this is where it is inspected: `sewnctl calls`, System Settings › Mary ›
 * Network activity. A ring of the last 500 in memory, and calls.jsonl under the state
 * directory, rotated at 1 MB. */
#ifndef MARY_SEWN_CALLS_H
#define MARY_SEWN_CALLS_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEWN_CALLS_RING 500
#define SEWN_CALLS_FILE_MAX (1u << 20)
#define SEWN_CALLS_FIELD 64

struct json_object;

/* The purpose vocabulary: chat | skills | embed | extract | summarize | route | speech | stt | voices | verify. */
typedef struct sewn_call_row {
    int64_t id;
    int64_t at_ms;                      /* wall clock */
    char provider[SEWN_CALLS_FIELD];
    char host[SEWN_CALLS_FIELD];
    char path[128];
    char purpose[SEWN_CALLS_FIELD];
    char request_id[SEWN_CALLS_FIELD];
    long status;                        /* HTTP status; 0 when none arrived */
    int64_t ms;
    int64_t bytes_out;
    int64_t bytes_in;
    char outcome[SEWN_CALLS_FIELD];     /* ok | cancelled | failed | stopped */
} sewn_call_row;

typedef struct sewn_calls {
    bool ready;                         /* initialised; a zeroed struct records nothing */
    pthread_mutex_t lock;
    sewn_call_row ring[SEWN_CALLS_RING];
    size_t head, count;
    int64_t next_id;
    char path[256];                     /* calls.jsonl; "" keeps the ring only */
} sewn_calls;

void sewn_calls_init(sewn_calls *c, const char *state_dir);
void sewn_calls_free(sewn_calls *c);
/* Appends the row (its id and at_ms filled here), to the ring and the file. */
int64_t sewn_calls_record(sewn_calls *c, sewn_call_row *row);
/* The newest `limit` rows (0: all in the ring), newest first, as JSON objects. */
struct json_object *sewn_calls_list(sewn_calls *c, int limit);
/* {count, by_purpose:{…}, by_provider:{…}, failed, bytes_out, bytes_in, first_ms, last_ms}. */
struct json_object *sewn_calls_stats(sewn_calls *c);

#endif
