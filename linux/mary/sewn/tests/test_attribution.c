/* Gita's span attribution, against the Swift fixtures (Tests/sewn-serverTests/
 * MarkerSpanTests.swift, Flow2_GitaSpanTests.swift). */
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "mary_test.h"
#include "sewn/attribution.h"

static struct json_object *parse(const char *text) { return mc_json_parse(text, strlen(text)); }

static sewn_partition part(const char *pid, const char *doc, const char *name, const char *text, const char *owner) {
    sewn_partition p = { 0 };
    p.partition_id = (char *)pid;
    p.document_id = (char *)doc;
    p.name = (char *)name;
    p.text = (char *)text;
    p.owner_id = (char *)owner;
    p.family = "file";
    p.lane = "personal";
    p.group_id = "files-alice";
    return p;
}

/* The code points [lower, upper) of `text`, as a string. */
static char *slice(const char *text, size_t lower, size_t upper) {
    size_t cp = 0, start = 0, end = strlen(text);
    for (size_t i = 0; text[i]; i++) {
        if (((unsigned char)text[i] & 0xC0) == 0x80) continue;
        if (cp == lower) start = i;
        if (cp == upper) { end = i; break; }
        cp++;
    }
    return strndup(text + start, end - start);
}

static const sewn_owner *owner_named(const sewn_contribution *c, const char *id) {
    for (size_t i = 0; i < c->n; i++) if (strcmp(c->owners[i].owner_id, id) == 0) return &c->owners[i];
    return NULL;
}

MARY_TEST(sentences_split_where_swift_splits) {
    sewn_sentence *s = NULL;
    size_t n = sewn_split_sentences("One. Two!\nThree?\n\nFour", &s);
    MARY_ASSERT_EQ(n, 4);
    MARY_ASSERT_EQ(s[0].lower, 0);
    MARY_ASSERT_EQ(s[0].upper, 5);        /* "One. " */
    MARY_ASSERT_EQ(s[1].lower, 5);
    MARY_ASSERT_EQ(s[1].upper, 10);       /* "Two!\n" */
    MARY_ASSERT_EQ(s[2].upper, 17);       /* "Three?\n": the break takes one newline */
    MARY_ASSERT_EQ(s[3].lower, 17);       /* "\nFour" */
    MARY_ASSERT_EQ(s[3].upper, 22);
    free(s);
    n = sewn_split_sentences("Caf\xC3\xA9 au lait. Oui.", &s);
    MARY_ASSERT_EQ(n, 2);
    MARY_ASSERT_EQ(s[1].lower, 14);       /* code points, not bytes */
    free(s);
    MARY_ASSERT_EQ(sewn_split_sentences("   ", &s), 0);
}

MARY_TEST(text_helpers_match_gita) {
    char *n = sewn_normalise("Hello, World!  It's \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x94 fine.");
    MARY_ASSERT_STR(n, "hello world it s quoted fine");
    free(n);
    char *m = sewn_strip_markdown("# Title\n**bold** and `code` here\n- item one\n2. item two\n> quote\n[link](http://x) ![img](y.png)\n```\ncode block\n```end");
    MARY_ASSERT_STR(m, "Title\n bold  and   here\nitem one\nitem two\nquote\nlink  \n end");
    free(m);
    char **words = NULL;
    size_t nw = sewn_content_words("the quick brown fox is 42 years and a day old", &words);
    MARY_ASSERT_EQ(nw, 6);                /* quick brown fox years day old */
    MARY_ASSERT_STR(words[0], "quick");
    MARY_ASSERT_STR(words[5], "old");
    char *a[] = { "x", "y", "z" }, *b[] = { "y", "z", "w", "v" };
    MARY_ASSERT_NEAR(sewn_overlap_coefficient(a, 3, b, 4), 2.0 / 3.0, 1e-9);
    sewn_words_free(words, nw);
}

MARY_TEST(the_contribution_splits_word_counts_by_owner_and_document) {
    sewn_partition ps[3] = {
        part("p1", "doc-a", "a", "one two three four", "alice"),          /* 4 words */
        part("p2", "doc-a", "a", "five six", "alice"),                    /* +2 → doc-a 6 */
        part("p3", "doc-b", "b", "seven eight", "bob"),                   /* 2 */
    };
    sewn_contribution c;
    sewn_contribution_build(ps, 3, "", &c);
    MARY_ASSERT_EQ(c.n, 2);
    const sewn_owner *alice = owner_named(&c, "alice"), *bob = owner_named(&c, "bob");
    MARY_ASSERT(alice && bob);
    MARY_ASSERT_NEAR(alice->royalty, 0.75, 1e-9);
    MARY_ASSERT_NEAR(bob->royalty, 0.25, 1e-9);
    MARY_ASSERT_EQ(alice->n_documents, 1);
    MARY_ASSERT_NEAR(alice->influence[0], 1.0, 1e-9);
    struct json_object *j = sewn_contribution_json(&c);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(j, "owners")), 2);
    json_object_put(j);
    sewn_contribution_free(&c);
    sewn_contribution_build(ps, 0, "", &c);
    MARY_ASSERT_EQ(c.n, 0);
    sewn_contribution_free(&c);
}

