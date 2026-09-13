#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common/base64.h"
#include "common/frame.h"
#include "common/io.h"
#include "common/json.h"
#include "mary_test.h"
#include "sewn/mistral.h"
#include "sewn/server.h"
#include "sewn/speech.h"
#include "sewn/turn.h"

#define MARY_TEST_PIECE 7

static const char *KEY = "abcdEFGH1234ijklMNOP5678qrst";

/* MARK: - The prompt */

static struct json_object *parse(const char *text) { return mc_json_parse(text, strlen(text)); }

MARY_TEST(the_system_prompt_is_sewns_for_a_turn_with_no_context) {
    struct json_object *start = parse(
        "{\"type\":\"turn.start\",\"request\":{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],"
        "\"persona\":{\"name\":\"Mary\",\"voice\":\"You are Mary.\"},\"instructions\":\"Speak plainly.\"}}");
    sewn_turn_request req;
    MARY_ASSERT_EQ(sewn_turn_request_parse(start, &req), 0);
    char *prompt = sewn_turn_system_prompt(&req);
    MARY_ASSERT_STR(prompt,
        "Your name is Mary.\n\n"
        "You are Mary. You have no retrieved memories or documents for this user. Do not reference, invent, or imply "
        "knowledge of any past conversations, notes, or memories \xE2\x80\x94 respond only from what the user tells you "
        "directly in this conversation.\n\n"
        "--- CONVERSATIONAL INSTRUCTIONS ---\nSpeak plainly.\n\n"
        "Keep responses under 6-7 sentences. Be specific and grounded. Never announce that you are an AI. Never output "
        "XML-like tags (such as <external>) in your response.\n\n");
    free(prompt);
    json_object_put(start);
}

MARY_TEST(request_defaults_follow_sewn) {
    struct json_object *start = parse(
        "{\"type\":\"turn.start\",\"request\":{\"messages\":[],\"model\":\"inkling-small\",\"temperature\":9,\"top_p\":0.5}}");
    sewn_turn_request req;
    MARY_ASSERT_EQ(sewn_turn_request_parse(start, &req), 0);
    MARY_ASSERT_STR(req.model, SEWN_CHAT_MODEL);          /* not a Mistral model: the default */
    MARY_ASSERT_EQ(req.max_tokens, SEWN_DEFAULT_MAX_TOKENS);
    MARY_ASSERT_NEAR(req.temperature, SEWN_DEFAULT_TEMPERATURE, 1e-9);
    MARY_ASSERT_NEAR(req.top_p, 0.5, 1e-9);
    MARY_ASSERT_STR(req.voice_id, SEWN_TTS_VOICE);
    MARY_ASSERT_STR(req.persona_name, "Mary");
    MARY_ASSERT(sewn_turn_messages(&req) == NULL);        /* no user message */
    json_object_put(start);
    struct json_object *bad = parse("{\"type\":\"turn.start\"}");
    MARY_ASSERT_EQ(sewn_turn_request_parse(bad, &req), -EINVAL);
    json_object_put(bad);
}

MARY_TEST(history_is_the_last_ten_turns_then_the_question_then_the_system) {
    mc_buf text = { 0 };
    mc_buf_append_str(&text, "{\"request\":{\"model\":\"mistral-small-latest\",\"messages\":[");
    for (int i = 0; i < 15; i++) {
        char m[96];
        snprintf(m, sizeof m, "%s{\"role\":\"%s\",\"content\":\"m%d\"}", i ? "," : "", i % 2 ? "assistant" : "user", i);
        mc_buf_append_str(&text, m);
    }
    mc_buf_append_str(&text, ",{\"role\":\"system\",\"content\":\"ignored\"},{\"role\":\"assistant\",\"content\":\"\"}]}}");
    struct json_object *start = parse((const char *)text.data);
    sewn_turn_request req;
    MARY_ASSERT_EQ(sewn_turn_request_parse(start, &req), 0);
    MARY_ASSERT_STR(req.model, "mistral-small-latest");
    struct json_object *messages = sewn_turn_messages(&req);
    MARY_ASSERT_EQ(json_object_array_length(messages), 12);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(messages, 0), "content"), "m4");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(messages, 10), "content"), "m14");
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(messages, 11), "role"), "system");
    json_object_put(messages);
    json_object_put(start);
    mc_buf_free(&text);
}

