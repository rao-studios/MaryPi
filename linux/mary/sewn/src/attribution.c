#include "sewn/attribution.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/buf.h"
#include "common/json.h"

/* MARK: - Spans */

void sewn_spans_add(sewn_spans *s, size_t lower, size_t upper) {
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        sewn_span *grown = realloc(s->v, cap * sizeof *grown);
        if (!grown) return;
        s->v = grown;
        s->cap = cap;
    }
    s->v[s->n++] = (sewn_span){ lower, upper };
}

void sewn_spans_free(sewn_spans *s) {
    free(s->v);
    memset(s, 0, sizeof *s);
}

static int span_cmp(const void *a, const void *b) {
    const sewn_span *x = a, *y = b;
    if (x->lower != y->lower) return x->lower < y->lower ? -1 : 1;
    return x->upper < y->upper ? -1 : x->upper > y->upper;
}

void sewn_spans_merge(sewn_spans *s) {
    if (s->n < 2) return;
    qsort(s->v, s->n, sizeof *s->v, span_cmp);
    size_t out = 0;
    for (size_t i = 0; i < s->n; i++) {
        if (out && s->v[i].lower <= s->v[out - 1].upper) {
            if (s->v[i].upper > s->v[out - 1].upper) s->v[out - 1].upper = s->v[i].upper;
        } else {
            s->v[out++] = s->v[i];
        }
    }
    s->n = out;
}

static struct json_object *spans_json(const sewn_spans *s) {
    struct json_object *arr = json_object_new_array();
    for (size_t i = 0; i < s->n; i++) {
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "lower", json_object_new_int64((int64_t)s->v[i].lower));
        json_object_object_add(o, "upper", json_object_new_int64((int64_t)s->v[i].upper));
        json_object_array_add(arr, o);
    }
    return arr;
}

/* MARK: - Text helpers */

