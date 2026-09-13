#include "ambient/classify.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ambient/place.h"

/* ---- vocabulary (EditIntentClassifier, NamedPartClassifier, RoutingLexicon) ---- */

static const char *const REPLACE_VERBS[] = { "replace", "swap", "substitute", "rewrite", "reword", "rephrase", "change", "update",
    "revise", "tighten", "shorten", "condense", "expand", "polish", "correct", "fix", NULL };
static const char *const DELETE_VERBS[] = { "get rid of", "take out", "cut out", "strike out", "delete", "remove", "cut", "drop", "strike", NULL };
static const char *const INSERT_VERBS[] = { "insert", "append", "add", "put", "write", "type", NULL };
static const char *const MOVE_VERBS[] = { "move", "relocate", "shift", NULL };
static const char *const SPLIT_WORDS[] = { "to say", "to read", "with", "into", "using", "for", NULL };
static const struct { const char *phrase; ma_edit_anchor anchor; } ANCHORS[] = {
    { "at the start of", MA_ANCHOR_BEFORE }, { "at the top of", MA_ANCHOR_BEFORE }, { "at the end of", MA_ANCHOR_AFTER },
    { "at the bottom of", MA_ANCHOR_AFTER }, { "underneath", MA_ANCHOR_AFTER }, { "inside", MA_ANCHOR_INTO }, { "before", MA_ANCHOR_BEFORE },
    { "above", MA_ANCHOR_BEFORE }, { "after", MA_ANCHOR_AFTER }, { "below", MA_ANCHOR_AFTER }, { "under", MA_ANCHOR_AFTER },
    { "into", MA_ANCHOR_INTO }, { NULL, MA_ANCHOR_NONE },
};
static const char IN_PLACE_OF[] = "in place of";
static const char *const COURTESY_TAILS[] = { "for me", "for us", "please", NULL };
static const char *const ANAPHORIC_TRAILERS[] = { "up", "please", "for", "me", "us", "again", "quickly", "a", "bit", "little", "now", "instead", NULL };
static const char *const ANAPHORIC_HEADS[] = { "it", "that", "this", "those", "these", "the", "one", "thing", "them", NULL };
static const char *const BACKCHANNEL[] = { "yeah", "yep", "right", "exactly", "sure", "alright", "so", "well", NULL };
static const char *const ADDRESS[] = { "hey", "mary", "ok", "okay", NULL };
static const char *const CONFIRMATIONS[][4] = {
    { "that", "s", "the", "one" }, { "thats", "the", "one", NULL }, { "that", "s", "it", NULL }, { "thats", "it", NULL }, { "this", "one", NULL }, { "that", "one", NULL },
};
static const char *const REQUEST_FRAMES[][2] = { { "can", "you" }, { "could", "you" }, { "would", "you" }, { "can", "we" }, { "could", "we" }, { "would", "we" } };
static const char *const QUESTION_OPENERS[] = { "what", "what's", "why", "how", "when", "who", "where", "which", "is", "are", "am", "do", "does",
    "did", "can", "could", "would", "should", "tell", "read", "show", "list", "search", "find", "check", "describe", "explain", "summarize", "give", NULL };
static const char *const CLAUSE_BREAKS[] = { ", ", " and then ", " and ", " so can you ", " can you ", NULL };

static const char *const PART_NOUNS[] = { "part", "section", "paragraph", "passage", "bit", "chapter", "page", "line", "heading", "header",
    "excerpt", "quote", "sentence", "clause", "appendix", "footnote", "chunk", "piece", "portion", "segment", "subsection", "point", "table", "figure", NULL };
static const char *const CONNECTORS[] = { "about", "on", "regarding", "concerning", "discussing", "mentioning", "covering", "describing", "titled",
    "called", "named", "headed", "labelled", "labeled", "starting with", "beginning with", "that starts with", "which starts with", "that begins with",
    "which begins with", "that says", "which says", "that talks about", "that mentions", "that covers", "that discusses", "where", NULL };
static const char *const NUMBERED_NOUNS[] = { "section", "chapter", "part", "paragraph", "page", "appendix", "figure", "table", "step", "article", "clause", NULL };
static const struct { const char *word, *digits; } SPOKEN_NUMBERS[] = {
    { "one", "1" }, { "two", "2" }, { "three", "3" }, { "four", "4" }, { "five", "5" }, { "six", "6" }, { "seven", "7" }, { "eight", "8" }, { "nine", "9" },
    { "ten", "10" }, { "eleven", "11" }, { "twelve", "12" }, { "thirteen", "13" }, { "fourteen", "14" }, { "fifteen", "15" }, { "sixteen", "16" },
    { "seventeen", "17" }, { "eighteen", "18" }, { "nineteen", "19" }, { "twenty", "20" }, { NULL, NULL },
};
static const char *const PART_CLAUSE_BREAKS[] = { "that", "which", "and", "or", "but", "so", "because", "if", "when", "then", "please", "again", "for",
    "to", "at", "from", "into", "out", "aloud", "loud", "thanks", "ok", "okay", NULL };
