#include <stdlib.h>

#include "mary_test.h"
#include "thread/text.h"

MARY_TEST(names_and_kinds_normalize_like_graphstore) {
    char *n = thread_normalize_name("  Ada \t\n  Lovelace  ");
    MARY_ASSERT_STR(n, "ada lovelace");
    free(n);
    n = thread_normalize_name("\xC3\x89" "cole\xC2\xA0Normale");
    MARY_ASSERT_STR(n, "\xC3\xA9" "cole normale");
    free(n);
    char kind[THREAD_KIND_MAX];
    thread_normalize_kind("  Person ", kind);
    MARY_ASSERT_STR(kind, "person");
    thread_normalize_kind("", kind);
    MARY_ASSERT_STR(kind, "concept");
    thread_normalize_kind(NULL, kind);
    MARY_ASSERT_STR(kind, "concept");
    char *p = thread_lower_trim("  Works At \n");
    MARY_ASSERT_STR(p, "works at");
    free(p);
    MARY_ASSERT_EQ(thread_utf8_count("h\xC3\xA9llo"), 5);
    MARY_ASSERT_EQ(thread_utf8_prefix("h\xC3\xA9llo", 2), 3);
}

static char *repeat_sentence(const char *sentence, int times) {
    size_t n = strlen(sentence);
    char *out = malloc(n * times + 1);
    out[0] = 0;
    for (int i = 0; i < times; i++) strcat(out, sentence);
    return out;
}

MARY_TEST(a_short_text_is_one_chunk_and_blank_text_none) {
    char **chunks = NULL;
    size_t n = thread_chunk("Hello world.\n\nSecond paragraph.", 1500, &chunks);
    MARY_ASSERT_EQ(n, 1);
    MARY_ASSERT_STR(chunks[0], "Hello world.\n\nSecond paragraph.");
    thread_strings_free(chunks, n);
    n = thread_chunk("  \n\n  \n", 1500, &chunks);
    MARY_ASSERT_EQ(n, 0);
    MARY_ASSERT(chunks == NULL);
    n = thread_chunk("", 0, &chunks);
    MARY_ASSERT_EQ(n, 0);
}

MARY_TEST(a_long_paragraph_splits_at_sentence_ends_under_the_cap) {
    /* 40 sentences of 100 characters: no chunk passes 1500, every chunk ends at a sentence. */
    char *text = repeat_sentence("This sentence is exactly one hundred characters long when it is written out in full, honestly so. ", 40);
    char **chunks = NULL;
    size_t n = thread_chunk(text, 1500, &chunks);
    MARY_ASSERT(n >= 3);
    for (size_t i = 0; i < n; i++) {
        MARY_ASSERT(thread_utf8_count(chunks[i]) <= 1500);
        MARY_ASSERT(chunks[i][strlen(chunks[i]) - 1] == '.');
    }
    thread_strings_free(chunks, n);
    free(text);
}

MARY_TEST(a_sentence_over_the_cap_is_hard_split_by_code_points) {
    /* 400 × "é" (800 bytes, 400 code points) with a cap of 150 code points: three pieces, none broken mid-character. */
    char *text = repeat_sentence("\xC3\xA9", 400);
    char **chunks = NULL;
    size_t n = thread_chunk(text, 150, &chunks);
    MARY_ASSERT_EQ(n, 3);
    if (n == 3) {
        MARY_ASSERT_EQ(thread_utf8_count(chunks[0]), 150);
        MARY_ASSERT_EQ(thread_utf8_count(chunks[2]), 100);
        MARY_ASSERT_EQ(strlen(chunks[0]), 300);
    }
    thread_strings_free(chunks, n);
    free(text);
    /* ".\n" is a sentence end too, and paragraphs pack with a blank line. */
    /* Sentences trim spaces and tabs only, as Swift's .whitespaces does: the newline
     * that ended "One." begins "Two.", and "\nTwo." + " " + "Three" is 11, over 8. */
    n = thread_chunk("One.\nTwo. Three", 8, &chunks);
    MARY_ASSERT_EQ(n, 3);
    if (n == 3) {
        MARY_ASSERT_STR(chunks[0], "One.");
        MARY_ASSERT_STR(chunks[1], "\nTwo.");
        MARY_ASSERT_STR(chunks[2], "Three");
    }
    thread_strings_free(chunks, n);
    n = thread_chunk("One.\nTwo. Three", 11, &chunks);
    MARY_ASSERT_EQ(n, 2);
    if (n == 2) {
        MARY_ASSERT_STR(chunks[0], "One. \nTwo.");
        MARY_ASSERT_STR(chunks[1], "Three");
    }
    thread_strings_free(chunks, n);
}

MARY_TEST(tags_are_the_frequent_long_words_minus_stopwords) {
    const char *texts[] = { "Mary sews threads into Gita's ballad; threads, threads everywhere, with frigates from the New World.",
                            "The ballad names every thread." };
    char **tags = NULL;
    size_t n = thread_tags(texts, 2, &tags);
    MARY_ASSERT(n >= 3);
    MARY_ASSERT_STR(tags[0], "threads");           /* 3 mentions */
    MARY_ASSERT_STR(tags[1], "ballad");            /* 2, then ties by word */
    for (size_t i = 0; i < n; i++) {
        MARY_ASSERT(strcmp(tags[i], "with") != 0);  /* a stopword */
        MARY_ASSERT(thread_utf8_count(tags[i]) > 3);
    }
    thread_strings_free(tags, n);
    MARY_ASSERT_EQ(thread_tags(texts, 0, &tags), 0);
}

int main(void) {
    MARY_RUN(names_and_kinds_normalize_like_graphstore);
    MARY_RUN(a_short_text_is_one_chunk_and_blank_text_none);
    MARY_RUN(a_long_paragraph_splits_at_sentence_ends_under_the_cap);
    MARY_RUN(a_sentence_over_the_cap_is_hard_split_by_code_points);
    MARY_RUN(tags_are_the_frequent_long_words_minus_stopwords);
    MARY_TEST_MAIN_END();
}