static bool is_space_byte(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* The code point at p and its byte length. */
static unsigned decode(const unsigned char *p, int *len) {
    if (*p < 0x80) { *len = 1; return *p; }
    if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *len = 2; return ((unsigned)(*p & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { *len = 3; return ((unsigned)(*p & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *len = 4;
        return ((unsigned)(*p & 0x07) << 18) | ((unsigned)(p[1] & 0x3F) << 12) | ((unsigned)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *len = 1;
    return *p;
}

static bool is_space_cp(unsigned cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v' || cp == 0xA0 || cp == 0x2028 || cp == 0x2029; }

/* Unicode's P* categories, as far as replies go: ASCII punctuation (not the symbols
 * $ + < = > ^ ` | ~), Latin-1's, and the general punctuation block's dashes and quotes. */
static bool is_punct_cp(unsigned cp) {
    if (cp < 0x80) return cp != 0 && strchr("!\"#%&'()*,-./:;?@[\\]_{}", (int)cp) != NULL;
    if (cp == 0xA1 || cp == 0xA7 || cp == 0xAB || cp == 0xB6 || cp == 0xB7 || cp == 0xBB || cp == 0xBF) return true;
    return (cp >= 0x2010 && cp <= 0x2027) || (cp >= 0x2030 && cp <= 0x205E) || (cp >= 0x3001 && cp <= 0x3003);
}

static size_t count_cps(const char *s, size_t bytes) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

size_t sewn_split_sentences(const char *text, sewn_sentence **out) {
    *out = NULL;
    size_t n = 0, cap = 0;
    size_t len = strlen(text);
    size_t seg_start = 0, seg_cp = 0;     /* the segment's byte start and code-point start */
    size_t cursor = 0, cursor_cp = 0;
    while (cursor < len) {
        int l;
        unsigned cp = decode((const unsigned char *)text + cursor, &l);
        size_t next = cursor + (size_t)l;
        bool brk = false;
        if ((cp == '.' || cp == '!' || cp == '?') && next < len && (text[next] == ' ' || text[next] == '\n')) brk = true;
        else if (cp == '\n' && next < len && text[next] == '\n') brk = true;
        if (brk) {
            size_t break_end = next + 1;   /* after the space or second newline */
            size_t break_cp = cursor_cp + 2;
            /* flush */
            bool blank = true;
            for (size_t i = seg_start; i < break_end; i++) if (!is_space_byte((unsigned char)text[i])) { blank = false; break; }
            if (!blank) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 8;
                    *out = realloc(*out, cap * sizeof **out);
                }
                (*out)[n++] = (sewn_sentence){ seg_cp, break_cp, seg_start, break_end };
            }
            seg_start = break_end;
            seg_cp = break_cp;
            cursor = break_end;
            cursor_cp = break_cp;
            continue;
        }
        cursor = next;
        cursor_cp++;
    }
    if (seg_start < len) {
        bool blank = true;
        for (size_t i = seg_start; i < len; i++) if (!is_space_byte((unsigned char)text[i])) { blank = false; break; }
        if (!blank) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                *out = realloc(*out, cap * sizeof **out);
            }
            (*out)[n++] = (sewn_sentence){ seg_cp, cursor_cp, seg_start, len };
        }
    }
    return n;
}

char *sewn_normalise(const char *text) {
    mc_buf b = { 0 };
    bool pending_space = false;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        int l;
        unsigned cp = decode(p, &l);
        if (is_punct_cp(cp) || is_space_cp(cp)) {
            pending_space = b.len > 0;
        } else {
            if (pending_space) mc_buf_append(&b, " ", 1);
            pending_space = false;
            if (cp < 0x80) {
                char c = (char)tolower((int)cp);
                mc_buf_append(&b, &c, 1);
            } else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) {
                /* Latin-1 capitals */
                unsigned low = cp + 32;
                char two[2] = { (char)(0xC0 | (low >> 6)), (char)(0x80 | (low & 0x3F)) };
                mc_buf_append(&b, two, 2);
            } else {
                mc_buf_append(&b, p, (size_t)l);
            }
        }
        p += l;
    }
    if (!b.data) mc_buf_append_str(&b, "");
    return (char *)b.data;
}

/* MARK: Markdown */

static bool at_line_start(const char *s, size_t i) { return i == 0 || s[i - 1] == '\n'; }

char *sewn_strip_markdown(const char *text) {
    /* a: fenced code → " " */
    mc_buf a = { 0 };
    for (const char *p = text; *p;) {
        if (strncmp(p, "```", 3) == 0) {
            const char *end = strstr(p + 3, "```");
            if (end) {
                mc_buf_append(&a, " ", 1);
                p = end + 3;
                continue;
            }
        }
        mc_buf_append(&a, p, 1);
        p++;
    }
    if (!a.data) mc_buf_append_str(&a, "");
    /* b: inline code → " "; c: images → " "; d: links → text */
    mc_buf b = { 0 };
    for (const char *p = (const char *)a.data; *p;) {
        if (*p == '`') {
            const char *end = strchr(p + 1, '`');
            if (end && end > p + 1) {
                mc_buf_append(&b, " ", 1);
                p = end + 1;
                continue;
            }
        }
        if ((*p == '!' && p[1] == '[') || *p == '[') {
            const char *open = *p == '!' ? p + 1 : p;
            const char *close = strchr(open + 1, ']');
            if (close && close[1] == '(' && (*p == '!' ? true : close > open + 1)) {
                const char *paren = strchr(close + 2, ')');
                bool clean = paren != NULL;
                for (const char *q = close + 2; clean && q < paren; q++) if (*q == '(') clean = false;
                if (clean) {
                    if (*p == '!') mc_buf_append(&b, " ", 1);
                    else mc_buf_append(&b, open + 1, (size_t)(close - open - 1));
                    p = paren + 1;
                    continue;
                }
            }
        }
        mc_buf_append(&b, p, 1);
        p++;
    }
    mc_buf_free(&a);
    if (!b.data) mc_buf_append_str(&b, "");
    /* e: headings; f: emphasis runs; g: blockquotes; h: list markers */
    const char *s = (const char *)b.data;
    mc_buf c = { 0 };
    for (size_t i = 0; s[i];) {
        if (at_line_start(s, i) && s[i] == '#') {
            size_t k = i;
            while (s[k] == '#' && k - i < 6) k++;
            while (is_space_byte((unsigned char)s[k]) && s[k] != '\n') k++;
            i = k;
            continue;
        }
        if (s[i] == '*' || s[i] == '_') {
            char ch = s[i];
            size_t k = i;
            while (s[k] == ch && k - i < 3) k++;
            mc_buf_append(&c, " ", 1);
            i = k;
            continue;
        }
        mc_buf_append(&c, s + i, 1);
        i++;
    }
    mc_buf_free(&b);
    if (!c.data) mc_buf_append_str(&c, "");
    s = (const char *)c.data;
    mc_buf d = { 0 };
    for (size_t i = 0; s[i];) {
        if (at_line_start(s, i)) {
            if (s[i] == '>') {
                size_t k = i + 1;
                while (is_space_byte((unsigned char)s[k]) && s[k] != '\n') k++;
                i = k;
                continue;
            }
            if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && is_space_byte((unsigned char)s[i + 1])) {
                size_t k = i + 1;
                while (is_space_byte((unsigned char)s[k])) k++;
                i = k;
                continue;
            }
            size_t k = i;
            while (isdigit((unsigned char)s[k])) k++;
            if (k > i && s[k] == '.' && is_space_byte((unsigned char)s[k + 1])) {
                k++;
                while (is_space_byte((unsigned char)s[k])) k++;
                i = k;
                continue;
            }
        }
        mc_buf_append(&d, s + i, 1);
        i++;
    }
    mc_buf_free(&c);
    if (!d.data) mc_buf_append_str(&d, "");
    return (char *)d.data;
}

/* MARK: Words */

static const char *const STOP_WORDS[] = {
    "a", "an", "the", "and", "or", "but", "if", "in", "on", "at", "to", "for", "of", "with", "by", "from", "is", "are", "was", "were",
    "be", "been", "being", "have", "has", "had", "do", "does", "did", "will", "would", "could", "should", "may", "might", "shall", "can",
    "that", "this", "these", "those", "it", "its", "as", "not", "no", "so", "yet", "both", "than", "then", "when", "where", "who", "which",
    "what", "how", "all", "each", "every", "any", "some", "their", "they", "them", "there", "we", "us", "our", "you", "your", "he", "she",
    "his", "her", "him", "i", "me", "my", "into", "about", "over", "also", "more", "very", "just", "like", "up", "out", "even", "back",
    "after", "through", "between", "much", "well", "most", "other", "while", "since", "within", "such", "only", "one", "two", "three",
    "because", "though", "here", "whether", "s", "t", "re", "ve", "ll", "d",
};

static bool is_stop_word(const char *w) {
    for (size_t i = 0; i < sizeof STOP_WORDS / sizeof STOP_WORDS[0]; i++) if (strcmp(w, STOP_WORDS[i]) == 0) return true;
    return false;
}

size_t sewn_content_words(const char *normalised, char ***out) {
    *out = NULL;
    size_t n = 0, cap = 0;
    const char *p = normalised;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != ' ') end++;
        size_t len = (size_t)(end - p);
        bool digits = true;
        for (size_t i = 0; i < len; i++) if (!isdigit((unsigned char)p[i])) { digits = false; break; }
        char *w = strndup(p, len);
        if (count_cps(w, len) > 1 && !is_stop_word(w) && !digits) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                *out = realloc(*out, cap * sizeof **out);
            }
            (*out)[n++] = w;
        } else {
            free(w);
        }
        p = end;
    }
    return n;
}

