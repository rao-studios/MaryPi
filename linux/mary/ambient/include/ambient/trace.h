/* MaryAmbient/Ambient/Engine/AmbientTraceLog.swift and MaryBrain/Trace/RetrievalTraceLedger.swift
 * in C: what the engine decided and what it cost, one row per turn, newest first, in a ring of
 * fifty; beside each row, what retrieval was asked for each purpose and what came back. The
 * Ambient app's Routes, Realms and Runs tabs and the Threads app's Retrieval tab read it
 * (`trace.list` on maryd's socket); `ma_trace_report` is the Mac's RouteReport, pasteable. */
#ifndef MARY_AMBIENT_TRACE_H
#define MARY_AMBIENT_TRACE_H

#include <stdbool.h>

#include "ambient/engine.h"

#define MA_TRACE_CAPACITY 50
#define MA_TRACE_RUNS 10
#define MA_TRACE_PURPOSES 3
#define MA_TRACE_RETURNED 12
#define MA_TRACE_PACKAGES 16

typedef struct ma_skill_run {
    char app[MA_ID_MAX], skill[MA_ID_MAX];
    char invocation[96];            /* "textedit.save" */
    char status[16];                /* running | completed | failed | blocked | cancelled */
    char effect[16];                /* read | act | destructive */
    double started, finished;       /* 0: not yet */
    bool found_nothing;
    char args[240];                 /* a summary: keys and short values, never raw documents */
    char result[240];
} ma_skill_run;

typedef struct ma_retrieved {
    char document_id[128], group_id[128], family[24], lane[16];
    double score;
} ma_retrieved;

typedef struct ma_retrieval_purpose {
    char name[16];                  /* routing | orchestration | context */
    char lanes[4][16];
    int lane_count;
    ma_retrieved returned[MA_TRACE_RETURNED];
    int returned_count;
    char warning[120];              /* "asked nothing back" … */
} ma_retrieval_purpose;

typedef struct ma_trace_record {
    char id[64];                    /* the turn's request id */
    double date;                    /* seconds since the epoch */
    char utterance[512];
    ma_route route;
    int system_prompt_chars;
    int exposed_skill_count;
    char packages[MA_TRACE_PACKAGES][MA_ID_MAX];
    int package_count;
    ma_skill_run runs[MA_TRACE_RUNS];
    int run_count;
    ma_place co_active[MA_CO_ACTIVE_MAX];
    int co_active_count;
    ma_place glanced[MA_CO_ACTIVE_MAX];
    int glanced_count;
    ma_retrieval_purpose purposes[MA_TRACE_PURPOSES];
    int purpose_count;
    char contribution[200];         /* "2 owners, 3 documents, 4 passages", or "" */
    char confirmation[16];          /* "" | pending | allowed | refused */
} ma_trace_record;

typedef struct ma_trace_log ma_trace_log;

ma_trace_log *ma_trace_log_new(int capacity);       /* 0: MA_TRACE_CAPACITY */
void ma_trace_log_free(ma_trace_log *log);
void ma_trace_push(ma_trace_log *log, const ma_trace_record *record);
/* Attaches a run to the turn; raw interaction values never enter the ledger. */
void ma_trace_note_skill_invocation(ma_trace_log *log, const char *turn_id, const char *app, const char *skill, const char *effect, const char *args, double now);
void ma_trace_note_skill_result(ma_trace_log *log, const char *turn_id, const char *app, const char *skill, const char *status, bool found_nothing, const char *result, double now);
void ma_trace_note_retrieval(ma_trace_log *log, const char *turn_id, const ma_retrieval_purpose *purpose);
void ma_trace_note_contribution(ma_trace_log *log, const char *turn_id, const char *summary);
void ma_trace_note_confirmation(ma_trace_log *log, const char *turn_id, const char *state);
int ma_trace_count(const ma_trace_log *log);
/* Newest first; NULL past the end. Borrowed until the next record. */
const ma_trace_record *ma_trace_entry(const ma_trace_log *log, int index);
const ma_trace_record *ma_trace_find(const ma_trace_log *log, const char *turn_id);
void ma_trace_clear(ma_trace_log *log);
/* A "voice spoke, nothing mutated" tally: observation only. */
void ma_trace_note_voice_without_mutation(ma_trace_log *log);
int ma_trace_voice_without_mutation(const ma_trace_log *log);

struct mc_buf;
/* RouteReport.serialize: the whole log, newest first, as deterministic key: value lines. */
int ma_trace_report(const ma_trace_log *log, const ma_roster *r, double now, struct mc_buf *out);
void ma_trace_age_string(double seconds, char *out, size_t n);   /* "1.2s", "3m 12s", "1h 3m" */

#endif
