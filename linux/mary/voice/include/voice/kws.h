/* "Hey Mary", spotted on the machine (MaryVoice's WakeWordListener transcribes every
 * utterance and looks for the name; MaryOS keeps the microphone local until the name is
 * heard — PORTING.md deviation 1). sherpa-onnx's keyword spotter runs the gigaspeech
 * zipformer pinned in third_party.lock over the phrases in voice/data/keywords.txt.
 *
 * mv_kws_feed runs the model: call it from maryd's own audio thread, never from PipeWire's
 * real-time callback. Without sherpa-onnx (a Mac build) mv_kws_open answers -ENOSYS; the
 * keyword-file check and the phrase mapping work everywhere. */
#ifndef MARY_VOICE_KWS_H
#define MARY_VOICE_KWS_H

#include <stddef.h>
#include <stdint.h>

#define MV_KWS_MODEL_DIR "/usr/share/mary/models/kws"
#define MV_KWS_KEYWORDS "/usr/share/mary/keywords.txt"
#define MV_KWS_ENCODER "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
#define MV_KWS_DECODER "decoder-epoch-12-avg-2-chunk-16-left-64.onnx"
#define MV_KWS_JOINER "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
#define MV_KWS_TOKENS "tokens.txt"
#define MV_KWS_SAMPLE_RATE 16000

typedef struct mv_kws_config {
    const char *model_dir;          /* MV_KWS_MODEL_DIR */
    const char *keywords_file;      /* MV_KWS_KEYWORDS */
    float threshold;                /* 0.25: lower spots more, and more wrongly */
    float score;                    /* 1.0: the boost a keyword's tokens get */
    int threads;                    /* 1 */
} mv_kws_config;

mv_kws_config mv_kws_config_default(void);

/* sherpa-onnx ends the whole process when a keyword names a token its model does not
 * have, so a keyword file is checked before the spotter sees it: 0 when every line has
 * tokens and each one is in tokens.txt; -EINVAL with the first unknown token (or
 * "(empty)" for a line with none) copied to bad; -errno when a file cannot be read. */
int mv_kws_check_keywords(const char *keywords_file, const char *tokens_file, char *bad, size_t n);

/* "HEY_MARY" → "hey mary": a keyword's @name as the words WakePlanner reads. */
void mv_kws_phrase(const char *keyword, char *out, size_t n);

typedef struct mv_kws mv_kws;

/* Checks the keyword file and the model's files, then loads the spotter.
 * NULL with *error set: -ENOSYS without sherpa-onnx, -ENOENT, -EINVAL. */
mv_kws *mv_kws_open(const mv_kws_config *config, int *error);
void mv_kws_close(mv_kws *kws);
/* 16 kHz mono s16. 1 when a keyword ended within these samples — its @name is copied to
 * keyword and the spotter starts over — and 0 when none did. */
int mv_kws_feed(mv_kws *kws, const int16_t *samples, size_t count, char *keyword, size_t n);
/* Forgets what it has heard so far (after a turn, before listening for the name again). */
void mv_kws_reset(mv_kws *kws);

#endif
