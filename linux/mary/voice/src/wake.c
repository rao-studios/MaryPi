#include "voice/wake.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static const char *const WAKE_NAMES[] = { "mary" };
static const char *const PREAMBLE[] = { "hey", "ok", "okay", "hi" };
static const char *const ADDRESS[] = { "mary", "hey", "ok", "okay", "please", "now" };
static const char *const DANGLING[] = {
    "a", "an", "the", "my", "your", "our", "their",
    "to", "of", "for", "with", "from", "into", "onto", "at", "by", "via",
    "and", "or", "but", "then", "because", "if", "than",
    "um", "uh", "uhm", "er", "erm", "hmm",
};

#define COUNT(a) (sizeof a / sizeof a[0])
#define MAX_TOKENS 64
#define TOKEN_MAX 64

static bool in_list(const char *word, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) if (strcmp(word, list[i]) == 0) return true;
    return false;
}

static bool prefix_of_any(const char *head, const char *const *list, size_t n) {
    size_t len = strlen(head);
    for (size_t i = 0; i < n; i++) if (strncmp(list[i], head, len) == 0) return true;
    return false;
}

/* One UTF-8 code point; an invalid byte reads as itself. */
static size_t next_cp(const unsigned char *s, uint32_t *cp) {
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *cp = (uint32_t)(s[0] & 0x1F) << 6 | (s[1] & 0x3F); return 2; }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = (uint32_t)(s[0] & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);
        return 4;
    }
    *cp = s[0];
    return 1;
}

/* isLetter || isNumber, for the scripts a transcript here holds. */
static bool is_word_cp(uint32_t cp) {
    if (cp < 0x80) return isalnum((int)cp) != 0;
    return (cp >= 0x00C0 && cp <= 0x024F && cp != 0x00D7 && cp != 0x00F7) || (cp >= 0x0370 && cp <= 0x03FF) ||
           (cp >= 0x0400 && cp <= 0x04FF) || (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

/* Splits on whitespace: [start, end) byte spans of the raw tokens. */
static size_t split(const char *text, size_t starts[], size_t ends[]) {
    size_t n = 0, i = 0, len = strlen(text);
    while (i < len && n < MAX_TOKENS) {
        while (i < len && isspace((unsigned char)text[i])) i++;
        if (i == len) break;
        starts[n] = i;
        while (i < len && !isspace((unsigned char)text[i])) i++;
        ends[n++] = i;
    }
    return n;
}

/* normalize: lowercased letters and digits of one raw token. */
static void normalize(const char *text, size_t start, size_t end, char *out) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)text + start, *stop = (const unsigned char *)text + end;
    while (p < stop && o + 5 < TOKEN_MAX) {
        uint32_t cp;
        size_t n = next_cp(p, &cp);
        if (p + n > stop) n = (size_t)(stop - p);
        if (is_word_cp(cp)) {
            if (cp < 0x80) out[o++] = (char)tolower((int)cp);
            else { memcpy(out + o, p, n); o += n; }
        }
        p += n;
    }
    out[o] = 0;
}

/* The normalized, non-empty tokens. */
static size_t words(const char *text, char out[][TOKEN_MAX]) {
    size_t starts[MAX_TOKENS], ends[MAX_TOKENS], n = split(text ? text : "", starts, ends), k = 0;
    for (size_t i = 0; i < n; i++) {
        normalize(text, starts[i], ends[i], out[k]);
        if (out[k][0]) k++;
    }
    return k;
}

/* remainderLeadTrim: whitespace and , ; : — – - … . */
static size_t trim_mark(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (isspace(c) || c == ',' || c == ';' || c == ':' || c == '-' || c == '.') return 1;
    if (c == 0xE2 && (unsigned char)s[1] == 0x80) {
        unsigned char d = (unsigned char)s[2];
        if (d == 0x94 || d == 0x93 || d == 0xA6) return 3;   /* — – … */
    }
    return 0;
}

