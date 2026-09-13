#include "voice/kws.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mv_kws_config mv_kws_config_default(void) {
    return (mv_kws_config){ .model_dir = MV_KWS_MODEL_DIR, .keywords_file = MV_KWS_KEYWORDS,
                            .threshold = 0.25f, .score = 1.0f, .threads = 1 };
}

static void copy_out(char *out, size_t n, const char *s) {
    if (n) snprintf(out, n, "%s", s);
}

/* The whole file after a leading '\n', so every line of tokens.txt reads "\n<token> <id>". */
static char *read_lines(const char *path, int *error) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *error = -errno;
        return NULL;
    }
    size_t cap = 8192, len = 1;
    char *buf = malloc(cap);
    if (buf) buf[0] = '\n';
    while (buf) {
        if (cap - len < 4096) {
            char *grown = realloc(buf, cap * 2);
            if (!grown) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
            cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - len - 1, f);
        len += got;
        if (got == 0) break;
    }
    bool failed = ferror(f);
    fclose(f);
    if (!buf || failed) {
        free(buf);
        *error = buf ? -EIO : -ENOMEM;
        return NULL;
    }
    buf[len] = 0;
    return buf;
}

static bool is_option(const char *word) { return word[0] == ':' || word[0] == '#' || word[0] == '@'; }

int mv_kws_check_keywords(const char *keywords_file, const char *tokens_file, char *bad, size_t n) {
    int rc = 0;
    char *tokens = read_lines(tokens_file, &rc);
    if (!tokens) return rc;
    char *keywords = read_lines(keywords_file, &rc);
    if (!keywords) {
        free(tokens);
        return rc;
    }
    int lines = 0;
    char *line = keywords + 1;
    while (rc == 0 && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        int count = 0;
        char *save = NULL;
        for (char *word = strtok_r(line, " \t", &save); word && rc == 0; word = strtok_r(NULL, " \t", &save)) {
            if (is_option(word)) continue;
            count++;
            char needle[256];
            if (snprintf(needle, sizeof needle, "\n%s ", word) >= (int)sizeof needle || !strstr(tokens, needle)) {
                copy_out(bad, n, word);
                rc = -EINVAL;
            }
        }
        if (rc == 0 && count == 0) {
            copy_out(bad, n, "(empty)");
            rc = -EINVAL;
        }
        lines++;
        if (!end) break;
        line = end + 1;
    }
    if (rc == 0 && lines == 0) {
        copy_out(bad, n, "(empty)");
        rc = -EINVAL;
    }
    free(tokens);
    free(keywords);
    return rc;
}

void mv_kws_phrase(const char *keyword, char *out, size_t n) {
    if (!n) return;
    size_t i = 0;
    for (; keyword && keyword[i] && i + 1 < n; i++)
        out[i] = keyword[i] == '_' ? ' ' : (char)tolower((unsigned char)keyword[i]);
    out[i] = 0;
}

#ifdef HAVE_SHERPA
#include <sherpa-onnx/c-api/c-api.h>
#include <unistd.h>

#include "common/log.h"

struct mv_kws {
    const SherpaOnnxKeywordSpotter *spotter;
    const SherpaOnnxOnlineStream *stream;
};

