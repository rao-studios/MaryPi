#include "sewn/chunker.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* MARK: - Code points */

/* One UTF-8 sequence at s[i]; an invalid byte decodes as U+FFFD, one byte long. */
static size_t utf8_next(const char *s, size_t len, size_t i, uint32_t *cp) {
    const unsigned char *p = (const unsigned char *)s + i;
    size_t left = len - i;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && left >= 2 && (p[1] & 0xC0) == 0x80) {
        *cp = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && left >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && left >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

/* Unicode White_Space. */
static bool is_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

static bool is_upper(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 0xC0 && c <= 0xDE) return c != 0xD7;
    if (c >= 0x100 && c <= 0x137) return c % 2 == 0;
    if (c >= 0x139 && c <= 0x148) return c % 2 == 1;
    if (c >= 0x14A && c <= 0x177) return c % 2 == 0;
    if (c == 0x178 || c == 0x179 || c == 0x17B || c == 0x17D) return true;
    if (c >= 0x391 && c <= 0x3A9) return c != 0x3A2;
    if (c >= 0x400 && c <= 0x42F) return true;
    return false;
}

static bool is_unambiguous_terminator(uint32_t c) {
    switch (c) {
    case '!': case '?': case 0x2026: case 0x203C: case 0x2049: case 0xFF01: case 0xFF1F:
    case 0x0964: case 0x0965: case 0x3002: case 0xFE12:
        return true;
    default:
        return false;
    }
}

static bool is_period(uint32_t c) { return c == '.' || c == 0xFE52 || c == 0xFF0E; }

static bool is_closing(uint32_t c) {
    switch (c) {
    case '"': case '\'': case 0x2018: case 0x2019: case 0x201C: case 0x201D: case 0x00BB: case 0x203A:
    case ')': case ']': case '}':
        return true;
    default:
        return false;
    }
}

size_t sewn_sentence_prefix_len(const char *t, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t n = utf8_next(t, len, i, &cp);
        if (is_unambiguous_terminator(cp)) {
            size_t end = i + n;
            if (end < len) {
                uint32_t next;
                size_t m = utf8_next(t, len, end, &next);
                if (is_closing(next)) end += m;
            }
            return end;
        }
        if (is_period(cp)) {
            size_t after = i + n;
            if (after == len) return len;
            uint32_t next;
            size_t m = utf8_next(t, len, after, &next);
            if (is_space(next)) {
                size_t after_space = after + m;
                if (after_space == len) return after;
                uint32_t third;
                utf8_next(t, len, after_space, &third);
                if (is_upper(third)) return after;
            }
        }
        i += n;
    }
    return len;
}

/* Trims Unicode whitespace from both ends: [*start, *end). */
static void trim(const char *s, size_t len, size_t *start, size_t *end) {
    size_t a = 0, last_non_space_end = 0;
    bool leading = true;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        size_t n = utf8_next(s, len, i, &cp);
        if (!is_space(cp)) {
            if (leading) { a = i; leading = false; }
            last_non_space_end = i + n;
        }
        i += n;
    }
    *start = leading ? len : a;
    *end = leading ? len : last_non_space_end;
}

static int word_count(const char *s) {
    size_t len = strlen(s);
    int words = 0;
    bool in_word = false;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += utf8_next(s, len, i, &cp);
        bool space = is_space(cp);
        if (!space && !in_word) words++;
        in_word = !space;
    }
    return words;
}

/* MARK: - Chunking */

void sewn_chunker_init(sewn_chunker *c) {
    memset(c, 0, sizeof *c);
    c->first_chunk_sentences = 1;
    c->sentences_per_chunk = 2;
    c->max_words_per_chunk = 30;
}

static int push_sentence(sewn_chunker *c, const char *s, size_t len) {
    size_t a, b;
    trim(s, len, &a, &b);
    if (a == b) return 0;
    if (c->pending_count == c->pending_cap) {
        size_t cap = c->pending_cap ? c->pending_cap * 2 : 8;
        char **grown = realloc(c->pending, cap * sizeof *grown);
        if (!grown) return -ENOMEM;
        c->pending = grown;
        c->pending_cap = cap;
    }
    char *copy = malloc(b - a + 1);
    if (!copy) return -ENOMEM;
    memcpy(copy, s + a, b - a);
    copy[b - a] = 0;
    c->pending[c->pending_count++] = copy;
    return 0;
}

static int extract_complete_sentences(sewn_chunker *c) {
    for (;;) {
        size_t cut = sewn_sentence_prefix_len((const char *)c->buffer.data, c->buffer.len);
        if (cut >= c->buffer.len) return 0;
        int rc = push_sentence(c, (const char *)c->buffer.data, cut);
        if (rc) return rc;
        mc_buf_consume(&c->buffer, cut);
    }
}

