#include <errno.h>

#include "common/base64.h"
#include "common/json.h"
#include "mary_test.h"
#include "sewn/mistral.h"

static void assert_json(struct json_object *obj, const char *expected) {
    MARY_ASSERT(obj != NULL);
    MARY_ASSERT_STR(mc_json_compact(obj, NULL), expected);
    json_object_put(obj);
}

MARY_TEST(the_chat_body_is_what_run_stream_mistral_sends) {
    struct json_object *messages = json_object_new_array();
    struct json_object *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string("user"));
    json_object_object_add(m, "content", json_object_new_string("Hi"));
    json_object_array_add(messages, m);
    assert_json(sewn_chat_body(NULL, messages, 1200, 0.4, 0.9),
        "{\"model\":\"mistral-medium-latest\",\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],"
        "\"max_tokens\":1200,\"temperature\":0.4,\"top_p\":0.9,\"stream\":true}");
    json_object_put(messages);   /* the body took its own reference */
}

MARY_TEST(chat_chunks_become_deltas_and_done) {
    sewn_chat_event e;
    const char *first = "{\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"Paris\"},\"finish_reason\":null}]}";
    sewn_chat_event_parse(first, strlen(first), &e);
    MARY_ASSERT_EQ(e.kind, SEWN_CHAT_DELTA);
    MARY_ASSERT_STR(e.content, "Paris");
    MARY_ASSERT(!e.finished);
    sewn_chat_event_release(&e);

    const char *last = "{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\"},\"finish_reason\":\"stop\"}]}";
    sewn_chat_event_parse(last, strlen(last), &e);
    MARY_ASSERT_EQ(e.kind, SEWN_CHAT_DELTA);
    MARY_ASSERT(e.finished);
    sewn_chat_event_release(&e);

    sewn_chat_event_parse("[DONE]", 6, &e);
    MARY_ASSERT_EQ(e.kind, SEWN_CHAT_DONE);
    sewn_chat_event_parse("{\"choices\":[]}", 14, &e);
    MARY_ASSERT_EQ(e.kind, SEWN_CHAT_IGNORED);
    sewn_chat_event_parse("not json", 8, &e);
    MARY_ASSERT_EQ(e.kind, SEWN_CHAT_IGNORED);
    MARY_ASSERT(e.holder == NULL);
}

MARY_TEST(speech_bodies_and_events_carry_pcm) {
    assert_json(sewn_speech_body(NULL, "Bonjour.", NULL),
        "{\"model\":\"voxtral-mini-tts-2603\",\"input\":\"Bonjour.\",\"voice_id\":\"fr_marie_neutral\",\"response_format\":\"pcm\",\"stream\":true}");

    const float samples[2] = { 0.5f, -0.25f };
    char b64[16];
    mc_base64_encode((const unsigned char *)samples, sizeof samples, b64, sizeof b64, NULL);
    char flat[64], nested[128];
    snprintf(flat, sizeof flat, "{\"audio_data\":\"%s\"}", b64);
    snprintf(nested, sizeof nested, "{\"event\":\"speech.audio.delta\",\"data\":{\"audio_data\":\"%s\"}}", b64);

    mc_buf pcm = { 0 };
    MARY_ASSERT_EQ(sewn_speech_event("speech.audio.delta", flat, strlen(flat), &pcm), SEWN_SPEECH_AUDIO);
    MARY_ASSERT_EQ(sewn_speech_event("", nested, strlen(nested), &pcm), SEWN_SPEECH_AUDIO);
    MARY_ASSERT_EQ(pcm.len, 2 * sizeof samples);
    float back[4];
    memcpy(back, pcm.data, sizeof back);
    MARY_ASSERT_NEAR(back[0], 0.5, 1e-9);
    MARY_ASSERT_NEAR(back[3], -0.25, 1e-9);

    const char *other = "{\"event\":\"speech.audio.meta\",\"data\":{\"audio_data\":\"AAAA\"}}";
    MARY_ASSERT_EQ(sewn_speech_event("speech.audio.delta", other, strlen(other), &pcm), 0);   /* requireDelta */
    MARY_ASSERT_EQ(sewn_speech_event("speech.audio.done", "{}", 2, &pcm), SEWN_SPEECH_DONE);
    MARY_ASSERT_EQ(sewn_speech_event("", "[DONE]", 6, &pcm), SEWN_SPEECH_DONE);
    MARY_ASSERT_EQ(sewn_speech_event("", "{\"audio_data\":\"@@@\"}", 20, &pcm), -EINVAL);
    MARY_ASSERT_EQ(pcm.len, 2 * sizeof samples);
    mc_buf_free(&pcm);
}