void sewn_words_free(char **words, size_t n) {
    for (size_t i = 0; i < n; i++) free(words[i]);
    free(words);
}

/* A set of strings: unique, for the overlap coefficient. */
struct set {
    char **v;
    size_t n;
};

static bool set_has(const struct set *s, const char *w) {
    for (size_t i = 0; i < s->n; i++) if (strcmp(s->v[i], w) == 0) return true;
    return false;
}

static void set_add(struct set *s, const char *w) {
    if (set_has(s, w)) return;
    s->v = realloc(s->v, (s->n + 1) * sizeof *s->v);
    s->v[s->n++] = strdup(w);
}

static void set_free(struct set *s) {
    sewn_words_free(s->v, s->n);
    memset(s, 0, sizeof *s);
}

static double overlap(const struct set *a, const struct set *b) {
    if (!a->n || !b->n) return 0;
    size_t both = 0;
    for (size_t i = 0; i < a->n; i++) if (set_has(b, a->v[i])) both++;
    size_t min = a->n < b->n ? a->n : b->n;
    return (double)both / (double)min;
}

double sewn_overlap_coefficient(char **a, size_t na, char **b, size_t nb) {
    struct set sa = { 0 }, sb = { 0 };
    for (size_t i = 0; i < na; i++) set_add(&sa, a[i]);
    for (size_t i = 0; i < nb; i++) set_add(&sb, b[i]);
    double r = overlap(&sa, &sb);
    set_free(&sa);
    set_free(&sb);
    return r;
}

#define NGRAM 3

static void ngrams(char **words, size_t n, struct set *out) {
    memset(out, 0, sizeof *out);
    if (n < NGRAM) return;
    for (size_t i = 0; i + NGRAM <= n; i++) {
        size_t len = 0;
        for (int k = 0; k < NGRAM; k++) len += strlen(words[i + k]) + 1;
        char *g = malloc(len + 1);
        g[0] = 0;
        for (int k = 0; k < NGRAM; k++) {
            if (k) strcat(g, " ");
            strcat(g, words[i + k]);
        }
        set_add(out, g);
        free(g);
    }
}

static void word_set(char **words, size_t n, struct set *out) {
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < n; i++) set_add(out, words[i]);
}

/* contentWords(normalise(stripMarkdown(text))) */
static size_t clean_words(const char *text, char ***out) {
    char *stripped = sewn_strip_markdown(text);
    char *norm = sewn_normalise(stripped);
    size_t n = sewn_content_words(norm, out);
    free(norm);
    free(stripped);
    return n;
}

/* MARK: - The contribution (Gita+Royalty) */

/* Swift's components(separatedBy: .whitespacesAndNewlines).count: one more than the whitespace characters. */
static size_t swift_word_count(const char *text) {
    size_t n = 1;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        int l;
        unsigned cp = decode(p, &l);
        if (is_space_cp(cp)) n++;
        p += l;
    }
    return n;
}

struct doc_count {
    const char *document_id;
    const char *owner_id;   /* the first partition's */
    size_t words;
};