MARY_TEST(markers_are_parsed_stripped_and_attributed_to_their_sentences) {
    struct json_object *index = parse("[\"doc-a\",\"doc-b\",\"doc-c\"]");
    const char *text = "You wrote about gauge theory last spring.[[1]] Someone else compared it to music.[[2]] That contrast is worth exploring.";
    char *visible = NULL;
    struct json_object *spans = NULL;
    MARY_ASSERT_EQ(sewn_parse_markers(text, index, &visible, &spans), 2);
    MARY_ASSERT(strstr(visible, "[[") == NULL);
    MARY_ASSERT_EQ(json_object_object_length(spans), 2);
    struct json_object *a = mc_json_array(spans, "doc-a"), *b = mc_json_array(spans, "doc-b");
    MARY_ASSERT_EQ(json_object_array_length(a), 1);
    int64_t lo = 0, hi = 0;
    mc_json_int64(json_object_array_get_idx(a, 0), "lower", &lo);
    mc_json_int64(json_object_array_get_idx(a, 0), "upper", &hi);
    char *s = slice(visible, (size_t)lo, (size_t)hi);
    MARY_ASSERT(strstr(s, "gauge theory") != NULL);
    free(s);
    mc_json_int64(json_object_array_get_idx(b, 0), "lower", &lo);
    mc_json_int64(json_object_array_get_idx(b, 0), "upper", &hi);
    s = slice(visible, (size_t)lo, (size_t)hi);
    MARY_ASSERT(strstr(s, "compared it to music") != NULL);
    free(s);
    json_object_put(spans);
    free(visible);

    /* a run attributes the same sentence to each source */
    MARY_ASSERT_EQ(sewn_parse_markers("Both notes converge on the same conclusion.[[1]][[3]]", index, &visible, &spans), 2);
    MARY_ASSERT_STR(json_object_to_json_string(mc_json_array(spans, "doc-a")), json_object_to_json_string(mc_json_array(spans, "doc-c")));
    json_object_put(spans);
    free(visible);
    /* an unknown index strips without attribution */
    MARY_ASSERT_EQ(sewn_parse_markers("A bold claim.[[9]]", index, &visible, &spans), 1);
    MARY_ASSERT_STR(visible, "A bold claim.");
    MARY_ASSERT_EQ(json_object_object_length(spans), 0);
    json_object_put(spans);
    free(visible);
    /* no markers: untouched */
    const char *plain = "Nothing to see here. Just prose with [brackets] and numbers [3].";
    MARY_ASSERT_EQ(sewn_parse_markers(plain, index, &visible, &spans), 0);
    MARY_ASSERT_STR(visible, plain);
    MARY_ASSERT(spans == NULL);
    free(visible);
    json_object_put(index);
}

static char *stream(const char *const *deltas, size_t n) {
    sewn_marker_filter f;
    sewn_marker_filter_init(&f);
    char out[512] = "";
    for (size_t i = 0; i < n; i++) {
        char *piece = sewn_marker_filter_feed(&f, deltas[i], strlen(deltas[i]));
        strcat(out, piece);
        free(piece);
    }
    char *tail = sewn_marker_filter_finish(&f);
    strcat(out, tail);
    free(tail);
    return strdup(out);
}

