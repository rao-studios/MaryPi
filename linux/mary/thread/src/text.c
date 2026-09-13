#include "thread/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t thread_utf8_count(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}

size_t thread_utf8_prefix(const char *s, size_t count) {
    const unsigned char *p = (const unsigned char *)s;
    size_t seen = 0, i = 0;
    for (; p[i]; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            if (seen == count) break;
            seen++;
        }
    }
    return i;
}

size_t thread_space_len(const unsigned char *p) {
    if (*p == ' ' || (*p >= 0x09 && *p <= 0x0D)) return 1;
    if (p[0] == 0xC2 && p[1] == 0xA0) return 2;
    if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) return 3;
    if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) return 3;
    return 0;
}

/* Lowercases in place where a byte-for-byte fold exists (ASCII, Latin-1 À–Þ). */
static void lower_in_place(unsigned char *s) {
    for (unsigned char *p = s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
        else if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {
            p[1] += 0x20;
            p++;
        }
    }
}

char *thread_normalize_name(const char *name) {
    size_t n = strlen(name);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t w = 0;
    int pending = 0;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        size_t ws = thread_space_len(p);
        if (ws) {
            if (w) pending = 1;
            p += ws;
            continue;
        }
        if (pending) {
            out[w++] = ' ';
            pending = 0;
        }
        out[w++] = (char)*p++;
    }
    out[w] = 0;
    lower_in_place((unsigned char *)out);
    return out;
}

char *thread_lower_trim(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    size_t ws;
    while ((ws = thread_space_len(p))) p += ws;
    size_t n = strlen((const char *)p);
    while (n) {
        /* trim trailing whitespace: step back over a whitespace sequence */
        size_t back = 0;
        for (size_t k = 1; k <= 3 && k <= n; k++) {
            if (thread_space_len(p + n - k) == k) back = k;
        }
        if (!back) break;
        n -= back;
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = 0;
    lower_in_place((unsigned char *)out);
    return out;
}

void thread_normalize_kind(const char *kind, char out[THREAD_KIND_MAX]) {
    char *k = thread_lower_trim(kind ? kind : "");
    snprintf(out, THREAD_KIND_MAX, "%s", k && *k ? k : "concept");
    free(k);
}

/* MARK: - Chunker */

typedef struct strings {
    char **v;
    size_t n, cap;
} strings;

static int strings_push(strings *s, char *item) {
    if (!item) return -1;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        char **v = realloc(s->v, cap * sizeof *v);
        if (!v) {
            free(item);
            return -1;
        }
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = item;
    return 0;
}

void thread_strings_free(char **v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* Trims `set` (1: whitespace and newlines; 0: spaces and tabs only) from both ends. Heap. */
static char *trimmed(const char *s, size_t len, int newlines) {
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    for (;;) {
        size_t ws = p < end ? thread_space_len(p) : 0;
        if (!ws || (!newlines && (*p == '\n' || *p == '\r'))) break;
        p += ws;
    }
    while (end > p) {
        size_t back = 0;
        for (size_t k = 1; k <= 3 && (size_t)(end - p) >= k; k++)
            if (thread_space_len(end - k) == k) back = k;
        if (!back || (!newlines && (end[-1] == '\n' || end[-1] == '\r'))) break;
        end -= back;
    }
    size_t n = (size_t)(end - p);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

static int append_joined(char **buf, const char *sep, const char *piece) {
    size_t a = strlen(*buf), b = strlen(sep), c = strlen(piece);
    char *joined = realloc(*buf, a + b + c + 1);
    if (!joined) return -1;
    memcpy(joined + a, sep, b);
    memcpy(joined + a + b, piece, c + 1);
    *buf = joined;
    return 0;
}

static void hard_split(strings *out, const char *text, size_t max_chars) {
    size_t total = thread_utf8_count(text);
    for (size_t start = 0; start < total; start += max_chars) {
        size_t from = thread_utf8_prefix(text, start);
        size_t to = thread_utf8_prefix(text, start + max_chars);
        char *piece = malloc(to - from + 1);
        if (!piece) return;
        memcpy(piece, text + from, to - from);
        piece[to - from] = 0;
        strings_push(out, piece);
    }
}

static void split_by_sentence(strings *out, const char *text, size_t max_chars) {
    strings sentences = { 0 };
    size_t len = strlen(text), start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '.' && (i + 1 == len || text[i + 1] == ' ' || text[i + 1] == '\n')) {
            strings_push(&sentences, trimmed(text + start, i + 1 - start, 0));
            start = i + 1;
        }
    }
    if (start < len) {
        char *rest = trimmed(text + start, len - start, 0);
        if (rest && *rest) strings_push(&sentences, rest);
        else free(rest);
    }
    char *buf = NULL;
    for (size_t s = 0; s < sentences.n; s++) {
        strings pieces = { 0 };
        if (thread_utf8_count(sentences.v[s]) <= max_chars) strings_push(&pieces, strdup(sentences.v[s]));
        else hard_split(&pieces, sentences.v[s], max_chars);
        for (size_t p = 0; p < pieces.n; p++) {
            const char *piece = pieces.v[p];
            if (!buf) buf = strdup(piece);
            else if (thread_utf8_count(buf) + 1 + thread_utf8_count(piece) <= max_chars) append_joined(&buf, " ", piece);
            else {
                strings_push(out, buf);
                buf = strdup(piece);
            }
        }
        thread_strings_free(pieces.v, pieces.n);
    }
    if (buf) strings_push(out, buf);
    thread_strings_free(sentences.v, sentences.n);
}

size_t thread_chunk(const char *text, size_t max_chars, char ***out) {
    strings chunks = { 0 };
    *out = NULL;
    if (!text) return 0;
    if (!max_chars) max_chars = THREAD_CHUNK_MAX_CHARS;
    char *buf = NULL;
    const char *p = text;
    while (*p) {
        const char *sep = strstr(p, "\n\n");
        size_t plen = sep ? (size_t)(sep - p) : strlen(p);
        char *para = trimmed(p, plen, 1);
        p = sep ? sep + 2 : p + plen;
        if (!para) continue;
        if (!*para) {
            free(para);
            continue;
        }
        strings segments = { 0 };
        if (thread_utf8_count(para) <= max_chars) strings_push(&segments, para);
        else {
            split_by_sentence(&segments, para, max_chars);
            free(para);
        }
        for (size_t s = 0; s < segments.n; s++) {
            const char *seg = segments.v[s];
            if (!buf) buf = strdup(seg);
            else if (thread_utf8_count(buf) + 2 + thread_utf8_count(seg) <= max_chars) append_joined(&buf, "\n\n", seg);
            else {
                strings_push(&chunks, buf);
                buf = strdup(seg);
            }
        }
        thread_strings_free(segments.v, segments.n);
    }
    if (buf) strings_push(&chunks, buf);
    *out = chunks.v;
    return chunks.n;
}

/* MARK: - Tags */

static const char *const STOPWORDS[] = {
    "about", "above", "after", "again", "against", "also", "among", "another",
    "before", "being", "below", "between", "both", "been", "because",
    "cannot", "could", "dure", "each", "either", "even",
    "from", "further", "have", "having", "here", "however",
    "into", "itself", "just", "like", "many", "more", "most", "much",
    "need", "neither", "none", "only", "onto", "other", "otherwise", "over", "own",
    "same", "should", "since", "some", "such", "than", "that", "them",
    "then", "there", "therefore", "these", "they", "this", "those", "through",
    "thus", "time", "under", "until", "upon", "used", "very", "well",
    "were", "what", "when", "where", "which", "while", "will", "with",
    "within", "without", "would", "your", "yours",
};

static bool is_stopword(const char *w) {
    for (size_t i = 0; i < sizeof STOPWORDS / sizeof STOPWORDS[0]; i++) if (strcmp(STOPWORDS[i], w) == 0) return true;
    return false;
}

typedef struct wordcount {
    char *word;
    size_t count;
} wordcount;

typedef struct wordmap {
    wordcount *slots;
    size_t cap, n;
} wordmap;

static unsigned hash_word(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int wordmap_add(wordmap *m, const char *word, size_t len) {
    if (m->n * 2 >= m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 64;
        wordcount *slots = calloc(cap, sizeof *slots);
        if (!slots) return -1;
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->slots[i].word) continue;
            size_t j = hash_word(m->slots[i].word) % cap;
            while (slots[j].word) j = (j + 1) % cap;
            slots[j] = m->slots[i];
        }
        free(m->slots);
        m->slots = slots;
        m->cap = cap;
    }
    char key[len + 1];
    memcpy(key, word, len);
    key[len] = 0;
    size_t j = hash_word(key) % m->cap;
    while (m->slots[j].word) {
        if (strcmp(m->slots[j].word, key) == 0) {
            m->slots[j].count++;
            return 0;
        }
        j = (j + 1) % m->cap;
    }
    m->slots[j].word = strdup(key);
    m->slots[j].count = 1;
    m->n++;
    return m->slots[j].word ? 0 : -1;
}