/* MARK: - The turn, against a scripted Mistral */

struct script {
    const char *chat;           /* SSE, fed in 7-byte pieces */
    long chat_status;
    int chat_rc;
    bool chat_waits_for_stop;   /* after its first token, until should_stop */
    char speech[1024];
    long speech_status;
    int speech_rc;
    char chat_body[8192];
    char speech_inputs[4][256];
    int speech_calls;
    atomic_bool saw_stop;
    const char *completion;     /* the JSON body for an unstreamed chat call (compaction, memory) */
    int completions;
    char completion_input[4096];
};

static struct script script;

static void pause_ms(int ms) {
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}

static int scripted(const char *path, const char *key, const char *body, size_t body_len, sewn_bytes_fn on_bytes,
                    sewn_stop_fn should_stop, void *user, long *status, char *message, size_t cap, void *transport_user) {
    struct script *s = transport_user;
    bool chat = strcmp(path, SEWN_MISTRAL_CHAT_PATH) == 0;
    if (strcmp(key, KEY) != 0) MARY_FAIL("the transport was handed another key");
    if (chat && !strstr(body, "\"stream\":true")) {       /* the body is mc_json_compact's, NUL-terminated */
        /* an unstreamed completion: answered whole */
        s->completions++;
        snprintf(s->completion_input, sizeof s->completion_input, "%.*s", (int)body_len, body);
        *status = 200;
        const char *answer = s->completion ? s->completion : "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"**A note**\\n\\nThe user asked about the capital of France several times over.\"}}]}";
        on_bytes(answer, strlen(answer), 200, user);
        return 0;
    }
    if (chat) {
        snprintf(s->chat_body, sizeof s->chat_body, "%.*s", (int)body_len, body);
    } else {
        struct json_object *b = mc_json_parse(body, body_len);
        if (s->speech_calls < 4) snprintf(s->speech_inputs[s->speech_calls], 256, "%s", mc_json_string(b, "input"));
        s->speech_calls++;
        json_object_put(b);
    }
    *status = chat ? s->chat_status : s->speech_status;
    int rc = chat ? s->chat_rc : s->speech_rc;
    if (rc < 0) {
        snprintf(message, cap, "the network is down");
        return rc;
    }
    const char *stream = chat ? s->chat : s->speech;
    size_t n = strlen(stream);
    const char *first_end = strstr(stream, "\n\n");
    size_t first_event = first_end ? (size_t)(first_end - stream) + 2 : n;
    bool waited = false;
    for (size_t i = 0; i < n; i += 7) {
        if (should_stop(user)) {
            atomic_store(&s->saw_stop, true);
            return -ECANCELED;
        }
        size_t piece = n - i < 7 ? n - i : 7;
        if (on_bytes(stream + i, piece, *status, user)) return SEWN_STREAM_STOPPED;
        /* Once the first event (a token) is out, hold the stream open until the client acts. */
        if (chat && s->chat_waits_for_stop && !waited && i + piece >= first_event) {
            waited = true;
            for (int k = 0; k < 400 && !should_stop(user); k++) pause_ms(5);
            if (should_stop(user)) {
                atomic_store(&s->saw_stop, true);
                return -ECANCELED;
            }
        }
    }
    return 0;
}

static char dir[64];
static sewn_service svc;

static void setup(bool with_key) {
    memset(&script, 0, sizeof script);
    script.chat =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Paris is the capital\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\" of France. It is\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\" **lovely**.\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    const unsigned char samples[8] = { 0, 0, 128, 63, 0, 0, 128, 191 };   /* 1.0, -1.0 */
    char b64[16];
    mc_base64_encode(samples, sizeof samples, b64, sizeof b64, NULL);
    snprintf(script.speech, sizeof script.speech,
             "event: speech.audio.delta\ndata: {\"event\":\"speech.audio.delta\",\"data\":{\"audio_data\":\"%s\"}}\n\n"
             "event: speech.audio.done\ndata: {\"event\":\"speech.audio.done\",\"data\":{}}\n\n", b64);
    snprintf(dir, sizeof dir, "/tmp/sewn-turn-XXXXXX");
    MARY_ASSERT(mkdtemp(dir) != NULL);
    sewn_service_init(&svc, dir);
    svc.post_stream = scripted;
    svc.post_stream_user = &script;
    svc.retrieve = NULL;        /* no Thread unless a test scripts one */
    svc.deposit = NULL;
    if (with_key) MARY_ASSERT_EQ(sewn_key_store_set(&svc.keys, KEY, strlen(KEY)), 0);
}