MARY_TEST(the_stream_filter_strips_split_markers_and_flushes_false_tails) {
    const char *split[] = { "The engine was designed", " in London.[", "[2]", "]", " Neat." };
    char *out = stream(split, 5);
    MARY_ASSERT_STR(out, "The engine was designed in London. Neat.");
    free(out);
    const char *literal[] = { "An array literal: [", "[1, 2, 3] is not a marker." };
    out = stream(literal, 2);
    MARY_ASSERT_STR(out, "An array literal: [[1, 2, 3] is not a marker.");
    free(out);
    sewn_marker_filter f;
    sewn_marker_filter_init(&f);
    char *held = sewn_marker_filter_feed(&f, "Ends with a dangling [[1", 24);
    MARY_ASSERT(strstr(held, "[[1") == NULL);
    char *tail = sewn_marker_filter_finish(&f);
    MARY_ASSERT_STR(tail, "[[1");
    free(held);
    free(tail);
    /* every 3-way split of a marked text streams to parseMarkers' visible text */
    const char *raw = "First point.[[1]] Second point spans[[2]] mid-sentence. Trailing [[3]]";
    struct json_object *index = parse("[\"doc-a\",\"doc-b\",\"doc-c\"]");
    char *expected = NULL;
    struct json_object *spans = NULL;
    sewn_parse_markers(raw, index, &expected, &spans);
    size_t len = strlen(raw);
    int diverged = 0;
    for (size_t i = 1; i < len - 1 && !diverged; i++) {
        for (size_t j = i + 1; j < len && !diverged; j++) {
            char *a = strndup(raw, i), *b = strndup(raw + i, j - i);
            const char *pieces[] = { a, b, raw + j };
            char *got = stream(pieces, 3);
            if (strcmp(got, expected) != 0) diverged = 1;
            free(got);
            free(a);
            free(b);
        }
    }
    MARY_ASSERT(!diverged);
    if (spans) json_object_put(spans);
    free(expected);
    json_object_put(index);
}

MARY_TEST(annotate_gives_exact_spans_to_marked_sentences_and_heuristic_ones_to_the_rest) {
    sewn_partition ps[2] = {
        part("p-a", "doc-a", "gauge-theory.txt", "gauge theory fiber bundles connections curvature", "alice"),
        part("p-b", "doc-b", "music-analogy.txt", "music harmony resonance analogy comparison", "bob"),
    };
    struct json_object *index = parse("[\"doc-a\",\"doc-b\",\"doc-c\"]");
    sewn_contribution c;
    sewn_contribution_build(ps, 2, "", &c);
    char *visible = NULL;
    sewn_annotate("You explored gauge theory in depth.[[1]] Someone compared the whole thing to music.[[2]]", &c, ps, 2, NULL, index, &visible);
    MARY_ASSERT(strstr(visible, "[[") == NULL);
    const sewn_owner *alice = owner_named(&c, "alice"), *bob = owner_named(&c, "bob");
    MARY_ASSERT(alice->document_spans && mc_json_array(alice->document_spans, "doc-a"));
    MARY_ASSERT(bob->document_spans && mc_json_array(bob->document_spans, "doc-b"));
    MARY_ASSERT(alice->spans.n == 1 && bob->spans.n == 1);
    char *s = slice(visible, alice->spans.v[0].lower, alice->spans.v[0].upper);
    MARY_ASSERT(strstr(s, "gauge theory") != NULL);
    free(s);
    s = slice(visible, bob->spans.v[0].lower, bob->spans.v[0].upper);
    MARY_ASSERT(strstr(s, "music") != NULL);
    free(s);
    free(visible);
    sewn_contribution_free(&c);

    /* no markers: the heuristic still finds the overlap, and there is no exact attribution */
    sewn_contribution_build(ps, 2, "", &c);
    struct json_object *citations = parse("[{\"document_id\":\"doc-a\",\"key_words\":[\"gauge\",\"theory\",\"fiber\",\"bundles\",\"connections\"]}]");
    const char *raw = "gauge theory fiber bundles connections curvature all matter here.";
    sewn_annotate(raw, &c, ps, 2, citations, index, &visible);
    MARY_ASSERT_STR(visible, raw);
    alice = owner_named(&c, "alice");
    MARY_ASSERT(alice->spans.n > 0);
    MARY_ASSERT(alice->document_spans == NULL);
    free(visible);
    json_object_put(citations);
    sewn_contribution_free(&c);

    /* mixed: the marked sentence is exact, the unmarked one heuristic, and they do not overlap */
    sewn_contribution_build(ps, 2, "", &c);
    citations = parse("[{\"document_id\":\"doc-b\",\"key_words\":[\"music\",\"harmony\",\"resonance\",\"analogy\",\"comparison\"]}]");
    sewn_annotate("You explored gauge theory fiber bundles connections curvature.[[1]] music harmony resonance analogy comparison shows up too.",
                  &c, ps, 2, citations, index, &visible);
    alice = owner_named(&c, "alice");
    bob = owner_named(&c, "bob");
    MARY_ASSERT(alice->document_spans != NULL);
    MARY_ASSERT(bob->spans.n > 0);
    for (size_t i = 0; i < alice->spans.n; i++)
        for (size_t k = 0; k < bob->spans.n; k++)
            MARY_ASSERT(!(alice->spans.v[i].lower < bob->spans.v[k].upper && bob->spans.v[k].lower < alice->spans.v[i].upper));
    free(visible);
    json_object_put(citations);
    sewn_contribution_free(&c);
    json_object_put(index);
}