static const char *const LEADING_ARTICLES[] = { "the", "a", "an", "my", "our", "your", "their", "its", "his", "her", "this", "that", NULL };
static const char *const TRAILING_FILLERS[] = { "say", "says", "said", "saying", "mean", "means", "meant", "go", "goes", "is", "was", "are", "were",
    "about", "again", "please", "aloud", "loud", "bit", "part", "one", NULL };
static const char *const USELESS[] = { "it", "this", "that", "these", "those", "they", "them", "us", "me", "you", "one", "thing", "things", "there",
    "here", "something", "anything", "everything", "stuff", "all", NULL };
static const char *const AMBIENT_SOURCES[] = { "calendar", "reminder", "event", "events", "appointment", "appointments", "schedule", "agenda",
    "shopping list", "grocery list", "to-do list", "todo list", "inbox", "email", "e-mail", "unread", NULL };

static bool in_list(const char *const *list, const char *word) {
    for (int i = 0; list[i]; i++) if (strcmp(list[i], word) == 0) return true;
    return false;
}

bool ma_is_question_opener(const char *w) { return w && in_list(QUESTION_OPENERS, w); }
bool ma_is_address_word(const char *w) { return w && in_list(ADDRESS, w); }
bool ma_is_backchannel_word(const char *w) { return w && in_list(BACKCHANNEL, w); }
bool ma_is_part_noun(const char *w) { return w && in_list(PART_NOUNS, w); }

bool ma_is_request_frame(const char *first, const char *second) {
    if (!first || !second) return false;
    for (size_t i = 0; i < sizeof REQUEST_FRAMES / sizeof *REQUEST_FRAMES; i++)
        if (strcmp(REQUEST_FRAMES[i][0], first) == 0 && strcmp(REQUEST_FRAMES[i][1], second) == 0) return true;
    return false;
}

static const char *const SHAPE_NAMES[] = { "replace", "insert", "delete", "move" };
static const char *const ANCHOR_NAMES[] = { NULL, "before", "after", "into" };
const char *ma_edit_shape_name(ma_edit_shape s) { return (unsigned)s < 4 ? SHAPE_NAMES[s] : NULL; }
const char *ma_edit_anchor_name(ma_edit_anchor a) { return (unsigned)a < 4 ? ANCHOR_NAMES[a] : NULL; }

/* ---- text helpers ---- */

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_word_char(unsigned char c) { return isalnum(c) || c >= 0x80; }

static void trim(const char *text, char *out, size_t n) {
    while (is_space(*text)) text++;
    size_t len = strlen(text);
    while (len && is_space(text[len - 1])) len--;
    if (len >= n) len = n - 1;
    memcpy(out, text, len);
    out[len] = 0;
}

/* Whether `phrase` (lowercase words separated by single spaces) occurs at `text` (case-insensitively),
 * followed by the end, whitespace, or a non-word character (\b). Returns its length. */
static size_t phrase_at(const char *text, const char *phrase) {
    size_t i = 0;
    for (; phrase[i]; i++) {
        char c = text[i];
        if (!c) return 0;
        if (phrase[i] == ' ') { if (!is_space(c)) return 0; continue; }
        if (tolower((unsigned char)c) != phrase[i]) return 0;
    }
    return is_word_char((unsigned char)text[i]) ? 0 : i;
}

/* "^(?:verbs)\b\s+(.+)$": the tail after a leading verb, or NULL. */
static const char *after_verb(const char *text, const char *const *verbs) {
    for (int v = 0; verbs[v]; v++) {
        size_t n = phrase_at(text, verbs[v]);
        if (!n || !is_space(text[n])) continue;
        const char *tail = text + n;
        while (is_space(*tail)) tail++;
        return *tail ? tail : NULL;
    }
    return NULL;
}

/* The earliest position in `text` where one of `phrases` sits between whitespace with text on both sides:
 * "(.+?)\s+(phrases)\s+(.+)". Sets *before_len (the head, trailing whitespace trimmed) and *after. */
static const char *split_at(const char *text, const char *const *phrases, const char **after, size_t *before_len, int *which) {
    for (const char *p = text; *p; p++) {
        if (p == text || !is_space(p[-1])) continue;
        for (int i = 0; phrases[i]; i++) {
            size_t n = phrase_at(p, phrases[i]);
            if (!n || !is_space(p[n])) continue;
            const char *rest = p + n;
            while (is_space(*rest)) rest++;
            if (!*rest) continue;
            size_t head = (size_t)(p - text);
            while (head && is_space(text[head - 1])) head--;
            if (!head) continue;
            if (after) *after = rest;
            if (before_len) *before_len = head;
            if (which) *which = i;
            return p;
        }
    }
    return NULL;
}