static void teardown(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/mistral.key", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/calls.jsonl", dir);
    unlink(path);
    sewn_service_free(&svc);
    rmdir(dir);
}

struct serving {
    int fd;
};

static void *serve(void *arg) {
    struct serving *s = arg;
    sewn_peer peer = { .uid = 1000, .gid = 1000, .pid = 1, .known = true };
    sewn_serve_connection(&svc, s->fd, &peer);
    close(s->fd);
    return NULL;
}

struct seen {
    char types[64][24];
    int count;
    char text[512];
    size_t pcm_bytes;
    bool pcm_misaligned;
    char error_stage[32];
    char error_message[256];
    int client_fd;
    int act_on_first_token;     /* 1: send cancel, 2: close */
    long tts_status;
    char tts_message[256];
    bool saw_key;
    char end[8192];             /* turn.end, compact */
    char retrieval[2048];       /* the retrieval frame */
};

static int on_frame(uint8_t kind, const unsigned char *bytes, size_t len, void *user) {
    struct seen *s = user;
    if (kind == MC_FRAME_PCM) {
        s->pcm_bytes += len;
        if (len % 4) s->pcm_misaligned = true;
        if (s->count < 64) snprintf(s->types[s->count++], 24, "pcm");
        return 0;
    }
    char copy[4096];
    snprintf(copy, sizeof copy, "%.*s", (int)(len < sizeof copy - 1 ? len : sizeof copy - 1), (const char *)bytes);
    if (strstr(copy, KEY)) s->saw_key = true;
    struct json_object *msg = mc_json_parse((const char *)bytes, len);
    const char *type = mc_json_type(msg);
    if (s->count < 64) snprintf(s->types[s->count++], 24, "%s", type ? type : "?");
    int stop = 0;
    if (type && strcmp(type, "token") == 0) {
        strncat(s->text, mc_json_string(msg, "text"), sizeof s->text - strlen(s->text) - 1);
        if (s->act_on_first_token == 1) {
            const char *cancel = "{\"type\":\"cancel\"}";
            mc_frame_write_fd(s->client_fd, MC_FRAME_JSON, (const unsigned char *)cancel, strlen(cancel));
            s->act_on_first_token = 0;
        } else if (s->act_on_first_token == 2) {
            stop = 1;
        }
    }
    if (type && strcmp(type, "tts.failed") == 0) {
        int64_t status = 0;
        mc_json_int64(msg, "status", &status);
        s->tts_status = (long)status;
        const char *why = mc_json_string(msg, "message");
        snprintf(s->tts_message, sizeof s->tts_message, "%s", why ? why : "");
    }
    if (type && strcmp(type, "error") == 0) {
        snprintf(s->error_stage, sizeof s->error_stage, "%s", mc_json_string(msg, "stage"));
        snprintf(s->error_message, sizeof s->error_message, "%s", mc_json_string(msg, "message"));
    }
    if (type && strcmp(type, "turn.end") == 0) snprintf(s->end, sizeof s->end, "%s", mc_json_compact(msg, NULL));
    if (type && strcmp(type, "retrieval") == 0) snprintf(s->retrieval, sizeof s->retrieval, "%s", mc_json_compact(msg, NULL));
    json_object_put(msg);
    return stop;
}

static int index_of(const struct seen *s, const char *type) {
    for (int i = 0; i < s->count; i++) if (strcmp(s->types[i], type) == 0) return i;
    return -1;
}