static size_t trim_mark_before(const char *s, size_t end) {
    if (end >= 1 && trim_mark(s + end - 1) == 1) return 1;
    if (end >= 3 && trim_mark(s + end - 3) == 3) return 3;
    return 0;
}

mv_wake mv_wake_in(const char *text, char *request, size_t cap) {
    if (request && cap) request[0] = 0;
    if (!text) return MV_WAKE_NONE;
    size_t starts[MAX_TOKENS], ends[MAX_TOKENS], n = split(text, starts, ends);
    long wake = -1;
    for (size_t i = 0; i < n; i++) {
        char token[TOKEN_MAX];
        normalize(text, starts[i], ends[i], token);
        if (!token[0]) continue;   /* bare punctuation decides nothing */
        if (in_list(token, WAKE_NAMES, COUNT(WAKE_NAMES))) {
            wake = (long)i;
            break;
        }
        if (!in_list(token, PREAMBLE, COUNT(PREAMBLE))) return MV_WAKE_NONE;
    }
    if (wake < 0) return MV_WAKE_NONE;
    if ((size_t)wake + 1 >= n) return MV_WAKE_BARE;
    /* The remainder's raw tokens joined by single spaces, then trimmed at both ends. */
    char joined[1024];
    size_t o = 0;
    for (size_t i = (size_t)wake + 1; i < n && o < sizeof joined - 1; i++) {
        if (o) joined[o++] = ' ';
        size_t len = ends[i] - starts[i];
        if (o + len >= sizeof joined) len = sizeof joined - 1 - o;
        memcpy(joined + o, text + starts[i], len);
        o += len;
    }
    joined[o] = 0;
    size_t a = 0, b = o, step;
    while (a < b && (step = trim_mark(joined + a))) a += step;
    while (b > a && (step = trim_mark_before(joined, b))) b -= step;
    if (a == b) return MV_WAKE_BARE;
    if (request && cap) {
        size_t len = b - a < cap - 1 ? b - a : cap - 1;
        memcpy(request, joined + a, len);
        request[len] = 0;
    }
    return MV_WAKE_REQUEST;
}

bool mv_could_still_wake(const char *partial) {
    char tokens[MAX_TOKENS][TOKEN_MAX];
    size_t n = words(partial, tokens), i = 0;
    while (i < n && in_list(tokens[i], PREAMBLE, COUNT(PREAMBLE))) i++;
    if (i == n) return true;   /* nothing decisive yet */
    if (in_list(tokens[i], WAKE_NAMES, COUNT(WAKE_NAMES))) return true;
    if (i != n - 1) return false;
    return prefix_of_any(tokens[i], WAKE_NAMES, COUNT(WAKE_NAMES)) || prefix_of_any(tokens[i], PREAMBLE, COUNT(PREAMBLE));
}

bool mv_is_stop_listening(const char *text) {
    char tokens[MAX_TOKENS][TOKEN_MAX];
    size_t n = words(text, tokens), first = 0, last = n;
    while (first < last && in_list(tokens[first], ADDRESS, COUNT(ADDRESS))) first++;
    while (last > first && in_list(tokens[last - 1], ADDRESS, COUNT(ADDRESS))) last--;
    if (last - first != 2 || strcmp(tokens[first + 1], "listening") != 0) return false;
    return strcmp(tokens[first], "stop") == 0 || strcmp(tokens[first], "quit") == 0;
}

double mv_endpoint_extra_silence(const char *partial) {
    size_t starts[MAX_TOKENS], ends[MAX_TOKENS], n = split(partial ? partial : "", starts, ends);
    if (n == 0) return 0;
    char word[TOKEN_MAX];
    normalize(partial, starts[n - 1], ends[n - 1], word);
    return in_list(word, DANGLING, COUNT(DANGLING)) ? MV_DANGLING_EXTENSION : 0;
}