void ma_edit_trim_courtesy_tail(const char *raw, char *out, size_t n) {
    trim(raw, out, n);
    for (bool trimming = true; trimming;) {
        trimming = false;
        size_t len = strlen(out);
        for (int i = 0; COURTESY_TAILS[i]; i++) {
            size_t tl = strlen(COURTESY_TAILS[i]);
            if (len <= tl + 1 || out[len - tl - 1] != ' ') continue;
            bool same = true;
            for (size_t k = 0; k < tl && same; k++) same = tolower((unsigned char)out[len - tl + k]) == COURTESY_TAILS[i][k];
            if (!same) continue;
            out[len - tl - 1] = 0;
            trim(out, out, n);
            trimming = true;
            break;
        }
    }
}

bool ma_edit_is_anaphoric_tail(const char *raw) {
    char words[64][32];
    int n = ma_words(raw, words, 64, false);
    if (!n) return false;
    bool saw_reference = false;
    for (int i = 0; i < n; i++) {
        if (in_list(ANAPHORIC_HEADS, words[i])) { saw_reference = true; continue; }
        if (saw_reference && ma_is_part_noun(words[i])) continue;
        if (in_list(ANAPHORIC_TRAILERS, words[i])) continue;
        return false;
    }
    return saw_reference;
}

bool ma_edit_is_fresh_surface_phrase(const char *phrase) {
    char words[16][32];
    int n = ma_words(phrase, words, 16, false), i = 0;
    if (i < n && (strcmp(words[i], "a") == 0 || strcmp(words[i], "an") == 0 || strcmp(words[i], "this") == 0 || strcmp(words[i], "that") == 0 || strcmp(words[i], "the") == 0)) i++;
    if (i < n && strcmp(words[i], "brand") == 0) i++;
    if (i >= n) return false;
    if (strcmp(words[i], "new") && strcmp(words[i], "fresh") && strcmp(words[i], "blank") && strcmp(words[i], "empty")) return false;
    i++;
    static const char *const NOUNS[] = { "document", "documents", "note", "notes", "page", "pages", "file", "files", "doc", "docs", NULL };
    if (i < n && in_list(NOUNS, words[i])) return true;
    return i + 1 < n && in_list(NOUNS, words[i + 1]);
}

/* ---- the named part ---- */

bool ma_names_ambient_source(const char *utterance) {
    char lowered[2048];
    size_t len = strlen(utterance);
    if (len > sizeof lowered - 3) len = sizeof lowered - 3;
    lowered[0] = ' ';
    for (size_t i = 0; i < len; i++) lowered[i + 1] = (char)tolower((unsigned char)utterance[i]);
    lowered[len + 1] = ' ';
    lowered[len + 2] = 0;
    for (int i = 0; AMBIENT_SOURCES[i]; i++) {
        char needle[48];
        snprintf(needle, sizeof needle, " %s", AMBIENT_SOURCES[i]);
        if (strstr(lowered, needle)) return true;
    }
    return false;
}

static void bare_word(const char *word, char *out, size_t n) {
    size_t start = 0, end = strlen(word);
    while (start < end && !is_word_char((unsigned char)word[start])) start++;
    while (end > start && !is_word_char((unsigned char)word[end - 1])) end--;
    size_t i = 0;
    for (size_t k = start; k < end && i + 1 < n; k++) out[i++] = (char)tolower((unsigned char)word[k]);
    out[i] = 0;
}

bool ma_named_part_clean(const char *captured, char *out, size_t n) {
    /* Words split on whitespace, with commas as words of their own. */
    char spaced[1024];
    size_t k = 0;
    for (const char *c = captured; *c && k + 3 < sizeof spaced; c++) {
        if (*c == ',') { spaced[k++] = ' '; spaced[k++] = ','; spaced[k++] = ' '; }
        else spaced[k++] = *c;
    }
    spaced[k] = 0;
    char *words[64];
    int count = 0;
    for (char *tok = strtok(spaced, " \t\n\r"); tok && count < 64; tok = strtok(NULL, " \t\n\r")) words[count++] = tok;
    int first = 0;
    char bare[32];
    if (count) { bare_word(words[0], bare, sizeof bare); if (in_list(LEADING_ARTICLES, bare)) first = 1; }
    char *kept[5];
    int kn = 0;
    for (int i = first; i < count; i++) {
        bare_word(words[i], bare, sizeof bare);
        if (strcmp(words[i], ",") == 0 || (kn && in_list(PART_CLAUSE_BREAKS, bare))) break;
        kept[kn++] = words[i];
        if (kn == 5) break;
    }
    while (kn) {
        bare_word(kept[kn - 1], bare, sizeof bare);
        if (!in_list(TRAILING_FILLERS, bare)) break;
        kn--;
    }
    char phrase[512] = "";
    for (int i = 0; i < kn; i++) { if (i) strncat(phrase, " ", sizeof phrase - strlen(phrase) - 1); strncat(phrase, kept[i], sizeof phrase - strlen(phrase) - 1); }
    /* Trim " \t\n.,;:!?\"'“”‘’()-" from both ends (the curly quotes are three bytes each). */
    const char *punct = " \t\n.,;:!?\"'()-";
    size_t start = 0, end = strlen(phrase);
    for (;;) {
        if (start < end && strchr(punct, phrase[start])) { start++; continue; }
        if (end - start >= 3 && (unsigned char)phrase[start] == 0xE2 && (unsigned char)phrase[start + 1] == 0x80 &&
            ((unsigned char)phrase[start + 2] == 0x9C || (unsigned char)phrase[start + 2] == 0x9D || (unsigned char)phrase[start + 2] == 0x98 || (unsigned char)phrase[start + 2] == 0x99)) { start += 3; continue; }
        break;
    }
    for (;;) {
        if (end > start && strchr(punct, phrase[end - 1])) { end--; continue; }
        if (end - start >= 3 && (unsigned char)phrase[end - 3] == 0xE2 && (unsigned char)phrase[end - 2] == 0x80 &&
            ((unsigned char)phrase[end - 1] == 0x9C || (unsigned char)phrase[end - 1] == 0x9D || (unsigned char)phrase[end - 1] == 0x98 || (unsigned char)phrase[end - 1] == 0x99)) { end -= 3; continue; }
        break;
    }
    phrase[end] = 0;
    const char *result = phrase + start;
    char lowered[512];
    size_t len = strlen(result);
    for (size_t i = 0; i <= len; i++) lowered[i] = (char)tolower((unsigned char)result[i]);
    if (len < 2 || in_list(PART_CLAUSE_BREAKS, lowered) || in_list(LEADING_ARTICLES, lowered) || in_list(USELESS, lowered)) return false;
    snprintf(out, n, "%s", result);
    return true;
}