void sewn_contribution_build(const sewn_partition *partitions, size_t n, const char *thread_id, sewn_contribution *out) {
    memset(out, 0, sizeof *out);
    if (!n) return;
    struct doc_count *docs = calloc(n, sizeof *docs);
    size_t nd = 0, total = 0;
    for (size_t i = 0; i < n; i++) {
        size_t words = swift_word_count(partitions[i].text);
        size_t k = 0;
        for (; k < nd; k++) if (strcmp(docs[k].document_id, partitions[i].document_id) == 0) break;
        if (k == nd) docs[nd++] = (struct doc_count){ partitions[i].document_id, partitions[i].owner_id, 0 };
        docs[k].words += words;
        total += words;
    }
    if (!total) {
        free(docs);
        return;
    }
    /* owners: keyed by owner id (the thread id when empty) */
    for (size_t k = 0; k < nd; k++) {
        const char *key = docs[k].owner_id && *docs[k].owner_id ? docs[k].owner_id : (thread_id ? thread_id : "");
        size_t o = 0;
        for (; o < out->n; o++) if (strcmp(out->owners[o].owner_id, key) == 0) break;
        if (o == out->n) {
            out->owners = realloc(out->owners, (out->n + 1) * sizeof *out->owners);
            sewn_owner *ow = &out->owners[out->n++];
            memset(ow, 0, sizeof *ow);
            ow->thread_id = strdup(thread_id ? thread_id : "");
            ow->owner_id = strdup(key);
        }
        sewn_owner *ow = &out->owners[o];
        ow->document_ids = realloc(ow->document_ids, (ow->n_documents + 1) * sizeof *ow->document_ids);
        ow->influence = realloc(ow->influence, (ow->n_documents + 1) * sizeof *ow->influence);
        ow->document_ids[ow->n_documents] = strdup(docs[k].document_id);
        ow->influence[ow->n_documents] = (double)docs[k].words;
        ow->n_documents++;
    }
    for (size_t o = 0; o < out->n; o++) {
        sewn_owner *ow = &out->owners[o];
        double owner_total = 0;
        for (size_t d = 0; d < ow->n_documents; d++) owner_total += ow->influence[d];
        ow->royalty = owner_total / (double)total;
        for (size_t d = 0; d < ow->n_documents; d++) ow->influence[d] /= owner_total;
    }
    free(docs);
}

void sewn_contribution_free(sewn_contribution *c) {
    for (size_t o = 0; o < c->n; o++) {
        sewn_owner *ow = &c->owners[o];
        free(ow->thread_id);
        free(ow->owner_id);
        for (size_t d = 0; d < ow->n_documents; d++) free(ow->document_ids[d]);
        free(ow->document_ids);
        free(ow->influence);
        sewn_spans_free(&ow->spans);
        if (ow->document_spans) json_object_put(ow->document_spans);
    }
    free(c->owners);
    memset(c, 0, sizeof *c);
}

struct json_object *sewn_contribution_json(const sewn_contribution *c) {
    struct json_object *o = json_object_new_object(), *owners = json_object_new_array();
    for (size_t i = 0; i < c->n; i++) {
        const sewn_owner *ow = &c->owners[i];
        struct json_object *j = json_object_new_object(), *ids = json_object_new_array(), *influence = json_object_new_object();
        json_object_object_add(j, "thread_id", json_object_new_string(ow->thread_id));
        json_object_object_add(j, "owner_id", json_object_new_string(ow->owner_id));
        for (size_t d = 0; d < ow->n_documents; d++) {
            json_object_array_add(ids, json_object_new_string(ow->document_ids[d]));
            json_object_object_add(influence, ow->document_ids[d], json_object_new_double(ow->influence[d]));
        }
        json_object_object_add(j, "document_ids", ids);
        json_object_object_add(j, "influence", influence);
        json_object_object_add(j, "royalty", json_object_new_double(ow->royalty));
        json_object_object_add(j, "spans", spans_json(&ow->spans));
        if (ow->document_spans) json_object_object_add(j, "document_spans", json_object_get(ow->document_spans));
        json_object_array_add(owners, j);
    }
    json_object_object_add(o, "owners", owners);
    return o;
}

/* MARK: - Citations (Gita.extractCitations) */

/* The name without its extension: URL.deletingPathExtension().lastPathComponent. */
static char *source_name(const char *name) {
    char *s = strdup(name ? name : "");
    char *dot = strrchr(s, '.');
    if (dot && dot != s && !strchr(dot, ' ') && !strchr(dot, '/')) *dot = 0;
    return s;
}

/* Sentences of `text` as normalised strings. */
static size_t normalised_sentences(const char *text, char ***out) {
    sewn_sentence *sents = NULL;
    size_t n = sewn_split_sentences(text, &sents);
    *out = n ? calloc(n, sizeof **out) : NULL;
    for (size_t i = 0; i < n; i++) {
        char *raw = strndup(text + sents[i].byte_lower, sents[i].byte_upper - sents[i].byte_lower);
        (*out)[i] = sewn_normalise(raw);
        free(raw);
    }
    free(sents);
    return n;
}

