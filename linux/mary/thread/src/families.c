#include "thread/families.h"

#include <string.h>

#include "common/json.h"

#define F(n, t, d) "{\"name\":\"" n "\",\"type\":\"" t "\",\"description\":\"" d "\"}"

const thread_family thread_families[] = {
    { "file", "File", "One record per file on the drive, kept in step by indexd: its text chunked and embedded, media by name.",
      "file-", "files-", "personal", "indexd",
      "[" F("path", "string", "the path under the home folder") "," F("kind", "string", "document, code, image, audio, video, pdf or other")
      "," F("size", "integer", "bytes") "," F("mtime_ms", "integer", "last modified, ms since the epoch") "," F("content_hash", "string", "SHA-256 of the bytes, as decimal")
      "," F("chunks", "integer", "partitions the text became") "," F("tags", "array", "TagGenerator's keywords") "]",
      "a file node; in → folder; opened with → app; about → the concepts extracted from it" },
    { "memory", "Memory", "Auto-memory: a note Sewn writes every seven user messages or on a change of topic.",
      "", "memory-", "personal", "sewnd",
      "[" F("summary", "string", "a bold title then prose, one partition per line") "," F("window_started_ms", "integer", "the first turn summarised")
      "," F("window_ended_ms", "integer", "the last") "]",
      "a memory node; about → concepts; from → the turns it summarises" },
    { "behavior", "Behavior", "A sealed BehavioralEpisode (schema mary.behavior): the request, the ambient capture, and every action taken.",
      "mary-behavior-", "mary-ability-", "behavioral", "maryd",
      "[" F("schema", "string", "mary.behavior") "," F("schemaVersion", "integer", "1") "," F("id", "string", "the turn's UUID")
      "," F("input", "object", "query, ambient capture, prior episode") "," F("output", "object", "actions: the BehavioralActionRecords")
      "," F("provenance", "object", "engine, lane, version") "," F("abilityTargets", "array", "the abilities the episode belongs to") "]",
      "an episode node; records → skill; practices → ability" },
    { "style", "Style profile", "One profile per discipline: the craft, the applications that realize it, and the words they answer to.",
      "mary-style-profile-", "mary-style-", "personal", "maryd",
      "[" F("discipline", "string", "the craft: writing, multimedia, awareness, system-control, window-management") "," F("apps", "array", "the applications that realize it")
      "," F("skills", "array", "their skills, by title") "]",
      "a discipline node; app practices → discipline" },
    { "routing", "Routing habit", "A request Mary settled without a model, kept so the router learns how the user asks.",
      "mary-routing-", "mary-routing-", "behavioral", "maryd",
      "[" F("intent", "string", "the intent") "," F("skill", "string", "the skill that answered") "," F("query", "string", "the bare utterance") "]",
      "utterance → skill" },
    { "unknown", "Unknown", "A record whose family nothing declared — the drift alarm.", "", "", "", "—", "[]", "" },
};
const size_t thread_family_count = sizeof thread_families / sizeof thread_families[0];

const char *const thread_lanes[THREAD_LANE_COUNT] = { "personal", "behavioral" };

const thread_family *thread_family_named(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < thread_family_count; i++) if (strcmp(thread_families[i].name, name) == 0) return &thread_families[i];
    return NULL;
}

int thread_lane_valid(const char *lane) {
    for (int i = 0; lane && i < THREAD_LANE_COUNT; i++) if (strcmp(thread_lanes[i], lane) == 0) return 1;
    return 0;
}

const char *thread_family_lane(const char *family) {
    const thread_family *f = thread_family_named(family);
    return f ? f->lane : "";
}

static const thread_family *longest_prefix(const char *value, int by_group) {
    const thread_family *best = NULL;
    size_t best_len = 0;
    if (!value || !*value) return NULL;
    for (size_t i = 0; i < thread_family_count; i++) {
        const char *prefix = by_group ? thread_families[i].group_prefix : thread_families[i].id_prefix;
        size_t n = strlen(prefix);
        if (n && n > best_len && strncmp(value, prefix, n) == 0) {
            best = &thread_families[i];
            best_len = n;
        }
    }
    return best;
}

const char *thread_family_of(const char *document_id, const char *group_id, const char *metadata_json, size_t metadata_len) {
    if (metadata_json && metadata_len) {
        struct json_object *meta = mc_json_parse(metadata_json, metadata_len);
        const char *named = mc_json_string(meta, "family");
        const thread_family *f = thread_family_named(named);
        json_object_put(meta);
        if (f) return f->name;
    }
    const thread_family *f = longest_prefix(document_id, 0);
    if (!f) f = longest_prefix(group_id, 1);
    return f ? f->name : "unknown";
}

struct json_object *thread_families_json(void) {
    struct json_object *arr = json_object_new_array();
    for (size_t i = 0; i < thread_family_count; i++) {
        const thread_family *f = &thread_families[i];
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "name", json_object_new_string(f->name));
        json_object_object_add(o, "label", json_object_new_string(f->label));
        json_object_object_add(o, "description", json_object_new_string(f->description));
        json_object_object_add(o, "id_prefix", json_object_new_string(f->id_prefix));
        json_object_object_add(o, "group_prefix", json_object_new_string(f->group_prefix));
        json_object_object_add(o, "lane", json_object_new_string(f->lane));
        json_object_object_add(o, "writer", json_object_new_string(f->writer));
        struct json_object *fields = mc_json_parse(f->fields, strlen(f->fields));
        json_object_object_add(o, "fields", fields ? fields : json_object_new_array());
        json_object_object_add(o, "graph", json_object_new_string(f->graph));
        json_object_array_add(arr, o);
    }
    return arr;
}