/* The word starting at `p` (letters and digits), lowercased into `out`; returns its length. */
static size_t word_at(const char *p, char *out, size_t n) {
    size_t i = 0;
    while (is_word_char((unsigned char)p[i])) { if (i + 1 < n) out[i] = (char)tolower((unsigned char)p[i]); i++; }
    out[i < n ? i : n - 1] = 0;
    return i;
}

static bool at_word_start(const char *text, const char *p) { return p == text || !is_word_char((unsigned char)p[-1]); }

/* Pattern 1: "<part noun>s? <connector> <phrase>". */
static bool named_part_by_noun(const char *text, char *out, size_t n) {
    for (const char *p = text; *p; p++) {
        if (!at_word_start(text, p)) continue;
        char word[32];
        size_t len = word_at(p, word, sizeof word);
        if (!len) continue;
        size_t wl = strlen(word);
        bool noun = ma_is_part_noun(word);
        if (!noun && wl > 1 && word[wl - 1] == 's') { word[wl - 1] = 0; noun = ma_is_part_noun(word); }
        if (!noun || !is_space(p[len])) continue;
        const char *q = p + len;
        while (is_space(*q)) q++;
        for (int c = 0; CONNECTORS[c]; c++) {
            size_t cl = phrase_at(q, CONNECTORS[c]);
            if (!cl || !is_space(q[cl])) continue;
            const char *rest = q + cl;
            while (is_space(*rest)) rest++;
            if (!*rest) continue;
            return ma_named_part_clean(rest, out, n);
        }
    }
    return false;
}

/* Pattern 2: "what|where does|do it|this|that|the X|my X|the X Y say(s) about|on|regarding <phrase>". */
static const char *skip_words(const char *p, int count) {
    for (int i = 0; i < count; i++) {
        while (is_space(*p)) p++;
        char w[32];
        size_t l = word_at(p, w, sizeof w);
        if (!l || !is_space(p[l])) return NULL;
        p += l;
    }
    return p;
}

static bool named_part_by_question(const char *text, char *out, size_t n) {
    for (const char *p = text; *p; p++) {
        if (!at_word_start(text, p)) continue;
        char w[32];
        size_t l = word_at(p, w, sizeof w);
        if (!l || (strcmp(w, "what") && strcmp(w, "where")) || !is_space(p[l])) continue;
        const char *q = skip_words(p + l, 0);
        while (is_space(*q)) q++;
        l = word_at(q, w, sizeof w);
        if (!l || (strcmp(w, "does") && strcmp(w, "do")) || !is_space(q[l])) continue;
        q += l;
        while (is_space(*q)) q++;
        l = word_at(q, w, sizeof w);
        if (!l || !is_space(q[l])) continue;
        /* The subject: it | this | that | the W | my W | the W W, in that order. */
        int shapes[3], shape_count = 0;
        if (!strcmp(w, "it") || !strcmp(w, "this") || !strcmp(w, "that")) shapes[shape_count++] = 0;
        else if (!strcmp(w, "the")) { shapes[shape_count++] = 1; shapes[shape_count++] = 2; }
        else if (!strcmp(w, "my")) shapes[shape_count++] = 1;
        else continue;
        const char *after_subject = q + l;
        for (int s = 0; s < shape_count; s++) {
            const char *r = skip_words(after_subject, shapes[s]);
            if (!r) continue;
            while (is_space(*r)) r++;
            char sw[32];
            size_t sl = word_at(r, sw, sizeof sw);
            if (!sl || (strcmp(sw, "say") && strcmp(sw, "says")) || !is_space(r[sl])) continue;
            const char *c = r + sl;
            while (is_space(*c)) c++;
            char cw[32];
            size_t cl = word_at(c, cw, sizeof cw);
            if (!cl || (strcmp(cw, "about") && strcmp(cw, "on") && strcmp(cw, "regarding")) || !is_space(c[cl])) continue;
            const char *rest = c + cl;
            while (is_space(*rest)) rest++;
            if (!*rest) continue;
            return ma_named_part_clean(rest, out, n);
        }
    }
    return false;
}