static void run_turn(struct seen *seen, const char *request) {
    int sv[2];
    MARY_ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    seen->client_fd = sv[0];
    MARY_ASSERT_EQ(mc_frame_write_fd(sv[0], MC_FRAME_JSON, (const unsigned char *)request, strlen(request)), 0);
    struct serving serving = { sv[1] };
    pthread_t thread;
    pthread_create(&thread, NULL, serve, &serving);
    mc_frame_reader reader;
    mc_frame_reader_init(&reader, 0, false);
    int status;
    while ((status = mc_frame_reader_read_fd(&reader, sv[0], on_frame, seen)) == MC_IO_OK) {}
    close(sv[0]);   /* after a stop this is the client hanging up */
    pthread_join(thread, NULL);
    mc_frame_reader_free(&reader);
}

static const char *TURN =
    "{\"type\":\"turn.start\",\"request\":{\"messages\":[{\"role\":\"user\",\"content\":\"What is the capital of France?\"}],"
    "\"max_tokens\":1200,\"temperature\":0.4,\"top_p\":0.9,\"persona\":{\"name\":\"Mary\",\"voice\":\"You are Mary.\"}},"
    "\"tts\":{\"voice_id\":\"fr_marie_neutral\"}}";

MARY_TEST(a_turn_streams_words_then_speaks_them_in_order) {
    setup(true);
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_STR(seen.types[0], "phase");
    MARY_ASSERT_STR(seen.text, "Paris is the capital of France. It is **lovely**.");
    int begin = index_of(&seen, "audio.begin"), pcm = index_of(&seen, "pcm");
    MARY_ASSERT(begin > 0 && pcm > begin);
    MARY_ASSERT_EQ(seen.pcm_bytes, 16);          /* two sentences, 8 bytes each */
    MARY_ASSERT(!seen.pcm_misaligned);
    MARY_ASSERT_STR(seen.types[seen.count - 1], "turn.end");
    MARY_ASSERT_EQ(index_of(&seen, "tts.failed"), -1);
    MARY_ASSERT_EQ(script.speech_calls, 2);
    MARY_ASSERT_STR(script.speech_inputs[0], "Paris is the capital of France.");
    MARY_ASSERT_STR(script.speech_inputs[1], "It is lovely.");   /* markdown is shown, not spoken */
    struct json_object *body = parse(script.chat_body);
    MARY_ASSERT_STR(mc_json_string(body, "model"), SEWN_CHAT_MODEL);
    int64_t max_tokens = 0;
    MARY_ASSERT(mc_json_int64(body, "max_tokens", &max_tokens) && max_tokens == 1200);
    struct json_object *messages = mc_json_array(body, "messages");
    MARY_ASSERT_EQ(json_object_array_length(messages), 2);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(messages, 1), "role"), "system");
    MARY_ASSERT(mc_json_object(body, "repetition_penalty") == NULL && !mc_json_double(body, "repetition_penalty", NULL));
    json_object_put(body);
    teardown();
}

MARY_TEST(a_cancel_frame_stops_the_turn_without_turn_end) {
    setup(true);
    script.chat_waits_for_stop = true;
    struct seen seen = { .act_on_first_token = 1 };
    run_turn(&seen, TURN);
    MARY_ASSERT(atomic_load(&script.saw_stop));
    MARY_ASSERT_EQ(index_of(&seen, "turn.end"), -1);
    teardown();
}

MARY_TEST(a_client_that_hangs_up_stops_the_turn) {
    setup(true);
    script.chat_waits_for_stop = true;
    struct seen seen = { .act_on_first_token = 2 };
    run_turn(&seen, TURN);
    MARY_ASSERT(atomic_load(&script.saw_stop));
    teardown();
}

MARY_TEST(failed_speech_leaves_the_words) {
    setup(true);
    script.speech_rc = -EIO;
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_STR(seen.text, "Paris is the capital of France. It is **lovely**.");
    MARY_ASSERT(index_of(&seen, "tts.failed") > 0);
    MARY_ASSERT_STR(seen.tts_message, "the network is down");
    MARY_ASSERT_EQ(index_of(&seen, "audio.begin"), -1);
    MARY_ASSERT_STR(seen.types[seen.count - 1], "turn.end");
    MARY_ASSERT_EQ(script.speech_calls, 1);      /* the lane ends at its first failure */
    teardown();
}

