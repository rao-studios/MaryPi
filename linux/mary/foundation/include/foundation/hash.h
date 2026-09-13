/* The hashes Mary mints identities with, byte for byte:
 *
 *   FNV-1a 64 as %016llx      ThreadMemoryTopology.hash, UnitIndexHashing.stableHash,
 *                             DepositSubject.stableHash — the mary-* document and group ids
 *   djb2 over UTF-8           StableHash.of (Home/Views/Highlight/ContributionSpans.swift) —
 *                             the seed of a brush stroke, wrapping like Swift's Int
 *   SHA-256 as decimal bytes  Database.computeNumericHash (Thread/Sources/Database/
 *                             Database+Utilities.swift) — every Thread document, partition,
 *                             entity and relationship id: "%02d" per byte, so a byte of 100 or
 *                             more takes three digits and the string is 64 to 96 characters
 *
 * canonical() is the whitespace-collapsing lowercase the FNV callers apply first. */
#ifndef MARY_FOUNDATION_HASH_H
#define MARY_FOUNDATION_HASH_H

#include <stddef.h>
#include <stdint.h>

#define MF_NUMERIC_HASH_MAX 97      /* 32 bytes × up to 3 digits, and the NUL */

uint64_t mf_fnv1a64(const char *s);
/* 16 lowercase hex digits and a NUL. */
void mf_fnv1a64_hex(const char *s, char out[17]);

/* Swift's `utf8.reduce(5381) { ($0 &* 33) &+ Int($1) }`. */
int64_t mf_djb2(const char *s);

/* SHA-256 of `len` bytes rendered as decimal bytes. */
void mf_numeric_hash(const void *bytes, size_t len, char out[MF_NUMERIC_HASH_MAX]);
void mf_numeric_hash_text(const char *text, char out[MF_NUMERIC_HASH_MAX]);

/* Runs of whitespace (ASCII, NBSP, U+2028/9, U+3000) become one space, the ends are
 * trimmed, and letters are lowercased (ASCII and Latin-1; Swift lowercases every
 * script). Heap; the caller frees. NULL out of memory. */
char *mf_canonical(const char *s);

#endif