static int compare_wordcount(const void *a, const void *b) {
    const wordcount *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return strcmp(x->word, y->word);
}

/* Alphanumeric as CharacterSet.alphanumerics sees the common cases: ASCII letters and
 * digits, and every non-ASCII byte (letters in other scripts stay inside a token). */
static bool is_alnum_byte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

size_t thread_tags(const char *const *texts, size_t n, char ***out) {
    *out = NULL;
    wordmap map = { 0 };
    for (size_t t = 0; t < n; t++) {
        char *lower = strdup(texts[t] ? texts[t] : "");
        if (!lower) continue;
        lower_in_place((unsigned char *)lower);
        const char *p = lower;
        while (*p) {
            while (*p && !is_alnum_byte((unsigned char)*p)) p++;
            const char *start = p;
            while (*p && is_alnum_byte((unsigned char)*p)) p++;
            size_t len = (size_t)(p - start);
            if (!len) continue;
            char word[len + 1];
            memcpy(word, start, len);
            word[len] = 0;
            if (thread_utf8_count(word) > 3 && !is_stopword(word)) wordmap_add(&map, word, len);
        }
        free(lower);
    }
    wordcount *all = malloc((map.n ? map.n : 1) * sizeof *all);
    size_t count = 0;
    for (size_t i = 0; all && i < map.cap; i++) if (map.slots[i].word) all[count++] = map.slots[i];
    free(map.slots);
    if (!all) return 0;
    qsort(all, count, sizeof *all, compare_wordcount);
    size_t keep = count < THREAD_TAGS_MAX ? count : THREAD_TAGS_MAX;
    char **tags = keep ? malloc(keep * sizeof *tags) : NULL;
    for (size_t i = 0; i < count; i++) {
        if (tags && i < keep) tags[i] = all[i].word;
        else free(all[i].word);
    }
    free(all);
    *out = tags;
    return tags ? keep : 0;
}