/* Pattern 3: a numbered division, as the document prints it. */
static bool named_part_by_number(const char *text, char *out, size_t n) {
    for (const char *p = text; *p; p++) {
        if (!at_word_start(text, p)) continue;
        char w[32];
        size_t l = word_at(p, w, sizeof w);
        if (!l || !in_list(NUMBERED_NOUNS, w) || !is_space(p[l])) continue;
        const char *q = p + l;
        while (is_space(*q)) q++;
        char num[32];
        size_t nl = word_at(q, num, sizeof num);
        if (!nl) continue;
        const char *digits = NULL;
        bool numeric = nl <= 3;
        for (size_t i = 0; i < nl && numeric; i++) numeric = isdigit((unsigned char)num[i]);
        char joined[32];
        if (numeric) {
            /* \d{1,3}(?:\.\d{1,3})* */
            size_t k = nl;
            while (q[k] == '.' && isdigit((unsigned char)q[k + 1])) {
                size_t d = 1;
                while (isdigit((unsigned char)q[k + d])) d++;
                if (d - 1 > 3) break;
                k += d;
            }
            if (is_word_char((unsigned char)q[k])) continue;
            snprintf(joined, sizeof joined, "%.*s", (int)(k < sizeof joined ? k : sizeof joined - 1), q);
            digits = joined;
        } else {
            for (int s = 0; SPOKEN_NUMBERS[s].word; s++) if (strcmp(SPOKEN_NUMBERS[s].word, num) == 0) digits = SPOKEN_NUMBERS[s].digits;
            if (!digits) continue;
        }
        snprintf(out, n, "%.*s %s", (int)l, p, digits);
        return true;
    }
    return false;
}

bool ma_named_part(const char *utterance, char *out, size_t n) {
    char text[2048];
    trim(utterance, text, sizeof text);
    if (!text[0] || ma_names_ambient_source(text)) return false;
    return named_part_by_noun(text, out, n) || named_part_by_question(text, out, n) || named_part_by_number(text, out, n);
}

/* ---- the edit intent ---- */

static bool same_ci(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

/* "Purpose section" → "Purpose" when the head is capitalised and a part noun remains. */
static bool heading_form(const char *candidate, char *out, size_t n) {
    char copy[MA_EDIT_TARGET_MAX];
    snprintf(copy, sizeof copy, "%s", candidate);
    char *words[32];
    int count = 0;
    for (char *tok = strtok(copy, " \t\n\r"); tok && count < 32; tok = strtok(NULL, " \t\n\r")) words[count++] = tok;
    if (count < 2) return false;
    char bare[32];
    bare_word(words[count - 1], bare, sizeof bare);
    size_t bl = strlen(bare);
    char singular[32];
    snprintf(singular, sizeof singular, "%s", bare);
    if (bl && bare[bl - 1] == 's') singular[bl - 1] = 0;
    if (!ma_is_part_noun(bare) && !ma_is_part_noun(singular)) return false;
    if (!isupper((unsigned char)words[count - 2][0])) return false;
    out[0] = 0;
    for (int i = 0; i < count - 1; i++) { if (i) strncat(out, " ", n - strlen(out) - 1); strncat(out, words[i], n - strlen(out) - 1); }
    return true;
}

int ma_edit_candidates(const char *phrase, char out[][MA_EDIT_TARGET_MAX], int max) {
    char found[8][MA_EDIT_TARGET_MAX];
    int n = 0;
#define OFFER(raw) do { const char *_raw = (raw); char cleaned[MA_EDIT_TARGET_MAX]; if (_raw && ma_named_part_clean(_raw, cleaned, sizeof cleaned)) { \
        bool known = false; for (int _i = 0; _i < n; _i++) if (same_ci(found[_i], cleaned)) known = true; \
        if (!known && n < 8) snprintf(found[n++], MA_EDIT_TARGET_MAX, "%s", cleaned); } } while (0)
    char part[MA_EDIT_TARGET_MAX];
    OFFER(ma_named_part(phrase, part, sizeof part) ? part : NULL);
    static const char *const CONTAINERS[] = { "of", "in", "from", NULL };
    const char *after = NULL;
    size_t before_len = 0;
    bool container = split_at(phrase, CONTAINERS, &after, &before_len, NULL) != NULL;
    if (container) {
        char head[MA_EDIT_TARGET_MAX];
        snprintf(head, sizeof head, "%.*s", (int)(before_len < sizeof head ? before_len : sizeof head - 1), phrase);
        OFFER(after);
        OFFER(head);
    } else {
        OFFER(phrase);
    }
    char headings[8][MA_EDIT_TARGET_MAX];
    int hn = 0;
    for (int i = 0; i < n; i++) {
        char stripped[MA_EDIT_TARGET_MAX], cleaned[MA_EDIT_TARGET_MAX];
        if (!heading_form(found[i], stripped, sizeof stripped) || !ma_named_part_clean(stripped, cleaned, sizeof cleaned)) continue;
        bool known = false;
        for (int k = 0; k < n && !known; k++) known = same_ci(found[k], cleaned);
        for (int k = 0; k < hn && !known; k++) known = same_ci(headings[k], cleaned);
        if (!known && hn < 8) snprintf(headings[hn++], MA_EDIT_TARGET_MAX, "%s", cleaned);
    }
    int total = 0;
    for (int i = 0; i < hn && total < max && total < MA_EDIT_CANDIDATES; i++) snprintf(out[total++], MA_EDIT_TARGET_MAX, "%s", headings[i]);
    for (int i = 0; i < n && total < max && total < MA_EDIT_CANDIDATES; i++) snprintf(out[total++], MA_EDIT_TARGET_MAX, "%s", found[i]);