MARY_TEST(voxtral_messages_match_the_sdk) {
    assert_json(sewn_stt_session_update(16000, 480),
        "{\"type\":\"session.update\",\"session\":{\"audio_format\":{\"encoding\":\"pcm_s16le\",\"sample_rate\":16000},\"target_streaming_delay_ms\":480}}");
    assert_json(sewn_stt_session_update(0, 0),
        "{\"type\":\"session.update\",\"session\":{\"audio_format\":{\"encoding\":\"pcm_s16le\",\"sample_rate\":16000}}}");
    const unsigned char pcm[] = { 1, 0, 255, 127 };
    assert_json(sewn_stt_append(pcm, sizeof pcm), "{\"type\":\"input_audio.append\",\"audio\":\"AQD/fw==\"}");
    assert_json(sewn_stt_flush(), "{\"type\":\"input_audio.flush\"}");
    assert_json(sewn_stt_end(), "{\"type\":\"input_audio.end\"}");
    errno = 0;
    MARY_ASSERT(sewn_stt_append(pcm, SEWN_STT_MAX_APPEND + 1) == NULL);
    MARY_ASSERT_EQ(errno, EMSGSIZE);
}

MARY_TEST(voxtral_events_are_classified) {
    sewn_stt_event e;
    const char *delta = "{\"type\":\"transcription.text.delta\",\"text\":\"what is\"}";
    MARY_ASSERT_EQ(sewn_stt_event_parse(delta, strlen(delta), &e), 0);
    MARY_ASSERT_EQ(e.kind, SEWN_STT_TEXT_DELTA);
    MARY_ASSERT_STR(e.text, "what is");
    sewn_stt_event_release(&e);

    const char *done = "{\"type\":\"transcription.done\",\"model\":\"voxtral-mini-transcribe-realtime-2602\",\"text\":\"What is the capital of France?\",\"language\":\"en\",\"usage\":{}}";
    MARY_ASSERT_EQ(sewn_stt_event_parse(done, strlen(done), &e), 0);
    MARY_ASSERT_EQ(e.kind, SEWN_STT_DONE);
    MARY_ASSERT_STR(e.text, "What is the capital of France?");
    sewn_stt_event_release(&e);

    const char *error = "{\"type\":\"error\",\"error\":{\"message\":\"Invalid audio format\",\"code\":4001}}";
    MARY_ASSERT_EQ(sewn_stt_event_parse(error, strlen(error), &e), 0);
    MARY_ASSERT_EQ(e.kind, SEWN_STT_ERROR);
    MARY_ASSERT_STR(e.text, "Invalid audio format");
    MARY_ASSERT_EQ(e.code, 4001);
    sewn_stt_event_release(&e);

    const char *object_message = "{\"type\":\"error\",\"error\":{\"message\":{\"detail\":\"quota\"},\"code\":1}}";
    MARY_ASSERT_EQ(sewn_stt_event_parse(object_message, strlen(object_message), &e), 0);
    MARY_ASSERT_STR(e.text, "{\"detail\":\"quota\"}");
    sewn_stt_event_release(&e);

    const char *created = "{\"type\":\"session.created\",\"session\":{}}";
    MARY_ASSERT_EQ(sewn_stt_event_parse(created, strlen(created), &e), 0);
    MARY_ASSERT_EQ(e.kind, SEWN_STT_SESSION);
    sewn_stt_event_release(&e);

    MARY_ASSERT_EQ(sewn_stt_event_parse("{\"type\":\"transcription.future\"}", 31, &e), 0);
    MARY_ASSERT_EQ(e.kind, SEWN_STT_UNKNOWN);
    sewn_stt_event_release(&e);
    MARY_ASSERT_EQ(sewn_stt_event_parse("{\"text\":\"no type\"}", 18, &e), -EINVAL);
}

int main(void) {
    MARY_RUN(the_chat_body_is_what_run_stream_mistral_sends);
    MARY_RUN(chat_chunks_become_deltas_and_done);
    MARY_RUN(speech_bodies_and_events_carry_pcm);
    MARY_RUN(voxtral_messages_match_the_sdk);
    MARY_RUN(voxtral_events_are_classified);
    MARY_TEST_MAIN_END();
}
