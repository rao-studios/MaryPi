#include "brain/request.h"

#include <stdio.h>

#include "common/json.h"

const char MB_PERSONA_VOICE[] =
    "You are Mary \xE2\x80\x94 that is your name; always identify as Mary, never any other assistant name. You are a voice "
    "assistant living in MaryOS, on the user's own computer: a warm, knowledgeable sibling and good company first. "
    "Match their register: when they are chatting, talk back; a question back is company. If they asked for "
    "something, name the heading in one beat and keep talking \xE2\x80\x94 never \"opening that\", \"adding that now\", or "
    "any result you have not been given.";

static struct json_object *number(double value) {
    char text[32];
    snprintf(text, sizeof text, "%.15g", value);
    return json_object_new_double_s(value, text);
}

struct json_object *mb_turn_start(const mb_history *history, const mb_turn_request *r) {
    struct json_object *request = json_object_new_object();
    json_object_object_add(request, "messages", mb_history_spoken_messages(history));
    json_object_object_add(request, "max_tokens", json_object_new_int(1200));
    json_object_object_add(request, "temperature", number(0.4));
    json_object_object_add(request, "top_p", number(0.9));
    json_object_object_add(request, "stream", json_object_new_boolean(1));
    struct json_object *stop = json_object_new_array();
    json_object_array_add(stop, json_object_new_string("###"));
    json_object_array_add(stop, json_object_new_string("END"));
    json_object_object_add(request, "stop", stop);
    json_object_object_add(request, "repetition_penalty", number(1.1));
    json_object_object_add(request, "repetition_context_size", json_object_new_int(20));
    if (r->instructions) json_object_object_add(request, "instructions", json_object_new_string(r->instructions));
    json_object_object_add(request, "client", json_object_new_string("mary"));
    json_object_object_add(request, "provider", json_object_new_string("mistral"));
    struct json_object *persona = json_object_new_object();
    json_object_object_add(persona, "name", json_object_new_string(MB_PERSONA_NAME));
    json_object_object_add(persona, "voice", json_object_new_string(MB_PERSONA_VOICE));
    json_object_object_add(request, "persona", persona);
    struct json_object *sewn = json_object_new_object();
    json_object_object_add(sewn, "owner_id", json_object_new_string(r->owner_id ? r->owner_id : ""));
    json_object_object_add(sewn, "scope", json_object_new_string("personal"));
    json_object_object_add(sewn, "aggregate", json_object_new_boolean(1));
    json_object_object_add(sewn, "request_id", json_object_new_string(r->request_id ? r->request_id : ""));
    json_object_object_add(request, "sewn", sewn);

    struct json_object *start = json_object_new_object(), *tts = json_object_new_object();
    json_object_object_add(start, "type", json_object_new_string("turn.start"));
    json_object_object_add(start, "request", request);
    json_object_object_add(tts, "voice_id", json_object_new_string(r->voice_id ? r->voice_id : MB_VOICE_ID));
    json_object_object_add(start, "tts", tts);
    return start;
}