#undef OFFER
    return total;
}

static void set_payload(ma_edit_intent *e, const char *captured) {
    char t[MA_EDIT_PAYLOAD_MAX];
    trim(captured, t, sizeof t);
    snprintf(e->payload, sizeof e->payload, "%s", t);
}

static bool replace_intent(const char *text, ma_edit_intent *e) {
    const char *raw = after_verb(text, REPLACE_VERBS);
    if (!raw) return false;
    char tail[1024];
    ma_edit_trim_courtesy_tail(raw, tail, sizeof tail);
    if (!tail[0]) return false;
    memset(e, 0, sizeof *e);
    e->shape = MA_EDIT_REPLACE;
    if (ma_edit_is_anaphoric_tail(tail)) { e->anaphoric = true; return true; }
    const char *after = NULL;
    size_t before_len = 0;
    if (split_at(tail, SPLIT_WORDS, &after, &before_len, NULL)) {
        char head[1024];
        snprintf(head, sizeof head, "%.*s", (int)before_len, tail);
        e->target_count = ma_edit_candidates(head, e->target, MA_EDIT_CANDIDATES);
        if (!e->target_count) return false;
        set_payload(e, after);
        return true;
    }
    e->target_count = ma_edit_candidates(tail, e->target, MA_EDIT_CANDIDATES);
    return e->target_count > 0;
}

static bool delete_intent(const char *text, ma_edit_intent *e) {
    const char *tail = after_verb(text, DELETE_VERBS);
    if (!tail) return false;
    memset(e, 0, sizeof *e);
    e->shape = MA_EDIT_DELETE;
    e->target_count = ma_edit_candidates(tail, e->target, MA_EDIT_CANDIDATES);
    return e->target_count > 0;
}

static bool insert_intent(const char *text, ma_edit_intent *e) {
    const char *tail = after_verb(text, INSERT_VERBS);
    if (!tail) return false;
    const char *markers[16];
    int m = 0;
    markers[m++] = IN_PLACE_OF;
    for (int i = 0; ANCHORS[i].phrase; i++) markers[m++] = ANCHORS[i].phrase;
    markers[m] = NULL;
    const char *after = NULL;
    size_t before_len = 0;
    int which = -1;
    if (!split_at(tail, markers, &after, &before_len, &which)) return false;
    if (ma_edit_is_fresh_surface_phrase(after)) return false;      /* composition into a fresh surface is not a revision */
    memset(e, 0, sizeof *e);
    e->target_count = ma_edit_candidates(after, e->target, MA_EDIT_CANDIDATES);
    if (!e->target_count) return false;
    char payload[1024];
    snprintf(payload, sizeof payload, "%.*s", (int)before_len, tail);
    set_payload(e, payload);
    if (which == 0) { e->shape = MA_EDIT_REPLACE; return true; }
    e->shape = MA_EDIT_INSERT;
    e->anchor = ANCHORS[which - 1].anchor;
    return true;
}

static bool move_intent(const char *text, ma_edit_intent *e) {
    const char *tail = after_verb(text, MOVE_VERBS);
    if (!tail) return false;
    const char *markers[16];
    int m = 0;
    for (int i = 0; ANCHORS[i].phrase; i++) markers[m++] = ANCHORS[i].phrase;
    markers[m++] = "to";
    markers[m] = NULL;
    const char *after = NULL;
    size_t before_len = 0;
    int which = -1;
    if (!split_at(tail, markers, &after, &before_len, &which)) return false;
    memset(e, 0, sizeof *e);
    e->shape = MA_EDIT_MOVE;
    char head[1024];
    snprintf(head, sizeof head, "%.*s", (int)before_len, tail);
    e->target_count = ma_edit_candidates(head, e->target, MA_EDIT_CANDIDATES);
    e->destination_count = ma_edit_candidates(after, e->destination, MA_EDIT_CANDIDATES);
    if (!e->target_count || !e->destination_count) return false;
    e->anchor = which < m - 1 ? ANCHORS[which].anchor : MA_ANCHOR_NONE;
    return true;
}

