/* MaryAmbient/Ambient/Models/AmbientAge.swift, AmbientSurface+Rendering.swift and
 * AmbientFact+Rendering.swift in C: one phrasing, two readers — the prompt and the
 * Ambient app both call these. Bounds are never invented; the age is never omitted; a
 * budget loser degrades to a mention, never to silence. Lengths are code points. */
#ifndef MARY_AMBIENT_RENDER_H
#define MARY_AMBIENT_RENDER_H

#include <stddef.h>

#include "ambient/store.h"

#define MA_SURFACE_LINE_CAP 220
#define MA_NOTABLE_LIMIT 5
#define MA_SURFACE_LINE_MAX 1024
#define MA_MENTION_MAX 768
#define MA_BLOCK_MAX (MA_CONTENT_CAP + MA_MENTION_MAX)

/* "8s", "3m", "2h", "an unknown time". */
void ma_age_string(double seconds, char *out, size_t n);
/* Code points in a UTF-8 string. */
size_t ma_utf8_count(const char *s);
/* The first `points` code points, with "…" when it was cut. */
void ma_utf8_prefix(const char *s, size_t points, char *out, size_t n, bool ellipsis);

/* "On screen: TextEdit — "Notes" (front of 2 windows; focused: body text) — offering: Save, Bold, +38 more — seen 8s ago" */
void ma_surface_line(const ma_surface *s, double now, char *out, size_t n);

/* The prompt's and the pane's phrasing for WHAT a fact is: "their highlight", "the part about "tides"" … */
void ma_fact_slot_phrase(const ma_fact *f, const ma_roster *r, char *out, size_t n);
/* "characters 40–900 of 1200", "about 300 characters of 1200", "characters 40–900", or "". */
void ma_fact_bounds_phrase(const ma_fact *f, char *out, size_t n);
/* "seen 8s ago", plus ", so it may have moved on since" past the fresh window. */
void ma_fact_age_phrase(const ma_fact *f, double now, char *out, size_t n);
/* The one-line mention: handle, place, subject, slot phrase, bounds, age. */
void ma_fact_mention_line(const ma_fact *f, double now, const ma_roster *r, char *out, size_t n);
/* The full rendering: the mention as a header, then the text, within `limit` code points. */
void ma_fact_block(const ma_fact *f, double now, const ma_roster *r, size_t limit, char *out, size_t n);

#endif