struct json_object *sewn_extract_citations(const char *compact_text, const sewn_partition *partitions, size_t n, const char *owner) {
    struct json_object *arr = json_object_new_array();
    if (!n || !compact_text || !*compact_text) return arr;
    /* the first <external>…</external> block */
    const char *open = strstr(compact_text, "<external>"), *close = open ? strstr(open, "</external>") : NULL;
    char *personal, *external;
    if (open && close) {
        size_t before = (size_t)(open - compact_text);
        const char *after = close + strlen("</external>");
        personal = malloc(before + strlen(after) + 1);
        memcpy(personal, compact_text, before);
        strcpy(personal + before, after);
        external = strndup(open + strlen("<external>"), (size_t)(close - open - strlen("<external>")));
    } else {
        personal = strdup(compact_text);
        external = strdup("");
    }
    char **ps = NULL, **es = NULL;
    size_t np = normalised_sentences(personal, &ps), ne = normalised_sentences(external, &es);
    for (size_t i = 0; i < n; i++) {
        char *raw_name = source_name(partitions[i].name);
        char *name = sewn_normalise(raw_name);
        free(raw_name);
        if (!*name) {
            free(name);
            continue;
        }
        bool mine = owner && strcasecmp(partitions[i].owner_id, owner) == 0;
        char **sents = mine ? ps : es;
        size_t ns = mine ? np : ne;
        struct json_object *entry = NULL, *words = NULL;
        for (size_t s = 0; s < ns; s++) {
            if (!strstr(sents[s], name)) continue;
            char **w = NULL;
            size_t nw = sewn_content_words(sents[s], &w);
            if (!entry) {
                /* one entry per document id */
                for (size_t e = 0; e < json_object_array_length(arr); e++) {
                    struct json_object *x = json_object_array_get_idx(arr, e);
                    if (strcmp(mc_json_string(x, "document_id"), partitions[i].document_id) == 0) {
                        entry = x;
                        words = mc_json_array(x, "key_words");
                    }
                }
                if (!entry) {
                    entry = json_object_new_object();
                    words = json_object_new_array();
                    json_object_object_add(entry, "document_id", json_object_new_string(partitions[i].document_id));
                    json_object_object_add(entry, "key_words", words);
                    json_object_array_add(arr, entry);
                }
            }
            for (size_t k = 0; k < nw; k++) json_object_array_add(words, json_object_new_string(w[k]));
            sewn_words_free(w, nw);
        }
        free(name);
    }
    sewn_words_free(ps, np);
    sewn_words_free(es, ne);
    free(personal);
    free(external);
    return arr;
}

/* MARK: - Chunks (Gita.computeSpans) */

struct chunk {
    size_t first, last;         /* sentence indices, inclusive */
    size_t lower, upper;        /* code points */
    char **words;
    size_t n_words;
    struct set features;
    struct set grams;
};

#define MIN_CONTENT_WORDS 5

static size_t build_chunks(const char *text, const sewn_sentence *sents, size_t ns, struct chunk **out) {
    *out = NULL;
    size_t n = 0;
    size_t i = 0;
    while (i < ns) {
        size_t last = i;
        mc_buf combined = { 0 };
        mc_buf_append(&combined, text + sents[i].byte_lower, sents[i].byte_upper - sents[i].byte_lower);
        char **words = NULL;
        size_t nw = clean_words((const char *)combined.data, &words);
        while (nw < MIN_CONTENT_WORDS && last + 1 < ns) {
            last++;
            mc_buf_append(&combined, " ", 1);
            mc_buf_append(&combined, text + sents[last].byte_lower, sents[last].byte_upper - sents[last].byte_lower);
            sewn_words_free(words, nw);
            nw = clean_words((const char *)combined.data, &words);
        }
        *out = realloc(*out, (n + 1) * sizeof **out);
        struct chunk *c = &(*out)[n++];
        memset(c, 0, sizeof *c);
        c->first = i;
        c->last = last;
        c->lower = sents[i].lower;
        c->upper = sents[last].upper;
        c->words = words;
        c->n_words = nw;
        word_set(words, nw, &c->features);
        ngrams(words, nw, &c->grams);
        mc_buf_free(&combined);
        i = last + 1;
    }
    return n;
}

static void chunks_free(struct chunk *chunks, size_t n) {
    for (size_t i = 0; i < n; i++) {
        sewn_words_free(chunks[i].words, chunks[i].n_words);
        set_free(&chunks[i].features);
        set_free(&chunks[i].grams);
    }
    free(chunks);
}

static bool chunk_claimed(const struct chunk *c, const bool *claimed) {
    for (size_t s = c->first; s <= c->last; s++) if (claimed[s]) return true;
    return false;
}

