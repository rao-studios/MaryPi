/* Gita on MaryOS: who the reply drew on, and where (Sewn/Sources/Gita/Gita+Royalty.swift,
 * Gita+Spans.swift, Gita+MarkerSpans.swift). The contribution names each owner's
 * retrieved documents, their influence (word share) and royalty; annotation turns the
 * finished reply into spans — exact ones from the [[n]] markers the model was asked to
 * append, and heuristic ones (n-gram overlap) for unmarked sentences — and strips the
 * markers so the user never sees them. Offsets are Unicode code points into the
 * visible reply. Royalty's pricing, the wallet and peers are not ported (gita/). */
#ifndef MARY_SEWN_ATTRIBUTION_H
#define MARY_SEWN_ATTRIBUTION_H

#include <stdbool.h>
#include <stddef.h>

#include "sewn/retrieve.h"

struct json_object;

typedef struct sewn_span {
    size_t lower, upper;    /* code points, [lower, upper) */
} sewn_span;

typedef struct sewn_spans {
    sewn_span *v;
    size_t n, cap;
} sewn_spans;

void sewn_spans_add(sewn_spans *s, size_t lower, size_t upper);
void sewn_spans_free(sewn_spans *s);
/* Sorted, overlapping and touching spans merged (Gita.mergeSpans). */
void sewn_spans_merge(sewn_spans *s);

/* Gita.Owner. */
typedef struct sewn_owner {
    char *thread_id;                /* "" locally */
    char *owner_id;
    char **document_ids;
    double *influence;              /* per document id, summing to 1 */
    size_t n_documents;
    double royalty;
    sewn_spans spans;
    struct json_object *document_spans; /* {document_id: [{lower, upper}]} or NULL (exact only) */
} sewn_owner;

typedef struct sewn_contribution {
    sewn_owner *owners;
    size_t n;
} sewn_contribution;

/* Gita.royalty(for:): word counts per document, split among owners; empty for no partitions. */
void sewn_contribution_build(const sewn_partition *partitions, size_t n, const char *thread_id, sewn_contribution *out);
void sewn_contribution_free(sewn_contribution *c);
/* {owners:[{thread_id, owner_id, document_ids, influence, royalty, spans, document_spans}]} */
struct json_object *sewn_contribution_json(const sewn_contribution *c);

/* MARK: Gita+Spans */

/* Sentences with code-point offsets (Gita.splitSentences). */
typedef struct sewn_sentence {
    size_t lower, upper;    /* into the text */
    size_t byte_lower, byte_upper;
} sewn_sentence;
size_t sewn_split_sentences(const char *text, sewn_sentence **out);
/* Lowercased, punctuation to spaces, whitespace collapsed (Gita.normalise). Heap. */
char *sewn_normalise(const char *text);
/* Markdown syntax removed (Gita.stripMarkdown). Heap. */
char *sewn_strip_markdown(const char *text);
/* Words > 1 char, not stop words, not all digits, from a normalised text. Heap array of heap strings. */
size_t sewn_content_words(const char *normalised, char ***out);
void sewn_words_free(char **words, size_t n);
double sewn_overlap_coefficient(char **a, size_t na, char **b, size_t nb);

/* Gita.extractCitations: sentences of the briefing that name a partition's source, their
 * content words per document id → [{document_id, key_words}]. */
struct json_object *sewn_extract_citations(const char *compact_text, const sewn_partition *partitions, size_t n, const char *owner);
/* Gita.computeSpans: heuristic spans on every owner of `c`, from the reply's text. */
void sewn_compute_spans(const char *response, sewn_contribution *c, const sewn_partition *partitions, size_t n, struct json_object *citations);

/* MARK: Gita+MarkerSpans */

/* Gita.parseMarkers: *visible is the reply without [[n]] markers (heap); document_spans
 * {document_id: [{lower, upper}]} for the sentences the markers ended; the number of
 * markers seen. source_index: [document_id, …], tag n = index n-1. */
int sewn_parse_markers(const char *text, struct json_object *source_index, char **visible, struct json_object **document_spans);
/* Gita.annotate: markers parsed and stripped, the heuristic run over the stripped text,
 * exact spans winning where they overlap. *visible is heap. */
void sewn_annotate(const char *response, sewn_contribution *c, const sewn_partition *partitions, size_t n, struct json_object *citations,
                   struct json_object *source_index, char **visible);

/* Gita.MarkerStreamFilter: strips markers from streamed deltas, holding back a suffix
 * that could still become one. */
typedef struct sewn_marker_filter {
    char held[16];
    size_t held_len;
} sewn_marker_filter;
void sewn_marker_filter_init(sewn_marker_filter *f);
/* Returns what may go out now (heap, possibly ""); the caller frees. */
char *sewn_marker_filter_feed(sewn_marker_filter *f, const char *delta, size_t len);
char *sewn_marker_filter_finish(sewn_marker_filter *f);

#endif