static int drain_ready_chunks(sewn_chunker *c, bool force, sewn_chunk_fn fn, void *user) {
    while (c->pending_count) {
        int target = c->emitted_first_chunk ? c->sentences_per_chunk : c->first_chunk_sentences;
        size_t take = 0;
        int words = 0;
        while (take < c->pending_count && (int)take < target && words < c->max_words_per_chunk) {
            words += word_count(c->pending[take]);
            take++;
        }
        bool complete = (int)take == target || words >= c->max_words_per_chunk;
        if (!complete && !force) break;
        mc_buf chunk = { 0 };
        int rc = 0;
        for (size_t i = 0; i < take && rc == 0; i++) {
            if (i) rc = mc_buf_append(&chunk, " ", 1);
            if (rc == 0) rc = mc_buf_append_str(&chunk, c->pending[i]);
        }
        if (rc) {
            mc_buf_free(&chunk);
            return rc;
        }
        for (size_t i = 0; i < take; i++) free(c->pending[i]);
        memmove(c->pending, c->pending + take, (c->pending_count - take) * sizeof *c->pending);
        c->pending_count -= take;
        c->emitted_first_chunk = true;
        int stop = fn((const char *)chunk.data, chunk.len, user);
        mc_buf_free(&chunk);
        if (stop) return 1;
    }
    return 0;
}

int sewn_chunker_feed(sewn_chunker *c, const char *delta, size_t len, sewn_chunk_fn fn, void *user) {
    int rc = mc_buf_append(&c->buffer, delta, len);
    if (rc == 0) rc = extract_complete_sentences(c);
    return rc ? rc : drain_ready_chunks(c, false, fn, user);
}

static int collect(const char *chunk, size_t len, void *user) {
    mc_buf *joined = user;
    if (joined->len && mc_buf_append(joined, " ", 1) < 0) return -ENOMEM;
    return mc_buf_append(joined, chunk, len) < 0 ? -ENOMEM : 0;
}

int sewn_chunker_flush(sewn_chunker *c, sewn_chunk_fn fn, void *user) {
    int rc = c->buffer.len ? push_sentence(c, (const char *)c->buffer.data, c->buffer.len) : 0;
    mc_buf_clear(&c->buffer);
    if (rc) return rc;
    mc_buf joined = { 0 };
    rc = drain_ready_chunks(c, true, collect, &joined);
    if (rc == 0 && joined.len) rc = fn((const char *)joined.data, joined.len, user) ? 1 : 0;
    mc_buf_free(&joined);
    return rc;
}

void sewn_chunker_free(sewn_chunker *c) {
    for (size_t i = 0; i < c->pending_count; i++) free(c->pending[i]);
    free(c->pending);
    mc_buf_free(&c->buffer);
    memset(c, 0, sizeof *c);
}

/* MARK: - Speaking markdown */

#include <regex.h>

/* Every non-overlapping match of `pattern`, left to right, replaced by group 1
 * (keep) or nothing — replacingOccurrences(of:with:options:.regularExpression).
 * `^` matches only at the start of the text, as it does there. */
static char *replace_all(char *in, const char *pattern, bool keep_group) {
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return in;
    mc_buf out = { 0 };
    size_t off = 0, len = strlen(in);
    regmatch_t m[2];
    int failed = 0;
    while (off <= len && regexec(&re, in + off, 2, m, off ? REG_NOTBOL : 0) == 0) {
        failed |= mc_buf_append(&out, in + off, (size_t)m[0].rm_so);
        if (keep_group && m[1].rm_so >= 0) failed |= mc_buf_append(&out, in + off + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
        if (m[0].rm_eo == m[0].rm_so) {
            if (off + (size_t)m[0].rm_eo >= len) { off = len + 1; break; }
            failed |= mc_buf_append(&out, in + off + m[0].rm_eo, 1);
            off += (size_t)m[0].rm_eo + 1;
        } else {
            off += (size_t)m[0].rm_eo;
        }
    }
    if (off < len) failed |= mc_buf_append(&out, in + off, len - off);
    regfree(&re);
    if (failed) {
        mc_buf_free(&out);
        return in;
    }
    free(in);
    if (!out.data) return strdup("");
    return (char *)out.data;
}

char *sewn_tts_sanitize(const char *text) {
    char *s = strdup(text ? text : "");
    if (!s) return NULL;
    s = replace_all(s, "\\[([^]]+)\\]\\([^)]*\\)", true);    /* [text](url) → text */
    s = replace_all(s, "`{1,3}([^`]*)`{1,3}", true);         /* inline and fenced code */
    s = replace_all(s, "\\*{1,3}([^*]+)\\*{1,3}", true);     /* *emphasis* */
    s = replace_all(s, "_{1,3}([^_]+)_{1,3}", true);         /* _emphasis_ */
    s = replace_all(s, "^#{1,6}[[:space:]]+", false);        /* a heading */
    s = replace_all(s, "^[[:space:]]*[-*+][[:space:]]+", false);   /* a list bullet */
    if (!s) return NULL;
    size_t a, b, len = strlen(s);
    trim(s, len, &a, &b);
    memmove(s, s + a, b - a);
    s[b - a] = 0;
    return s;
}
