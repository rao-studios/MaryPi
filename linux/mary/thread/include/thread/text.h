/* Thread's text utilities in C: GraphStore's name and kind normalization,
 * TextChunker (Utilities/TextChunker.swift) and TagGenerator
 * (Utilities/TagGenerator.swift). Lengths count Unicode code points where Swift
 * counts Characters, and case folds ASCII and Latin-1 where Swift folds every
 * script (PORTING.md deviation 11). */
#ifndef MARY_THREAD_TEXT_H
#define MARY_THREAD_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#define THREAD_CHUNK_MAX_CHARS 1500
#define THREAD_TAGS_MAX 10
#define THREAD_KIND_MAX 64

/* Code points in a UTF-8 string. */
size_t thread_utf8_count(const char *s);
/* The byte length of the first `count` code points. */
size_t thread_utf8_prefix(const char *s, size_t count);
/* Whitespace as Swift's .whitespacesAndNewlines: ASCII, NBSP, U+2028/9, U+3000. 0 when not. */
size_t thread_space_len(const unsigned char *p);

/* Trimmed, lowercased, whitespace collapsed to one space. Heap. */
char *thread_normalize_name(const char *name);
/* Trimmed and lowercased; empty becomes "concept". */
void thread_normalize_kind(const char *kind, char out[THREAD_KIND_MAX]);
/* Trimmed and lowercased (a predicate). Heap. */
char *thread_lower_trim(const char *s);

/* TextChunker.chunk: paragraphs ("\n\n"), then sentences (". " / ".\n"), then a hard
 * split, packed to at most max_chars code points. *out is a heap array of heap
 * strings; returns the count (0 with *out NULL for blank text). */
size_t thread_chunk(const char *text, size_t max_chars, char ***out);
/* TagGenerator.generate: the ten most frequent words longer than three characters,
 * minus stopwords, ties by the word. */
size_t thread_tags(const char *const *texts, size_t n, char ***out);
void thread_strings_free(char **v, size_t n);

#endif