/* The citation key words for a document, from the [{document_id, key_words}] array. */
static size_t citation_words(struct json_object *citations, const char *document_id, char ***out) {
    *out = NULL;
    for (size_t i = 0; citations && i < json_object_array_length(citations); i++) {
        struct json_object *c = json_object_array_get_idx(citations, i);
        const char *id = mc_json_string(c, "document_id");
        if (!id || strcmp(id, document_id) != 0) continue;
        struct json_object *words = mc_json_array(c, "key_words");
        size_t n = words ? json_object_array_length(words) : 0;
        *out = n ? calloc(n, sizeof **out) : NULL;
        for (size_t k = 0; k < n; k++) (*out)[k] = strdup(json_object_get_string(json_object_array_get_idx(words, k)));
        return n;
    }
    return 0;
}

static const struct chunk *best_chunk(const char *node_text, char **compact_words, size_t n_compact, const struct chunk *chunks, size_t nc,
                                      const bool *claimed) {
    char **node_words = NULL;
    size_t nn = clean_words(node_text, &node_words);
    if (!nn) return NULL;
    struct set compact_grams, node_grams, node_features;
    ngrams(compact_words, n_compact, &compact_grams);
    ngrams(node_words, nn, &node_grams);
    word_set(node_words, nn, &node_features);
    double best_compact = 0, best_direct = 0, best_sim = 0;
    const struct chunk *cc = NULL, *dc = NULL, *sc = NULL;
    for (size_t i = 0; i < nc; i++) {
        const struct chunk *c = &chunks[i];
        if (chunk_claimed(c, claimed)) continue;
        if (compact_grams.n) {
            double s = overlap(&compact_grams, &c->grams);
            if (s > best_compact) { best_compact = s; cc = c; }
        }
        if (node_grams.n) {
            double s = overlap(&node_grams, &c->grams);
            if (s > best_direct) { best_direct = s; dc = c; }
        }
        double sim = overlap(&node_features, &c->features);
        if (sim > best_sim) { best_sim = sim; sc = c; }
    }
    set_free(&compact_grams);
    set_free(&node_grams);
    set_free(&node_features);
    sewn_words_free(node_words, nn);
    if (best_compact >= 0.25 && cc) return cc;
    if (best_direct >= 0.20 && dc) return dc;
    if (best_sim >= 0.15) return sc;
    return NULL;
}

static int chunk_ptr_cmp(const void *a, const void *b) {
    const struct chunk *x = *(const struct chunk *const *)a, *y = *(const struct chunk *const *)b;
    return x->first < y->first ? -1 : x->first > y->first;
}

static void merge_adjacent(const struct chunk **matched, size_t n, sewn_spans *out) {
    if (!n) return;
    qsort(matched, n, sizeof *matched, chunk_ptr_cmp);
    size_t lower = matched[0]->lower, upper = matched[0]->upper, max_idx = matched[0]->last;
    for (size_t i = 1; i < n; i++) {
        if (matched[i]->first <= max_idx + 1) {
            if (matched[i]->upper > upper) upper = matched[i]->upper;
            if (matched[i]->last > max_idx) max_idx = matched[i]->last;
        } else {
            sewn_spans_add(out, lower, upper);
            lower = matched[i]->lower;
            upper = matched[i]->upper;
            max_idx = matched[i]->last;
        }
    }
    sewn_spans_add(out, lower, upper);
}

static double max_influence(const sewn_owner *o) {
    double m = 0;
    for (size_t d = 0; d < o->n_documents; d++) if (o->influence[d] > m) m = o->influence[d];
    return m;
}

static bool owner_has(const sewn_owner *o, const char *document_id, double *influence) {
    for (size_t d = 0; d < o->n_documents; d++) {
        if (strcmp(o->document_ids[d], document_id) == 0) {
            if (influence) *influence = o->influence[d];
            return true;
        }
    }
    return false;
}

void sewn_compute_spans(const char *response, sewn_contribution *c, const sewn_partition *partitions, size_t n, struct json_object *citations) {
    if (!c->n || !response || !*response) return;
    sewn_sentence *sents = NULL;
    size_t ns = sewn_split_sentences(response, &sents);
    if (!ns) return;
    struct chunk *chunks = NULL;
    size_t nc = build_chunks(response, sents, ns, &chunks);
    bool *claimed = calloc(ns, sizeof *claimed);
    /* owners ranked by peak influence */
    sewn_owner **ranked = calloc(c->n, sizeof *ranked);
    for (size_t i = 0; i < c->n; i++) ranked[i] = &c->owners[i];
    for (size_t i = 1; i < c->n; i++)
        for (size_t k = i; k > 0 && max_influence(ranked[k]) > max_influence(ranked[k - 1]); k--) {
            sewn_owner *t = ranked[k];
            ranked[k] = ranked[k - 1];
            ranked[k - 1] = t;
        }
    for (size_t r = 0; r < c->n; r++) {
        sewn_owner *o = ranked[r];
        /* the owner's partitions, by influence */
        const sewn_partition **mine = calloc(n ? n : 1, sizeof *mine);
        double *infl = calloc(n ? n : 1, sizeof *infl);
        size_t nm = 0;
        for (size_t i = 0; i < n; i++) {
            double inf = 0;
            if (!owner_has(o, partitions[i].document_id, &inf)) continue;
            size_t k = nm++;
            while (k > 0 && infl[k - 1] < inf) {
                mine[k] = mine[k - 1];
                infl[k] = infl[k - 1];
                k--;
            }
            mine[k] = &partitions[i];
            infl[k] = inf;
        }
        const struct chunk **matched = calloc(nm ? nm : 1, sizeof *matched);
        size_t n_matched = 0;
        for (size_t i = 0; i < nm; i++) {
            char **cw = NULL;
            size_t ncw = citation_words(citations, mine[i]->document_id, &cw);
            const struct chunk *best = best_chunk(mine[i]->text, cw, ncw, chunks, nc, claimed);
            sewn_words_free(cw, ncw);
            if (!best) continue;
            for (size_t s = best->first; s <= best->last; s++) claimed[s] = true;
            matched[n_matched++] = best;
        }
        sewn_spans_free(&o->spans);
        merge_adjacent(matched, n_matched, &o->spans);
        free(matched);
        free(mine);
        free(infl);
    }
    free(ranked);
    free(claimed);
    chunks_free(chunks, nc);
    free(sents);
}