/* nextWord: skipping whitespace and commas, the next run of letters, lowercased; *after set past it. */
static size_t next_word(const char *p, char *out, size_t n, const char **after) {
    while (*p && (is_space(*p) || *p == ',')) p++;
    size_t i = 0;
    while (isalpha((unsigned char)p[i]) || (unsigned char)p[i] >= 0x80) { if (i + 1 < n) out[i] = (char)tolower((unsigned char)p[i]); i++; }
    out[i < n ? i : n - 1] = 0;
    if (after) *after = p + i;
    return i;
}

/* nextLetterRun: stepping over anything that is not a letter, so contractions split into words ("that's" → that, s). */
static size_t next_letter_run(const char *p, char *out, size_t n, const char **after) {
    while (*p && !(isalpha((unsigned char)*p) || (unsigned char)*p >= 0x80)) p++;
    return next_word(p, out, n, after);
}

static bool in_aliases(const char *const *aliases, int n, const char *word) {
    for (int i = 0; i < n; i++) if (aliases[i] && strcasecmp(aliases[i], word) == 0) return true;
    return false;
}

void ma_edit_strip_preamble(const char *text, const char *const *aliases, int alias_count, char *out, size_t n) {
    const char *p = text;
    bool peeled_alias = false;
    for (;;) {
        while (is_space(*p) || *p == ',') p++;
        char word[32];
        const char *after;
        size_t len = 0;
        while (isalpha((unsigned char)p[len]) || (unsigned char)p[len] >= 0x80) { if (len + 1 < sizeof word) word[len] = (char)tolower((unsigned char)p[len]); len++; }
        word[len < sizeof word ? len : sizeof word - 1] = 0;
        after = p + len;
        if (!len) break;
        if (ma_is_address_word(word) || ma_is_backchannel_word(word)) { p = after; continue; }
        if (!peeled_alias && in_aliases(aliases, alias_count, word)) { peeled_alias = true; p = after; continue; }
        /* A request frame: two words, and an optional "please". */
        bool frame_opener = false;
        for (size_t i = 0; i < sizeof REQUEST_FRAMES / sizeof *REQUEST_FRAMES; i++) if (strcmp(REQUEST_FRAMES[i][0], word) == 0) frame_opener = true;
        if (frame_opener) {
            char second[32], third[32];
            const char *after2, *after3;
            next_word(after, second, sizeof second, &after2);
            if (ma_is_request_frame(word, second)) {
                next_word(after2, third, sizeof third, &after3);
                p = strcmp(third, MA_POLITE_TAIL) == 0 ? after3 : after2;
                continue;
            }
        }
        /* A confirmation phrase, longest first. */
        bool confirmed = false;
        static const int ORDER[] = { 0, 1, 2, 3, 4, 5 };   /* the four-word one first, then three, then two */
        for (size_t c = 0; c < sizeof ORDER / sizeof *ORDER && !confirmed; c++) {
            const char *const *phrase = CONFIRMATIONS[ORDER[c]];
            const char *cursor = p;
            bool matched = true;
            for (int k = 0; k < 4 && phrase[k]; k++) {
                char w[32];
                const char *a;
                /* Letter runs step over apostrophes, so "that's" splits into "that", "s". */
                next_letter_run(cursor, w, sizeof w, &a);
                if (strcmp(w, phrase[k]) != 0) { matched = false; break; }
                cursor = a;
            }
            if (matched) { p = cursor; confirmed = true; }
        }
        if (confirmed) continue;
        break;
    }
    while (is_space(*p) || *p == ',') p++;
    snprintf(out, n, "%s", p);
}

static bool begins_with_request_frame(const char *utterance, const char *const *aliases, int alias_count) {
    char words[64][32];
    int n = 0, i = 0;
    /* Split on anything that is not a letter (the Swift's CharacterSet.letters.inverted). */
    {
        size_t len = 0;
        for (const char *c = utterance;; c++) {
            bool in = *c && (isalpha((unsigned char)*c) || (unsigned char)*c >= 0x80);
            if (in) { if (n < 64 && len < 31) words[n][len++] = (char)tolower((unsigned char)*c); continue; }
            if (len) { if (n < 64) words[n][len] = 0, n++; len = 0; }
            if (!*c) break;
        }
    }
    bool peeled_alias = false;
    while (i < n && (ma_is_address_word(words[i]) || ma_is_backchannel_word(words[i]) || (!peeled_alias && in_aliases(aliases, alias_count, words[i])))) {
        if (in_aliases(aliases, alias_count, words[i]) && !ma_is_address_word(words[i]) && !ma_is_backchannel_word(words[i])) peeled_alias = true;
        i++;
    }
    return n - i >= 2 && ma_is_request_frame(words[i], words[i + 1]);
}

