#define _DARWIN_C_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mary_test.h"
#include "voice/kws.h"
#include "voice/wake.h"

static const char *temp_file(const char *body, char *path, size_t n) {
    snprintf(path, n, "/tmp/mary-kws-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    size_t len = strlen(body);
    if (write(fd, body, len) != (ssize_t)len) MARY_FAIL("could not write %s", path);
    close(fd);
    return path;
}

static int check_body(const char *body, char *bad, size_t n) {
    char kw[64], tokens[512];
    temp_file(body, kw, sizeof kw);
    int rc = mv_kws_check_keywords(kw, mary_test_path("voice/tests/data/tokens.txt", tokens, sizeof tokens), bad, n);
    unlink(kw);
    return rc;
}

MARY_TEST(marys_keywords_use_only_tokens_the_model_has) {
    char kw[512], tokens[512], bad[64] = "";
    mary_test_path("voice/data/keywords.txt", kw, sizeof kw);
    mary_test_path("voice/tests/data/tokens.txt", tokens, sizeof tokens);
    MARY_ASSERT_EQ(mv_kws_check_keywords(kw, tokens, bad, sizeof bad), 0);
    MARY_ASSERT_STR(bad, "");
}

MARY_TEST(an_unknown_token_is_named_before_sherpa_can_end_the_process) {
    char bad[64] = "";
    MARY_ASSERT_EQ(check_body("▁HE Y ▁MAR Y :2.0 @HEY_MARY\n▁HE Y ▁MARRY :2.0 @HEY_MARRY\n", bad, sizeof bad), -EINVAL);
    MARY_ASSERT_STR(bad, "▁MARRY");
    /* A token is a whole line entry of tokens.txt, never a prefix of one. */
    MARY_ASSERT_EQ(check_body("▁H Y\n", bad, sizeof bad), -EINVAL);
    MARY_ASSERT_STR(bad, "▁H");
}

MARY_TEST(a_line_without_tokens_is_refused) {
    char bad[64] = "";
    MARY_ASSERT_EQ(check_body(":2.0 #0.25 @HEY_MARY\n", bad, sizeof bad), -EINVAL);
    MARY_ASSERT_STR(bad, "(empty)");
    MARY_ASSERT_EQ(check_body("▁HE Y ▁MAR Y\n\n▁HI ▁MAR Y\n", bad, sizeof bad), -EINVAL);
    MARY_ASSERT_EQ(check_body("", bad, sizeof bad), -EINVAL);
    MARY_ASSERT_EQ(check_body("▁HE Y ▁MAR Y\r\n", bad, sizeof bad), 0);
    MARY_ASSERT_EQ(check_body("▁HI ▁MAR Y", bad, sizeof bad), 0);
}

MARY_TEST(a_file_that_cannot_be_read_is_an_errno) {
    char kw[512], bad[64] = "";
    mary_test_path("voice/data/keywords.txt", kw, sizeof kw);
    MARY_ASSERT_EQ(mv_kws_check_keywords(kw, "/nonexistent/tokens.txt", bad, sizeof bad), -ENOENT);
    MARY_ASSERT_EQ(mv_kws_check_keywords("/nonexistent/keywords.txt", kw, bad, sizeof bad), -ENOENT);
}

MARY_TEST(keyword_names_read_as_words) {
    char out[32];
    mv_kws_phrase("HEY_MARY", out, sizeof out);
    MARY_ASSERT_STR(out, "hey mary");
    mv_kws_phrase("HEY_MARY", out, 4);
    MARY_ASSERT_STR(out, "hey");
    mv_kws_phrase(NULL, out, sizeof out);
    MARY_ASSERT_STR(out, "");
}

/* The spotter and WakePlanner must agree: each keyword, read as words, is a bare wake. */
MARY_TEST(every_keyword_is_a_phrase_wake_planner_accepts) {
    char path[512], line[256], phrase[64], request[64];
    FILE *f = fopen(mary_test_path("voice/data/keywords.txt", path, sizeof path), "r");
    MARY_ASSERT(f != NULL);
    int count = 0;
    while (f && fgets(line, sizeof line, f)) {
        char *at = strchr(line, '@');
        if (!at) {
            MARY_FAIL("a keyword without an @name: %s", line);
            continue;
        }
        at[strcspn(at, "\r\n")] = 0;
        mv_kws_phrase(at + 1, phrase, sizeof phrase);
        if (mv_wake_in(phrase, request, sizeof request) != MV_WAKE_BARE) MARY_FAIL("\"%s\" does not wake", phrase);
        count++;
    }
    if (f) fclose(f);
    MARY_ASSERT_EQ(count, 4);
    /* A leading "Mary" wakes (as in Swift: "Mary open my email"); a mention does not. */
    mv_kws_phrase("TELL_MARY", phrase, sizeof phrase);
    MARY_ASSERT_EQ(mv_wake_in(phrase, request, sizeof request), MV_WAKE_NONE);
}

#ifndef HAVE_SHERPA
MARY_TEST(without_sherpa_onnx_there_is_no_spotter) {
    int error = 0;
    MARY_ASSERT(mv_kws_open(NULL, &error) == NULL);
    MARY_ASSERT_EQ(error, -ENOSYS);
}
#else
/* 16 kHz mono s16 samples of a RIFF file, and half a second of silence after them. */
static int16_t *read_wav(const char *path, size_t *count) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *bytes = malloc(4 << 20);
    size_t size = fread(bytes, 1, 4 << 20, f);
    fclose(f);
    int16_t *samples = NULL;
    for (size_t off = 12; off + 8 <= size;) {
        uint32_t len = bytes[off + 4] | bytes[off + 5] << 8 | bytes[off + 6] << 16 | (uint32_t)bytes[off + 7] << 24;
        if (memcmp(bytes + off, "data", 4) == 0 && off + 8 + len <= size) {
            *count = len / 2 + 8000;
            samples = calloc(*count, sizeof *samples);
            for (size_t i = 0; i < len / 2; i++) samples[i] = (int16_t)(bytes[off + 8 + 2 * i] | bytes[off + 9 + 2 * i] << 8);
            break;
        }
        off += 8 + len;
    }
    free(bytes);
    return samples;
}

