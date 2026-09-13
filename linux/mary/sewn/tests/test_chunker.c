#include "mary_test.h"
#include "sewn/chunker.h"

struct chunks {
    char text[8][256];
    int count;
    int stop_after;
};

static int collect(const char *chunk, size_t len, void *user) {
    struct chunks *c = user;
    if (c->count < 8) snprintf(c->text[c->count], sizeof c->text[0], "%.*s", (int)len, chunk);
    c->count++;
    return c->stop_after && c->count >= c->stop_after;
}

static void feed(sewn_chunker *k, const char *s, struct chunks *c) {
    MARY_ASSERT(sewn_chunker_feed(k, s, strlen(s), collect, c) >= 0);
}

MARY_TEST(the_first_chunk_is_one_sentence_then_two) {
    sewn_chunker k;
    sewn_chunker_init(&k);
    struct chunks c = { 0 };
    feed(&k, "Hello there.", &c);
    MARY_ASSERT_EQ(c.count, 0);                     /* a terminator at the end is held */
    feed(&k, " How", &c);
    MARY_ASSERT_EQ(c.count, 1);
    MARY_ASSERT_STR(c.text[0], "Hello there.");
    feed(&k, " are you? I am", &c);
    MARY_ASSERT_EQ(c.count, 1);                     /* one sentence waits for its pair */
    feed(&k, " fine. Thanks", &c);
    MARY_ASSERT_EQ(c.count, 2);
    MARY_ASSERT_STR(c.text[1], "How are you? I am fine.");
    MARY_ASSERT_EQ(sewn_chunker_flush(&k, collect, &c), 0);
    MARY_ASSERT_EQ(c.count, 3);
    MARY_ASSERT_STR(c.text[2], "Thanks");
    MARY_ASSERT_EQ(sewn_chunker_flush(&k, collect, &c), 0);
    MARY_ASSERT_EQ(c.count, 3);
    sewn_chunker_free(&k);
}

MARY_TEST(decimals_and_lowercase_after_a_period_do_not_split) {
    sewn_chunker k;
    sewn_chunker_init(&k);
    struct chunks c = { 0 };
    feed(&k, "Pi is 3.14 and e is 2.71. next we", &c);
    MARY_ASSERT_EQ(c.count, 0);
    MARY_ASSERT_EQ(sewn_chunker_flush(&k, collect, &c), 0);
    MARY_ASSERT_EQ(c.count, 1);
    MARY_ASSERT_STR(c.text[0], "Pi is 3.14 and e is 2.71. next we");
    sewn_chunker_free(&k);
}

MARY_TEST(a_period_before_a_capital_ends_a_sentence_even_after_an_abbreviation) {
    /* SentenceBoundary's heuristic, kept as it is in Swift: "Dr. Smith" splits. */
    MARY_ASSERT_EQ(sewn_sentence_prefix_len("Ask Dr. Smith", 13), 7);
    MARY_ASSERT_EQ(sewn_sentence_prefix_len("Done.", 5), 5);
    MARY_ASSERT_EQ(sewn_sentence_prefix_len("Done. ", 6), 5);
    MARY_ASSERT_EQ(sewn_sentence_prefix_len("no boundary here", 16), 16);
}

MARY_TEST(closing_punctuation_stays_with_its_sentence) {
    sewn_chunker k;
    sewn_chunker_init(&k);
    struct chunks c = { 0 };
    feed(&k, "He said \"stop!\" Then he left", &c);
    MARY_ASSERT_EQ(c.count, 1);
    MARY_ASSERT_STR(c.text[0], "He said \"stop!\"");
    sewn_chunker_free(&k);
}

MARY_TEST(thirty_words_make_a_chunk_on_their_own) {
    sewn_chunker k;
    sewn_chunker_init(&k);
    struct chunks c = { 0 };
    feed(&k, "Yes. ", &c);
    feed(&k, "One two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen "
             "seventeen eighteen nineteen twenty twentyone twentytwo twentythree twentyfour twentyfive twentysix "
             "twentyseven twentyeight twentynine thirty thirtyone. And", &c);
    /* Thirty-one words complete a chunk without waiting for a second sentence: it
     * goes as soon as " And" shows the sentence is over. */
    MARY_ASSERT_EQ(c.count, 2);
    MARY_ASSERT(strncmp(c.text[1], "One two three", 13) == 0);
    feed(&k, " more", &c);
    MARY_ASSERT_EQ(c.count, 2);
    sewn_chunker_free(&k);
}

MARY_TEST(unicode_terminators_and_spaces_count) {
    sewn_chunker k;
    sewn_chunker_init(&k);
    struct chunks c = { 0 };
    feed(&k, "你好。再见", &c);            /* 你好。再见 */
    MARY_ASSERT_EQ(c.count, 1);
    MARY_ASSERT_STR(c.text[0], "你好。");
    MARY_ASSERT_EQ(sewn_sentence_prefix_len("Ol\xC3\xA1. \xC3\x89 isso", 13), 5);   /* "Olá. É isso": É is uppercase */
    struct chunks d = { .stop_after = 1 };
    sewn_chunker k2;
    sewn_chunker_init(&k2);
    MARY_ASSERT_EQ(sewn_chunker_feed(&k2, "A! B! C! D", 10, collect, &d), 1);
    sewn_chunker_free(&k);
    sewn_chunker_free(&k2);
}

MARY_TEST(markdown_is_not_spoken) {
    char *s = sewn_tts_sanitize("**Paris** is the [capital](https://example.com/fr) of `France`.");
    MARY_ASSERT_STR(s, "Paris is the capital of France.");
    free(s);
    s = sewn_tts_sanitize("## A heading\nand text");
    MARY_ASSERT_STR(s, "A heading\nand text");
    free(s);
    s = sewn_tts_sanitize("  - one item  ");
    MARY_ASSERT_STR(s, "one item");
    free(s);
    s = sewn_tts_sanitize("\xC2\xA0plain\xE2\x80\x83");    /* NBSP and em space trim */
    MARY_ASSERT_STR(s, "plain");
    free(s);
    s = sewn_tts_sanitize("a_snake_case_name");            /* as in Swift: "_snake_" reads as emphasis, once */
    MARY_ASSERT_STR(s, "asnakecase_name");
    free(s);
    s = sewn_tts_sanitize("");
    MARY_ASSERT_STR(s, "");
    free(s);
}

int main(void) {
    MARY_RUN(the_first_chunk_is_one_sentence_then_two);
    MARY_RUN(decimals_and_lowercase_after_a_period_do_not_split);
    MARY_RUN(a_period_before_a_capital_ends_a_sentence_even_after_an_abbreviation);
    MARY_RUN(closing_punctuation_stays_with_its_sentence);
    MARY_RUN(thirty_words_make_a_chunk_on_their_own);
    MARY_RUN(unicode_terminators_and_spaces_count);
    MARY_RUN(markdown_is_not_spoken);
    MARY_TEST_MAIN_END();
}