static bool intent_in_clause(const char *clause, const char *const *aliases, int alias_count, ma_edit_intent *out) {
    char stripped[2048], text[2048];
    ma_edit_strip_preamble(clause, aliases, alias_count, stripped, sizeof stripped);
    trim(stripped, text, sizeof text);
    if (!text[0]) return false;
    char words[64][32];
    int n = ma_words(text, words, 64, false);
    if (!n) return false;
    if (ma_is_question_opener(words[0])) return false;
    if (n >= 2 && (strcmp(words[1], "me") == 0 || strcmp(words[1], "us") == 0)) return false;
    if (ma_names_ambient_source(text)) return false;
    return replace_intent(text, out) || delete_intent(text, out) || insert_intent(text, out) || move_intent(text, out);
}

static int word_count(const char *s) {
    int n = 0;
    bool in = false;
    for (; *s; s++) { if (is_space(*s)) in = false; else if (!in) { in = true; n++; } }
    return n;
}

bool ma_edit_intent_in(const char *utterance, const char *const *aliases, int alias_count, ma_edit_intent *out) {
    /* The utterance, then each clause around a break: heads, then tails. */
    char clauses[24][1024];
    int count = 0;
    snprintf(clauses[count++], 1024, "%s", utterance);
    char lowered[2048];
    size_t len = strlen(utterance);
    if (len >= sizeof lowered) len = sizeof lowered - 1;
    for (size_t i = 0; i < len; i++) lowered[i] = (char)tolower((unsigned char)utterance[i]);
    lowered[len] = 0;
    char heads[12][1024], tails[12][1024];
    int hn = 0, tn = 0;
#define QUALIFIES(c) (word_count(c) >= 2 && strcmp((c), clauses[0]) != 0 && !known_clause(heads, hn, (c)) && !known_clause(tails, tn, (c)))
    for (int b = 0; CLAUSE_BREAKS[b]; b++) {
        const char *from = lowered;
        const char *at;
        while ((at = strstr(from, CLAUSE_BREAKS[b]))) {
            size_t pos = (size_t)(at - lowered), bl = strlen(CLAUSE_BREAKS[b]);
            char head[1024], tail[1024];
            snprintf(head, sizeof head, "%.*s", (int)pos, utterance);
            trim(head, head, sizeof head);
            trim(utterance + pos + bl, tail, sizeof tail);
            if (hn < 12 && word_count(head) >= 2 && strcmp(head, clauses[0]) != 0) {
                bool known = false;
                for (int i = 0; i < hn && !known; i++) known = strcmp(heads[i], head) == 0;
                for (int i = 0; i < tn && !known; i++) known = strcmp(tails[i], head) == 0;
                if (!known) snprintf(heads[hn++], 1024, "%s", head);
            }
            if (tn < 12 && word_count(tail) >= 2 && strcmp(tail, clauses[0]) != 0) {
                bool known = false;
                for (int i = 0; i < hn && !known; i++) known = strcmp(heads[i], tail) == 0;
                for (int i = 0; i < tn && !known; i++) known = strcmp(tails[i], tail) == 0;
                if (!known) snprintf(tails[tn++], 1024, "%s", tail);
            }
            from = at + bl;
        }
    }
#undef QUALIFIES
    for (int i = 0; i < hn && count < 24; i++) snprintf(clauses[count++], 1024, "%s", heads[i]);
    for (int i = 0; i < tn && count < 24; i++) snprintf(clauses[count++], 1024, "%s", tails[i]);
    for (int i = 0; i < count; i++) {
        if (strchr(clauses[i], '?') && !begins_with_request_frame(clauses[i], aliases, alias_count)) continue;
        if (intent_in_clause(clauses[i], aliases, alias_count, out)) return true;
    }
    return false;
}

/* ---- question forms, decisions ---- */

static const char *const QUESTION_NAMES[] = { "what", "which", "where", "how", "why", "when", "who" };

unsigned ma_question_forms(const char *utterance) {
    char words[96][32];
    int n = ma_words(utterance, words, 96, false);
    unsigned forms = 0;
    for (int i = 0; i < n; i++)
        for (unsigned q = 0; q < 7; q++) if (strcmp(words[i], QUESTION_NAMES[q]) == 0) forms |= 1u << q;
    return forms;
}

const char *ma_question_name(unsigned bit) {
    for (unsigned q = 0; q < 7; q++) if (bit == 1u << q) return QUESTION_NAMES[q];
    return NULL;
}

int ma_bare_decision(const char *utterance) {
    static const char *const YES[] = { "yes", "yeah", "yep", "yup", "sure", "ok", "okay", "please", "do", "it", "go", "ahead", "fine", "confirm", "absolutely", NULL };
    static const char *const NO[] = { "no", "nope", "nah", "don't", "dont", "cancel", "never", "mind", "stop", "not", "now", NULL };
    char words[16][32];
    int n = ma_words(utterance, words, 16, true);
    if (!n || n > 4) return -1;
    bool yes = true, no = true;
    for (int i = 0; i < n; i++) {
        if (ma_is_address_word(words[i])) continue;
        if (!in_list(YES, words[i])) yes = false;
        if (!in_list(NO, words[i])) no = false;
    }
    if (no && (in_list(NO, words[0]) || (n > 1 && in_list(NO, words[1])))) return 0;
    return yes ? 1 : -1;
}
