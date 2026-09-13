/* Sewn/Sources/Utilities/StreamingSentenceChunker.swift and SentenceBoundary.swift
 * in C: streamed reply text in, speakable chunks out. The first chunk is one
 * sentence so audio starts as early as possible; later chunks batch two (or stop
 * at thirty words) so synthesis stays efficient. A sentence counts as complete
 * only when its terminator is strictly inside the buffer — a terminator at the
 * end may yet be followed by a closing quote, or be an abbreviation.
 *
 * Text is UTF-8. Where Swift asks a Character whether it is whitespace or
 * uppercase, this asks the code point: Unicode White_Space, and uppercase in
 * the Latin, Greek and Cyrillic blocks (the scripts Voxtral TTS speaks). */
#ifndef MARY_SEWN_CHUNKER_H
#define MARY_SEWN_CHUNKER_H

#include <stdbool.h>
#include <stddef.h>

#include "common/buf.h"

/* `chunk` is NUL-terminated and valid only during the call. Nonzero stops. */
typedef int (*sewn_chunk_fn)(const char *chunk, size_t len, void *user);

typedef struct sewn_chunker {
    int first_chunk_sentences;      /* 1 */
    int sentences_per_chunk;        /* 2 */
    int max_words_per_chunk;        /* 30 */
    mc_buf buffer;
    char **pending;
    size_t pending_count;
    size_t pending_cap;
    bool emitted_first_chunk;
} sewn_chunker;

void sewn_chunker_init(sewn_chunker *c);
/* feed(_:): 0, 1 when fn stopped, or -ENOMEM. */
int sewn_chunker_feed(sewn_chunker *c, const char *delta, size_t len, sewn_chunk_fn fn, void *user);
/* flushRemainder(): the trailing partial sentence and any batched sentences, as
 * one chunk (fn is not called when nothing is left). 0, 1, or -ENOMEM. */
int sewn_chunker_flush(sewn_chunker *c, sewn_chunk_fn fn, void *user);
void sewn_chunker_free(sewn_chunker *c);

/* SentenceBoundary.upToFirstSentence: the byte length of `text` up to and
 * including its first sentence boundary, or `len` when there is none. A period
 * is a boundary only before whitespace and an uppercase letter (or at the end). */
size_t sewn_sentence_prefix_len(const char *text, size_t len);

/* TTSTextSanitizer.sanitize: markdown that must not be spoken — links reduced to
 * their text, code and emphasis markers dropped, a leading heading mark or list
 * bullet removed — then trimmed. NULL when out of memory; the caller frees. */
char *sewn_tts_sanitize(const char *text);

#endif