/* MARK: - Markers (Gita+MarkerSpans) */

/* A complete marker at p: "[[" 1-3 digits "]]". Its length, or 0. */
static size_t marker_at(const char *p, int *index) {
    if (p[0] != '[' || p[1] != '[') return 0;
    size_t i = 2;
    int value = 0;
    while (i < 5 && isdigit((unsigned char)p[i])) {
        value = value * 10 + (p[i] - '0');
        i++;
    }
    if (i == 2 || p[i] != ']' || p[i + 1] != ']') return 0;
    if (index) *index = value;
    return i + 2;
}

int sewn_parse_markers(const char *text, struct json_object *source_index, char **visible, struct json_object **document_spans) {
    *document_spans = NULL;
    mc_buf out = { 0 };
    struct marker { size_t offset; const char *document_id; } *markers = NULL;
    size_t n_markers = 0, count = 0, cps = 0;
    for (const char *p = text; *p;) {
        int index = 0;
        size_t len = marker_at(p, &index);
        if (len) {
            count++;
            const char *doc = source_index && index >= 1 && (size_t)index <= json_object_array_length(source_index)
                                  ? json_object_get_string(json_object_array_get_idx(source_index, (size_t)index - 1)) : NULL;
            if (doc) {
                markers = realloc(markers, (n_markers + 1) * sizeof *markers);
                markers[n_markers++] = (struct marker){ cps, doc };
            }
            p += len;
            continue;
        }
        int l;
        decode((const unsigned char *)p, &l);
        mc_buf_append(&out, p, (size_t)l);
        cps++;
        p += l;
    }
    if (!out.data) mc_buf_append_str(&out, "");
    *visible = (char *)out.data;
    if (!count) {
        free(markers);
        return 0;
    }
    sewn_sentence *sents = NULL;
    size_t ns = sewn_split_sentences(*visible, &sents);
    struct json_object *spans = json_object_new_object();
    for (size_t m = 0; m < n_markers && ns; m++) {
        const sewn_sentence *s = NULL;
        for (size_t i = ns; i > 0; i--) if (sents[i - 1].lower < markers[m].offset) { s = &sents[i - 1]; break; }
        if (!s) s = &sents[0];
        struct json_object *list = NULL;
        if (!json_object_object_get_ex(spans, markers[m].document_id, &list)) {
            list = json_object_new_array();
            json_object_object_add(spans, markers[m].document_id, list);
        }
        struct json_object *span = json_object_new_object();
        json_object_object_add(span, "lower", json_object_new_int64((int64_t)s->lower));
        json_object_object_add(span, "upper", json_object_new_int64((int64_t)s->upper));
        json_object_array_add(list, span);
    }
    /* merge per document */
    json_object_object_foreach(spans, key, list) {
        (void)key;
        sewn_spans merged = { 0 };
        for (size_t i = 0; i < json_object_array_length(list); i++) {
            struct json_object *sp = json_object_array_get_idx(list, i);
            int64_t lo = 0, hi = 0;
            mc_json_int64(sp, "lower", &lo);
            mc_json_int64(sp, "upper", &hi);
            sewn_spans_add(&merged, (size_t)lo, (size_t)hi);
        }
        sewn_spans_merge(&merged);
        struct json_object *fresh = spans_json(&merged);
        sewn_spans_free(&merged);
        json_object_object_add(spans, key, fresh);
    }
    free(sents);
    free(markers);
    *document_spans = spans;
    return (int)count;
}