MARY_TEST(a_refused_key_is_reported_and_the_turn_still_ends) {
    setup(true);
    script.chat = "{\"message\":\"Unauthorized\"}";
    script.chat_status = 401;
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_STR(seen.error_stage, "grounded");
    MARY_ASSERT_STR(seen.error_message, "Mistral rejected the key");
    MARY_ASSERT_EQ(index_of(&seen, "token"), -1);
    MARY_ASSERT_STR(seen.types[seen.count - 1], "turn.end");
    teardown();
}

MARY_TEST(no_key_means_no_turn) {
    setup(false);
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_EQ(seen.count, 1);
    MARY_ASSERT_STR(seen.error_stage, "key");
    MARY_ASSERT_EQ(script.speech_calls, 0);
    MARY_ASSERT_STR(script.chat_body, "");
    teardown();
}

MARY_TEST(audio_split_mid_sample_goes_out_whole) {
    setup(true);
    const unsigned char six[6] = { 1, 2, 3, 4, 5, 6 }, two[2] = { 7, 8 };
    char a[16], b[16];
    mc_base64_encode(six, 6, a, sizeof a, NULL);
    mc_base64_encode(two, 2, b, sizeof b, NULL);
    snprintf(script.speech, sizeof script.speech, "data: {\"audio_data\":\"%s\"}\n\ndata: {\"audio_data\":\"%s\"}\n\ndata: [DONE]\n\n", a, b);
    script.chat = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Yes.\"},\"finish_reason\":\"stop\"}]}\n\n";
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_EQ(seen.pcm_bytes, 8);
    MARY_ASSERT(!seen.pcm_misaligned);
    teardown();
}

MARY_TEST(refused_speech_says_what_mistral_said) {
    setup(true);
    snprintf(script.speech, sizeof script.speech,
             "{\"object\":\"error\",\"message\":\"Invalid voice: fr_nobody\",\"type\":\"invalid_request_error\"}");
    script.speech_status = 422;
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_STR(seen.text, "Paris is the capital of France. It is **lovely**.");
    MARY_ASSERT(index_of(&seen, "tts.failed") > 0);
    MARY_ASSERT_EQ(seen.tts_status, 422);
    MARY_ASSERT_STR(seen.tts_message, "Mistral answered HTTP 422: Invalid voice: fr_nobody");
    MARY_ASSERT_STR(seen.types[seen.count - 1], "turn.end");
    MARY_ASSERT_EQ(script.speech_calls, 1);
    MARY_ASSERT(!seen.saw_key);
    teardown();
}

MARY_TEST(forbidden_speech_is_not_a_refused_key) {
    setup(true);
    snprintf(script.speech, sizeof script.speech, "{\"detail\":[{\"type\":\"moderation\",\"msg\":\"The input\\nwas blocked\"}]}");
    script.speech_status = 403;
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_EQ(seen.tts_status, 403);
    MARY_ASSERT_STR(seen.tts_message, "Mistral refused to speak it (HTTP 403: The input was blocked)");
    teardown();
}

MARY_TEST(an_oversize_speech_event_fails_the_lane) {
    setup(true);
    svc.speech_line_max = 64;                   /* the audio event in setup's script is longer */
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT(index_of(&seen, "tts.failed") > 0);
    MARY_ASSERT_EQ(index_of(&seen, "audio.begin"), -1);
    MARY_ASSERT_STR(seen.tts_message, "Mistral's speech came in a piece too large to read");
    MARY_ASSERT_EQ(script.speech_calls, 1);
    teardown();
}

MARY_TEST(speech_that_carries_no_audio_fails) {
    setup(true);
    snprintf(script.speech, sizeof script.speech, "event: speech.audio.done\ndata: {\"event\":\"speech.audio.done\",\"data\":{}}\n\n");
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT(index_of(&seen, "tts.failed") > 0);
    MARY_ASSERT_STR(seen.tts_message, "Mistral answered with no audio");
    teardown();
}