mv_kws *mv_kws_open(const mv_kws_config *config, int *error) {
    mv_kws_config c = config ? *config : mv_kws_config_default();
    char encoder[1024], decoder[1024], joiner[1024], tokens[1024], bad[128] = "";
    snprintf(encoder, sizeof encoder, "%s/%s", c.model_dir, MV_KWS_ENCODER);
    snprintf(decoder, sizeof decoder, "%s/%s", c.model_dir, MV_KWS_DECODER);
    snprintf(joiner, sizeof joiner, "%s/%s", c.model_dir, MV_KWS_JOINER);
    snprintf(tokens, sizeof tokens, "%s/%s", c.model_dir, MV_KWS_TOKENS);
    const char *files[] = { encoder, decoder, joiner, tokens };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        if (access(files[i], R_OK) != 0) {
            int rc = -errno;
            mc_log(MC_LOG_ERROR, "keyword model: %s: %s", files[i], strerror(-rc));
            if (error) *error = rc;
            return NULL;
        }
    }
    int rc = mv_kws_check_keywords(c.keywords_file, tokens, bad, sizeof bad);
    if (rc) {
        if (rc == -EINVAL) mc_log(MC_LOG_ERROR, "keywords %s: %s is not a token of the model", c.keywords_file, bad);
        else mc_log(MC_LOG_ERROR, "keywords %s: %s", c.keywords_file, strerror(-rc));
        if (error) *error = rc;
        return NULL;
    }

    SherpaOnnxKeywordSpotterConfig sc;
    memset(&sc, 0, sizeof sc);
    sc.feat_config.sample_rate = MV_KWS_SAMPLE_RATE;
    sc.feat_config.feature_dim = 80;
    sc.model_config.transducer.encoder = encoder;
    sc.model_config.transducer.decoder = decoder;
    sc.model_config.transducer.joiner = joiner;
    sc.model_config.tokens = tokens;
    sc.model_config.num_threads = c.threads > 0 ? c.threads : 1;
    sc.model_config.provider = "cpu";
    sc.max_active_paths = 4;
    sc.num_trailing_blanks = 1;
    sc.keywords_score = c.score;
    sc.keywords_threshold = c.threshold;
    sc.keywords_file = c.keywords_file;

    struct mv_kws *k = calloc(1, sizeof *k);
    if (!k) {
        if (error) *error = -ENOMEM;
        return NULL;
    }
    k->spotter = SherpaOnnxCreateKeywordSpotter(&sc);
    if (k->spotter) k->stream = SherpaOnnxCreateKeywordStream(k->spotter);
    if (!k->spotter || !k->stream) {
        mc_log(MC_LOG_ERROR, "the keyword spotter could not be loaded from %s", c.model_dir);
        mv_kws_close(k);
        if (error) *error = -EINVAL;
        return NULL;
    }
    return k;
}

void mv_kws_close(mv_kws *k) {
    if (!k) return;
    if (k->stream) SherpaOnnxDestroyOnlineStream(k->stream);
    if (k->spotter) SherpaOnnxDestroyKeywordSpotter(k->spotter);
    free(k);
}

int mv_kws_feed(mv_kws *k, const int16_t *samples, size_t count, char *keyword, size_t n) {
    float chunk[320];
    int spotted = 0;
    while (count) {
        size_t m = count < 320 ? count : 320;
        for (size_t i = 0; i < m; i++) chunk[i] = samples[i] / 32768.0f;
        SherpaOnnxOnlineStreamAcceptWaveform(k->stream, MV_KWS_SAMPLE_RATE, chunk, (int32_t)m);
        samples += m;
        count -= m;
        while (SherpaOnnxIsKeywordStreamReady(k->spotter, k->stream)) {
            SherpaOnnxDecodeKeywordStream(k->spotter, k->stream);
            const SherpaOnnxKeywordResult *r = SherpaOnnxGetKeywordResult(k->spotter, k->stream);
            if (r && r->keyword && r->keyword[0]) {
                if (!spotted) copy_out(keyword, n, r->keyword);
                spotted = 1;
                SherpaOnnxResetKeywordStream(k->spotter, k->stream);
            }
            if (r) SherpaOnnxDestroyKeywordResult(r);
        }
    }
    return spotted;
}

void mv_kws_reset(mv_kws *k) { SherpaOnnxResetKeywordStream(k->spotter, k->stream); }

#else

struct mv_kws {
    int unused;
};

mv_kws *mv_kws_open(const mv_kws_config *config, int *error) {
    if (error) *error = -ENOSYS;
    return NULL;
}
void mv_kws_close(mv_kws *kws) {}
int mv_kws_feed(mv_kws *kws, const int16_t *samples, size_t count, char *keyword, size_t n) { return -ENOSYS; }
void mv_kws_reset(mv_kws *kws) {}
#endif