void sewn_annotate(const char *response, sewn_contribution *c, const sewn_partition *partitions, size_t n, struct json_object *citations,
                   struct json_object *source_index, char **visible) {
    struct json_object *document_spans = NULL;
    int markers = sewn_parse_markers(response, source_index, visible, &document_spans);
    sewn_compute_spans(*visible, c, partitions, n, citations);
    if (markers <= 0 || !document_spans || !json_object_object_length(document_spans)) {
        if (document_spans) json_object_put(document_spans);
        return;
    }
    sewn_spans all_exact = { 0 };
    json_object_object_foreach(document_spans, key, list) {
        (void)key;
        for (size_t i = 0; i < json_object_array_length(list); i++) {
            int64_t lo = 0, hi = 0;
            mc_json_int64(json_object_array_get_idx(list, i), "lower", &lo);
            mc_json_int64(json_object_array_get_idx(list, i), "upper", &hi);
            sewn_spans_add(&all_exact, (size_t)lo, (size_t)hi);
        }
    }
    sewn_spans_merge(&all_exact);
    for (size_t o = 0; o < c->n; o++) {
        sewn_owner *ow = &c->owners[o];
        struct json_object *owned = json_object_new_object();
        sewn_spans merged = { 0 };
        for (size_t i = 0; i < ow->spans.n; i++) {
            bool overlapped = false;
            for (size_t e = 0; e < all_exact.n; e++)
                if (all_exact.v[e].lower < ow->spans.v[i].upper && ow->spans.v[i].lower < all_exact.v[e].upper) overlapped = true;
            if (!overlapped) sewn_spans_add(&merged, ow->spans.v[i].lower, ow->spans.v[i].upper);
        }
        for (size_t d = 0; d < ow->n_documents; d++) {
            struct json_object *list = NULL;
            if (!json_object_object_get_ex(document_spans, ow->document_ids[d], &list)) continue;
            json_object_object_add(owned, ow->document_ids[d], json_object_get(list));
            for (size_t i = 0; i < json_object_array_length(list); i++) {
                int64_t lo = 0, hi = 0;
                mc_json_int64(json_object_array_get_idx(list, i), "lower", &lo);
                mc_json_int64(json_object_array_get_idx(list, i), "upper", &hi);
                sewn_spans_add(&merged, (size_t)lo, (size_t)hi);
            }
        }
        sewn_spans_merge(&merged);
        sewn_spans_free(&ow->spans);
        ow->spans = merged;
        if (ow->document_spans) json_object_put(ow->document_spans);
        if (json_object_object_length(owned)) ow->document_spans = owned;
        else {
            json_object_put(owned);
            ow->document_spans = NULL;
        }
    }
    sewn_spans_free(&all_exact);
    json_object_put(document_spans);
}

/* MARK: The stream filter */

#define MARKER_MAX 7

void sewn_marker_filter_init(sewn_marker_filter *f) { memset(f, 0, sizeof *f); }

/* True when `s` (len bytes) is a strict prefix of a marker. */
static bool is_partial_marker(const char *s, size_t len) {
    if (len > MARKER_MAX) return false;
    int stage = 0, digits = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if (i == 0 && ch == '[') stage = 1;
        else if (i == 1 && ch == '[') stage = 2;
        else if (stage == 2 && isdigit((unsigned char)ch)) {
            if (++digits > 3) return false;
        } else if (ch == ']' && stage == 2 && digits > 0) stage = 3;
        else if (ch == ']' && stage == 3) return false;   /* complete: not partial */
        else return false;
    }
    return stage >= 1;
}

/* The start of a trailing substring that could still become a marker, or len. */
static size_t partial_suffix_start(const char *s, size_t len) {
    size_t scanned = 0;
    for (size_t i = len; i > 0 && scanned < MARKER_MAX; scanned++) {
        i--;
        if (s[i] != '[') continue;
        if (is_partial_marker(s + i, len - i)) {
            if (i > 0 && s[i - 1] == '[' && is_partial_marker(s + i - 1, len - i + 1)) return i - 1;
            return i;
        }
    }
    return len;
}

char *sewn_marker_filter_feed(sewn_marker_filter *f, const char *delta, size_t len) {
    mc_buf b = { 0 };
    mc_buf_append(&b, f->held, f->held_len);
    mc_buf_append(&b, delta, len);
    f->held_len = 0;
    if (!b.data) mc_buf_append_str(&b, "");
    /* strip complete markers */
    mc_buf out = { 0 };
    const char *s = (const char *)b.data;
    for (size_t i = 0; i < b.len;) {
        size_t m = marker_at(s + i, NULL);
        if (m) {
            i += m;
            continue;
        }
        mc_buf_append(&out, s + i, 1);
        i++;
    }
    mc_buf_free(&b);
    if (!out.data) mc_buf_append_str(&out, "");
    size_t hold = partial_suffix_start((const char *)out.data, out.len);
    if (hold < out.len) {
        f->held_len = out.len - hold;
        memcpy(f->held, out.data + hold, f->held_len);
        out.data[hold] = 0;
        out.len = hold;
    }
    return (char *)out.data;
}

char *sewn_marker_filter_finish(sewn_marker_filter *f) {
    char *tail = strndup(f->held, f->held_len);
    f->held_len = 0;
    return tail;
}