MARY_TEST(citations_come_from_the_briefings_sentences_that_name_a_source) {
    sewn_partition ps[2] = {
        part("p1", "d1", "startup-ideas", "ideas text", "alice"),
        part("p2", "d2", "travel-diary", "diary text", "alice"),
    };
    struct json_object *c = sewn_extract_citations("In your note \"startup-ideas\", you mentioned raising capital next spring.\n"
                                                   "Someone noted in \"travel-diary\" that the Kyoto trip was transformative.", ps, 2, "alice");
    MARY_ASSERT_EQ(json_object_array_length(c), 2);
    struct json_object *first = json_object_array_get_idx(c, 0);
    MARY_ASSERT_STR(mc_json_string(first, "document_id"), "d1");
    const char *words = json_object_to_json_string(mc_json_array(first, "key_words"));
    MARY_ASSERT(strstr(words, "raising") || strstr(words, "capital"));
    MARY_ASSERT(strstr(words, "\"in\"") == NULL);
    json_object_put(c);
    c = sewn_extract_citations("", ps, 2, "alice");
    MARY_ASSERT_EQ(json_object_array_length(c), 0);
    json_object_put(c);
    c = sewn_extract_citations("In your note \"other-thing\", you wrote about fundraising.", ps, 2, "alice");
    MARY_ASSERT_EQ(json_object_array_length(c), 0);
    json_object_put(c);
    /* two sentences citing one source accumulate */
    sewn_partition one = part("p", "d", "fitness-log", "some text", "alice");
    c = sewn_extract_citations("In your note \"fitness-log\", you tracked your marathon training schedule.\nYour \"fitness-log\" also mentioned a target of sub-four hours.", &one, 1, "alice");
    MARY_ASSERT_EQ(json_object_array_length(c), 1);
    words = json_object_to_json_string(mc_json_array(json_object_array_get_idx(c, 0), "key_words"));
    MARY_ASSERT(strstr(words, "marathon") && strstr(words, "target"));
    json_object_put(c);
}

MARY_TEST(compute_spans_prefers_citation_phrases_then_direct_phrases_then_overlap) {
    sewn_partition p = part("p", "d", "water-history", "ancient roman engineering techniques historical aqueducts infrastructure supply", "alice");
    struct json_object *citations = sewn_extract_citations("In your note \"water-history\", you recalled that Roman aqueducts supplied clean water to cities.", &p, 1, "alice");
    MARY_ASSERT_EQ(json_object_array_length(citations), 1);
    sewn_contribution c;
    sewn_contribution_build(&p, 1, "", &c);
    sewn_compute_spans("Roman aqueducts supplied clean water to entire cities, which is remarkable engineering.", &c, &p, 1, citations);
    MARY_ASSERT(c.owners[0].spans.n > 0);
    sewn_contribution_free(&c);
    json_object_put(citations);
    sewn_partition q = part("p", "d", "ideas", "fundraising venture capital startup growth runway investors seed round", "alice");
    sewn_contribution_build(&q, 1, "", &c);
    sewn_compute_spans("Fundraising runway and investor relations are key startup concerns.", &c, &q, 1, NULL);
    MARY_ASSERT(c.owners[0].spans.n > 0);          /* the similarity path */
    sewn_contribution_free(&c);
    sewn_partition r = part("p", "d", "doc", "xylophone rhinoceros quasar nebula vortex", "alice");
    sewn_contribution_build(&r, 1, "", &c);
    sewn_compute_spans("Today was sunny and warm outside the garden.", &c, &r, 1, NULL);
    MARY_ASSERT_EQ(c.owners[0].spans.n, 0);
    sewn_compute_spans("", &c, &r, 1, NULL);
    MARY_ASSERT_EQ(c.owners[0].spans.n, 0);
    sewn_contribution_free(&c);
}

int main(void) {
    MARY_RUN(sentences_split_where_swift_splits);
    MARY_RUN(text_helpers_match_gita);
    MARY_RUN(the_contribution_splits_word_counts_by_owner_and_document);
    MARY_RUN(markers_are_parsed_stripped_and_attributed_to_their_sentences);
    MARY_RUN(the_stream_filter_strips_split_markers_and_flushes_false_tails);
    MARY_RUN(annotate_gives_exact_spans_to_marked_sentences_and_heuristic_ones_to_the_rest);
    MARY_RUN(citations_come_from_the_briefings_sentences_that_name_a_source);
    MARY_RUN(compute_spans_prefers_citation_phrases_then_direct_phrases_then_overlap);
    MARY_TEST_MAIN_END();
}