MARY_TEST(bare_json_lines_are_spoken_too) {
    setup(true);
    const unsigned char samples[8] = { 0, 0, 128, 63, 0, 0, 128, 191 };
    char b64[16];
    mc_base64_encode(samples, sizeof samples, b64, sizeof b64, NULL);
    snprintf(script.speech, sizeof script.speech, "{\"audio_data\":\"%s\"}\n{\"event\":\"speech.audio.done\",\"data\":{}}\n", b64);
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_EQ(index_of(&seen, "tts.failed"), -1);
    MARY_ASSERT_EQ(seen.pcm_bytes, 16);
    teardown();
}

MARY_TEST(a_refusal_never_repeats_the_key_or_the_words) {
    char out[256];
    const char *noisy = "{\"message\":\"bad\\u0007  thing\\t here\"}";
    sewn_speech_failure(400, noisy, strlen(noisy), out, sizeof out);
    MARY_ASSERT_STR(out, "Mistral answered HTTP 400: bad thing here");
    sewn_speech_failure(502, "<html>Bad gateway</html>", 24, out, sizeof out);
    MARY_ASSERT_STR(out, "Mistral answered HTTP 502");
    sewn_speech_failure(401, "{\"message\":\"whatever\"}", 22, out, sizeof out);
    MARY_ASSERT_STR(out, "Mistral rejected the key");
    sewn_speech_failure(500, "Upstream timed out", 18, out, sizeof out);
    MARY_ASSERT_STR(out, "Mistral answered HTTP 500: Upstream timed out");
    char long_reason[512] = "{\"message\":\"";
    for (int i = 0; i < 150; i++) strcat(long_reason, "\xC3\xA9");   /* 300 bytes of \u00e9 */
    strcat(long_reason, "\"}");
    sewn_speech_failure(400, long_reason, strlen(long_reason), out, sizeof out);
    size_t n = strlen(out);
    MARY_ASSERT(n < 200);
    MARY_ASSERT(((unsigned char)out[n - 1] & 0xC0) == 0x80 && (unsigned char)out[n - 2] == 0xC3);   /* ends on a whole character */

    setup(true);                                /* Mistral echoing the words back: the reason is dropped */
    snprintf(script.speech, sizeof script.speech, "{\"message\":\"cannot say Paris is the capital of France.\"}");
    script.speech_status = 400;
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_STR(seen.tts_message, "Mistral answered HTTP 400");
    teardown();
}

/* MARK: - The Thread around the turn */

static int retrieve_calls;
static char retrieve_owner[64], retrieve_lanes[128];

/* Two of the owner's own documents, whatever the query. */
static int scripted_retrieve(const sewn_scope *scope, const char *query, int top_k, sewn_retrieval *out, char *message, size_t cap, void *user) {
    retrieve_calls++;
    snprintf(retrieve_owner, sizeof retrieve_owner, "%s", scope->owner_id ? scope->owner_id : "");
    retrieve_lanes[0] = 0;
    for (size_t i = 0; i < scope->n_lanes; i++) {
        strcat(retrieve_lanes, i ? "," : "");
        strcat(retrieve_lanes, scope->lanes[i]);
    }
    char result[1024];
    snprintf(result, sizeof result,
             "{\"results\":[{\"partition_id\":\"p1\",\"document_id\":\"file-1\",\"owner_id\":\"%s\",\"text\":\"Paris is the capital of France, a note I keep.\","
             "\"name\":\"notes.txt\",\"family\":\"file\",\"lane\":\"personal\",\"group_id\":\"files-%s\",\"score\":1.5},"
             "{\"partition_id\":\"p2\",\"document_id\":\"mary-turn-9\",\"owner_id\":\"%s\",\"text\":\"It is lovely in spring, you said.\","
             "\"name\":\"mary-turn-9\",\"family\":\"conversation\",\"lane\":\"conversation\",\"group_id\":\"conversation-%s\",\"score\":2.5}],\"ms\":3}",
             retrieve_owner, retrieve_owner, retrieve_owner, retrieve_owner);
    struct json_object *o = parse(result);
    int rc = sewn_retrieval_parse(o, out);
    json_object_put(o);
    return rc;
}

static int deposit_calls;
static char deposit_owner[64], deposit_group[64], deposit_label[64], deposit_texts[1024];