static int spot(mv_kws *k, const int16_t *samples, size_t count, char *first, size_t n) {
    int spotted = 0;
    char keyword[64];
    for (size_t i = 0; i < count; i += 320) {
        if (mv_kws_feed(k, samples + i, count - i < 320 ? count - i : 320, keyword, sizeof keyword) == 1) {
            if (!spotted) snprintf(first, n, "%s", keyword);
            spotted++;
        }
    }
    return spotted;
}

/* The builder unpacks the pinned model and sets MARY_KWS_MODEL; "0.wav" says "light up". */
MARY_TEST(the_pinned_model_loads_marys_keywords_and_spots_its_own_sample) {
    const char *model = getenv("MARY_KWS_MODEL");
    if (!model || !*model) {
        printf("  (MARY_KWS_MODEL is unset; the builder sets it)\n");
        return;
    }
    char kw[512], tokens[1024], sample_kw[1024], wav[1024], bad[64] = "", first[64] = "";
    mary_test_path("voice/data/keywords.txt", kw, sizeof kw);
    snprintf(tokens, sizeof tokens, "%s/tokens.txt", model);
    snprintf(sample_kw, sizeof sample_kw, "%s/test_wavs/test_keywords.txt", model);
    snprintf(wav, sizeof wav, "%s/test_wavs/0.wav", model);
    MARY_ASSERT_EQ(mv_kws_check_keywords(kw, tokens, bad, sizeof bad), 0);
    MARY_ASSERT_STR(bad, "");

    size_t count = 0;
    int16_t *samples = read_wav(wav, &count);
    MARY_ASSERT(samples != NULL);
    mv_kws_config c = mv_kws_config_default();
    c.model_dir = model;
    c.keywords_file = kw;
    int error = 0;
    mv_kws *k = mv_kws_open(&c, &error);
    MARY_ASSERT(k != NULL);
    if (k && samples) MARY_ASSERT_EQ(spot(k, samples, count, first, sizeof first), 0);
    mv_kws_close(k);

    c.keywords_file = sample_kw;
    k = mv_kws_open(&c, &error);
    MARY_ASSERT(k != NULL);
    if (k && samples) {
        MARY_ASSERT_EQ(spot(k, samples, count, first, sizeof first), 1);
        MARY_ASSERT_STR(first, "LIGHT UP");
    }
    mv_kws_close(k);
    free(samples);

    char broken[64];
    c.keywords_file = temp_file("▁NOT_A_TOKEN @X\n", broken, sizeof broken);
    error = 0;
    MARY_ASSERT(mv_kws_open(&c, &error) == NULL);    /* and this process is still here to say so */
    MARY_ASSERT_EQ(error, -EINVAL);
    unlink(broken);

    c.model_dir = "/nonexistent";
    MARY_ASSERT(mv_kws_open(&c, &error) == NULL);
    MARY_ASSERT_EQ(error, -ENOENT);
}
#endif

int main(void) {
    MARY_RUN(marys_keywords_use_only_tokens_the_model_has);
    MARY_RUN(an_unknown_token_is_named_before_sherpa_can_end_the_process);
    MARY_RUN(a_line_without_tokens_is_refused);
    MARY_RUN(a_file_that_cannot_be_read_is_an_errno);
    MARY_RUN(keyword_names_read_as_words);
    MARY_RUN(every_keyword_is_a_phrase_wake_planner_accepts);
#ifndef HAVE_SHERPA
    MARY_RUN(without_sherpa_onnx_there_is_no_spotter);
#else
    MARY_RUN(the_pinned_model_loads_marys_keywords_and_spots_its_own_sample);
#endif
    MARY_TEST_MAIN_END();
}