static int scripted_deposit(const char *owner_id, const char *group_id, const char *label, const char *family, const char *const *texts, size_t n,
                            char *document_id, size_t cap, void *user) {
    deposit_calls++;
    snprintf(deposit_owner, sizeof deposit_owner, "%s", owner_id);
    snprintf(deposit_group, sizeof deposit_group, "%s", group_id);
    snprintf(deposit_label, sizeof deposit_label, "%s", label);
    deposit_texts[0] = 0;
    for (size_t i = 0; i < n; i++) {
        strcat(deposit_texts, i ? "|" : "");
        strncat(deposit_texts, texts[i], sizeof deposit_texts - strlen(deposit_texts) - 2);
    }
    snprintf(document_id, cap, "memory-doc");
    return 0;
}

static const char *TURN_TINKER =
    "{\"type\":\"turn.start\",\"request\":{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"provider\":\"tinker\"}}";

MARY_TEST(an_engine_that_is_not_served_is_refused_before_any_call) {
    setup(true);
    struct seen seen = { 0 };
    run_turn(&seen, TURN_TINKER);
    MARY_ASSERT_STR(seen.error_stage, "engine");
    MARY_ASSERT_STR(seen.error_message, SEWN_ENGINE_UNAVAILABLE);
    MARY_ASSERT_EQ(index_of(&seen, "turn.end"), -1);
    MARY_ASSERT_STR(script.chat_body, "");                      /* no transport was opened */
    struct json_object *calls = sewn_calls_list(&svc.calls, 0);
    MARY_ASSERT_EQ(json_object_array_length(calls), 0);         /* and the ledger has no row */
    json_object_put(calls);
    teardown();
}

MARY_TEST(a_turn_with_context_cites_its_sources_and_strips_the_markers) {
    setup(true);
    svc.retrieve = scripted_retrieve;
    retrieve_calls = 0;
    script.chat =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Paris is the capital[\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"[1]] of France. It is\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\" **lovely**.[[2]]\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct seen seen = { 0 };
    run_turn(&seen, TURN);
    MARY_ASSERT_EQ(retrieve_calls, 1);
    MARY_ASSERT_STR(retrieve_lanes, "conversation,personal");    /* the default lanes */
    MARY_ASSERT(strlen(retrieve_owner) > 0);                      /* the connection's user, not the request's */
    MARY_ASSERT_STR(seen.text, "Paris is the capital of France. It is **lovely**.");   /* markers gone, even split across deltas */
    MARY_ASSERT(index_of(&seen, "retrieval") > 0 && index_of(&seen, "retrieval") < index_of(&seen, "token"));
    MARY_ASSERT_STR(script.speech_inputs[0], "Paris is the capital of France.");
    MARY_ASSERT_EQ(script.speech_calls, 2);
    struct json_object *body = parse(script.chat_body);
    struct json_object *messages = mc_json_array(body, "messages");
    MARY_ASSERT_EQ(json_object_array_length(messages), 2);        /* verbatim: the question, then the system */
    const char *system = mc_json_string(json_object_array_get_idx(messages, 1), "content");
    MARY_ASSERT(strstr(system, "--- CONTEXT ---") != NULL);
    MARY_ASSERT(strstr(system, "[1] \"notes.txt\"") != NULL);
    MARY_ASSERT(strstr(system, "**Conversation (what was said before") != NULL);
    MARY_ASSERT(strstr(system, "[[n]]") != NULL);                  /* the citation protocol */
    MARY_ASSERT(strstr(system, "no retrieved memories") == NULL);
    json_object_put(body);
    struct json_object *end = parse(seen.end);
    MARY_ASSERT(end != NULL);
    MARY_ASSERT_STR(mc_json_string(end, "text"), "Paris is the capital of France. It is **lovely**.");
    struct json_object *owners = mc_json_array(mc_json_object(end, "contribution"), "owners");
    MARY_ASSERT_EQ(json_object_array_length(owners), 1);
    struct json_object *owner = json_object_array_get_idx(owners, 0);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(owner, "document_ids")), 2);
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(owner, "spans")), 1);   /* the two sentences touch: one span */
    struct json_object *doc_spans = mc_json_object(owner, "document_spans");
    MARY_ASSERT(doc_spans && mc_json_array(doc_spans, "file-1") && mc_json_array(doc_spans, "mary-turn-9"));
    MARY_ASSERT_EQ(json_object_array_length(mc_json_array(end, "retrieved")), 2);
    MARY_ASSERT_STR(mc_json_string(json_object_array_get_idx(mc_json_array(end, "retrieved"), 1), "lane"), "conversation");
    json_object_put(end);
    MARY_ASSERT_EQ(script.completions, 0);                        /* small context: no briefing call */
    teardown();
}

MARY_TEST(every_seventh_user_message_writes_a_memory) {
    setup(true);
    svc.retrieve = scripted_retrieve;
    svc.deposit = scripted_deposit;
    deposit_calls = 0;
    mc_buf text = { 0 };
    mc_buf_append_str(&text, "{\"type\":\"turn.start\",\"request\":{\"messages\":[");
    for (int i = 0; i < 13; i++) {
        char m[96];
        snprintf(m, sizeof m, "%s{\"role\":\"%s\",\"content\":\"m%d\"}", i ? "," : "", i % 2 ? "assistant" : "user", i);
        mc_buf_append_str(&text, m);
    }
    mc_buf_append_str(&text, "],\"sewn\":{\"owner_id\":\"somebody-else\",\"lanes\":[\"personal\"]}}}");
    struct seen seen = { 0 };
    run_turn(&seen, (const char *)text.data);          /* 7 user messages */
    MARY_ASSERT_STR(seen.types[seen.count - 1], "turn.end");
    MARY_ASSERT_STR(retrieve_lanes, "personal");
    MARY_ASSERT_EQ(script.completions, 1);              /* the note */
    MARY_ASSERT(strstr(script.completion_input, "[user]: m12") != NULL);
    MARY_ASSERT(strstr(script.completion_input, "mistral-tiny") != NULL);
    MARY_ASSERT_EQ(deposit_calls, 1);
    MARY_ASSERT_STR(deposit_label, "Memory");
    MARY_ASSERT(strncmp(deposit_group, "memory-", 7) == 0);
    MARY_ASSERT_STR(deposit_owner, retrieve_owner);     /* the connection's user, never "somebody-else" */
    MARY_ASSERT_STR(deposit_texts, "The user asked about the capital of France several times over.");   /* the short title is dropped */
    mc_buf_free(&text);
    teardown();
    setup(true);
    svc.deposit = scripted_deposit;
    deposit_calls = 0;
    seen = (struct seen){ 0 };
    run_turn(&seen, TURN);                              /* one user message: no memory, and no embedding to check the topic */
    MARY_ASSERT_EQ(deposit_calls, 0);
    MARY_ASSERT_EQ(script.completions, 0);
    teardown();
}

int main(void) {
    MARY_RUN(the_system_prompt_is_sewns_for_a_turn_with_no_context);
    MARY_RUN(request_defaults_follow_sewn);
    MARY_RUN(history_is_the_last_ten_turns_then_the_question_then_the_system);
    MARY_RUN(a_turn_streams_words_then_speaks_them_in_order);
    MARY_RUN(a_cancel_frame_stops_the_turn_without_turn_end);
    MARY_RUN(a_client_that_hangs_up_stops_the_turn);
    MARY_RUN(failed_speech_leaves_the_words);
    MARY_RUN(a_refused_key_is_reported_and_the_turn_still_ends);
    MARY_RUN(no_key_means_no_turn);
    MARY_RUN(audio_split_mid_sample_goes_out_whole);
    MARY_RUN(refused_speech_says_what_mistral_said);
    MARY_RUN(forbidden_speech_is_not_a_refused_key);
    MARY_RUN(an_oversize_speech_event_fails_the_lane);
    MARY_RUN(speech_that_carries_no_audio_fails);
    MARY_RUN(bare_json_lines_are_spoken_too);
    MARY_RUN(a_refusal_never_repeats_the_key_or_the_words);
    MARY_RUN(an_engine_that_is_not_served_is_refused_before_any_call);
    MARY_RUN(a_turn_with_context_cites_its_sources_and_strips_the_markers);
    MARY_RUN(every_seventh_user_message_writes_a_memory);
    MARY_TEST_MAIN_END();
}
