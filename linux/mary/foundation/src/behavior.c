#include "foundation/behavior.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

static const char *const SEAL_NAMES[] = { NULL, "completed", "superseded", "cancelled", "appQuit" };
static const char *const DISPOSITION_NAMES[] = { "succeeded", "failed", "blocked", "deferred", "cancelled", "requestedConfirmation", "unsettled" };
static const char *const INITIATOR_NAMES[] = { "model", "maryRead", "maryAct" };

const char *mf_seal_reason_name(mf_seal_reason r) { return (unsigned)r < 5 ? SEAL_NAMES[r] : NULL; }
const char *mf_disposition_name(mf_disposition d) { return (unsigned)d < 7 ? DISPOSITION_NAMES[d] : "unknown"; }
const char *mf_initiator_name(mf_initiator i) { return (unsigned)i < 3 ? INITIATOR_NAMES[i] : "unknown"; }

bool mf_disposition_did_run(mf_disposition d) {
    switch (d) {
    case MF_DISPOSITION_SUCCEEDED: case MF_DISPOSITION_FAILED: case MF_DISPOSITION_DEFERRED: case MF_DISPOSITION_CANCELLED: case MF_DISPOSITION_UNSETTLED: return true;
    default: return false;
    }
}

void mf_behavior_open(mf_behavior_episode *e, const char *id, const char *query, double opened_at, const char *engine, const char *lane, const char *app_version) {
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id ? id : "");
    snprintf(e->query, sizeof e->query, "%s", query ? query : "");
    e->opened_at = opened_at;
    snprintf(e->engine, sizeof e->engine, "%s", engine ? engine : "");
    snprintf(e->lane, sizeof e->lane, "%s", lane ? lane : "");
    snprintf(e->app_version, sizeof e->app_version, "%s", app_version ? app_version : "");
}

void mf_behavior_free(mf_behavior_episode *e) {
    free(e->ambient_json);
    e->ambient_json = NULL;
}

int mf_behavior_append(mf_behavior_episode *e, const mf_action_record *record) {
    if (e->action_count >= MF_BEHAVIOR_ACTIONS_MAX) return -ENOSPC;
    e->actions[e->action_count++] = *record;
    return 0;
}

void mf_behavior_seal(mf_behavior_episode *e, mf_seal_reason reason, double at) {
    if (e->sealed_reason != MF_SEAL_NONE || reason == MF_SEAL_NONE) return;
    e->sealed_reason = reason;
    e->sealed_at = at;
}

bool mf_behavior_is_sealed(const mf_behavior_episode *e) { return e->sealed_reason != MF_SEAL_NONE; }

bool mf_behavior_did_act(const mf_behavior_episode *e) {
    for (int i = 0; i < e->action_count; i++) if (mf_disposition_did_run(e->actions[i].disposition)) return true;
    return false;
}

static int compare_targets(const void *a, const void *b) {
    const mf_ability_target *x = a, *y = b;
    int c = strcmp(x->ability_id, y->ability_id);
    return c ? c : strcmp(x->paradigm, y->paradigm);
}

void mf_behavior_add_target(mf_behavior_episode *e, const char *ability_id, const char *paradigm) {
    if (!ability_id) return;
    for (int i = 0; i < e->target_count; i++)
        if (strcmp(e->targets[i].ability_id, ability_id) == 0 && strcmp(e->targets[i].paradigm, paradigm ? paradigm : "") == 0) return;
    if (e->target_count >= MF_BEHAVIOR_TARGETS_MAX) return;
    snprintf(e->targets[e->target_count].ability_id, 64, "%s", ability_id);
    snprintf(e->targets[e->target_count].paradigm, 32, "%s", paradigm ? paradigm : "");
    e->target_count++;
    qsort(e->targets, (size_t)e->target_count, sizeof *e->targets, compare_targets);
}

/* ---- dates, ids ---- */

void mf_iso8601(double seconds, char *out, size_t n) {
    time_t whole = (time_t)floor(seconds);
    int ms = (int)lround((seconds - (double)whole) * 1000.0);
    if (ms >= 1000) { whole++; ms -= 1000; }
    struct tm tm;
    gmtime_r(&whole, &tm);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

bool mf_iso8601_parse(const char *text, double *seconds) {
    int y, mo, d, h, mi, s, ms = 0, n = 0;
    if (!text || sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &n) < 6) return false;
    const char *p = text + n;
    if (*p == '.') { p++; int digits = 0; while (*p >= '0' && *p <= '9') { if (digits < 3) ms = ms * 10 + (*p - '0'); digits++; p++; } while (digits < 3) { ms *= 10; digits++; } }
    if (*p != 'Z') return false;
    /* days from civil (Howard Hinnant), so no timegm(3) is needed under strict POSIX */
    int yy = mo <= 2 ? y - 1 : y;
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    if (seconds) *seconds = (double)days * 86400.0 + h * 3600.0 + mi * 60.0 + s + ms / 1000.0;
    return true;
}

void mf_uuid_v4(char out[MF_UUID_LEN + 1]) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b) : -1;
    if (fd >= 0) close(fd);
    if (got != (ssize_t)sizeof b) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t x = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)getpid();
        for (size_t i = 0; i < sizeof b; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; b[i] = (unsigned char)x; }
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    snprintf(out, MF_UUID_LEN + 1, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* ---- the codec: sorted keys, like JSONEncoder.sortedKeys ---- */

#define SORTED (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

static int compare_keys(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* A copy of `o` whose objects have their keys in sorted order, recursively. */
static struct json_object *sorted_copy(struct json_object *o) {
    if (json_object_is_type(o, json_type_object)) {
        const char *keys[256];
        int n = 0;
        json_object_object_foreach(o, key, value) { (void)value; if (n < 256) keys[n++] = key; }
        qsort(keys, (size_t)n, sizeof *keys, compare_keys);
        struct json_object *out = json_object_new_object();
        for (int i = 0; i < n; i++) {
            struct json_object *v;
            json_object_object_get_ex(o, keys[i], &v);
            json_object_object_add(out, keys[i], sorted_copy(v));
        }
        return out;
    }
    if (json_object_is_type(o, json_type_array)) {
        struct json_object *out = json_object_new_array();
        for (size_t i = 0; i < json_object_array_length(o); i++) json_object_array_add(out, sorted_copy(json_object_array_get_idx(o, i)));
        return out;
    }
    return json_object_get(o);
}

char *mf_canonical_json(const char *json) {
    if (!json) return strdup("{}");
    const char *start = json;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    size_t len = strlen(start);
    while (len && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\n' || start[len - 1] == '\r')) len--;
    struct json_tokener *tok = json_tokener_new();
    struct json_object *o = tok ? json_tokener_parse_ex(tok, start, (int)len) : NULL;
    bool whole = o && json_tokener_get_error(tok) == json_tokener_success && json_tokener_get_parse_end(tok) == len && json_object_is_type(o, json_type_object);
    if (tok) json_tokener_free(tok);
    if (!whole) {
        if (o) json_object_put(o);
        return strndup(start, len);
    }
    struct json_object *sorted = sorted_copy(o);
    char *text = strdup(json_object_to_json_string_ext(sorted, SORTED));
    json_object_put(sorted);
    json_object_put(o);
    return text;
}

static struct json_object *str(const char *s) { return json_object_new_string(s ? s : ""); }
static struct json_object *date(double seconds) { char text[40]; mf_iso8601(seconds, text, sizeof text); return str(text); }

static struct json_object *reference_json(const mf_skill_reference *r) {
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "packageID", str(r->package_id));
    json_object_object_add(o, "abilityID", str(r->ability_id));
    json_object_object_add(o, "skillID", str(r->skill_id));
    json_object_object_add(o, "invocationName", str(r->invocation));
    json_object_object_add(o, "version", str(r->version[0] ? r->version : "1.0.0"));
    json_object_object_add(o, "digest", str(r->digest));
    return o;
}

static struct json_object *record_json(const mf_action_record *a) {
    struct json_object *o = json_object_new_object(), *action = json_object_new_object(), *adapters = json_object_new_array();
    json_object_object_add(o, "id", str(a->id));
    json_object_object_add(action, "intention", str(a->intention));
    json_object_object_add(action, "argumentsJSON", str(a->arguments_json[0] ? a->arguments_json : "{}"));
    json_object_object_add(action, "skill", reference_json(&a->skill));
    for (int i = 0; i < a->adapter_count; i++) json_object_array_add(adapters, str(a->adapters[i]));
    json_object_object_add(action, "adapters", adapters);
    json_object_object_add(o, "action", action);
    json_object_object_add(o, "disposition", str(mf_disposition_name(a->disposition)));
    json_object_object_add(o, "summary", str(a->summary));
    json_object_object_add(o, "foundNothing", json_object_new_boolean(a->found_nothing));
    json_object_object_add(o, "undoable", json_object_new_boolean(a->undoable));
    if (a->container_key[0]) json_object_object_add(o, "containerKey", str(a->container_key));
    json_object_object_add(o, "initiator", str(mf_initiator_name(a->initiator)));
    return o;
}

char *mf_behavior_encode(const mf_behavior_episode *e) {
    struct json_object *o = json_object_new_object(), *input = json_object_new_object(), *output = json_object_new_object(), *actions = json_object_new_array(),
                       *provenance = json_object_new_object(), *targets = json_object_new_array();
    json_object_object_add(o, "schema", str(MF_BEHAVIOR_SCHEMA));
    json_object_object_add(o, "schemaVersion", json_object_new_int(MF_BEHAVIOR_SCHEMA_VERSION));
    json_object_object_add(o, "id", str(e->id));
    json_object_object_add(o, "openedAt", date(e->opened_at));
    if (e->sealed_reason != MF_SEAL_NONE) {
        json_object_object_add(o, "sealedAt", date(e->sealed_at));
        json_object_object_add(o, "sealedReason", str(mf_seal_reason_name(e->sealed_reason)));
    }
    json_object_object_add(input, "query", str(e->query));
    if (e->ambient_json) {
        struct json_object *ambient = json_tokener_parse(e->ambient_json);
        if (ambient) json_object_object_add(input, "ambient", ambient);
    }
    if (e->prior_episode_id[0]) json_object_object_add(input, "priorEpisodeID", str(e->prior_episode_id));
    json_object_object_add(o, "input", input);
    for (int i = 0; i < e->action_count; i++) json_object_array_add(actions, record_json(&e->actions[i]));
    json_object_object_add(output, "actions", actions);
    json_object_object_add(o, "output", output);
    json_object_object_add(provenance, "engine", str(e->engine));
    json_object_object_add(provenance, "lane", str(e->lane));
    json_object_object_add(provenance, "appVersion", str(e->app_version));
    json_object_object_add(o, "provenance", provenance);
    for (int i = 0; i < e->target_count; i++) {
        struct json_object *t = json_object_new_object();
        json_object_object_add(t, "abilityID", str(e->targets[i].ability_id));
        json_object_object_add(t, "paradigm", str(e->targets[i].paradigm));
        json_object_array_add(targets, t);
    }
    json_object_object_add(o, "abilityTargets", targets);
    struct json_object *sorted = sorted_copy(o);
    char *text = strdup(json_object_to_json_string_ext(sorted, SORTED));
    json_object_put(sorted);
    json_object_put(o);
    return text;
}

static const char *field(struct json_object *o, const char *key) {
    struct json_object *v;
    return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}

static void copy(char *out, size_t n, const char *s) { snprintf(out, n, "%s", s ? s : ""); }

int mf_behavior_decode(const char *json, size_t len, mf_behavior_episode *out) {
    struct json_tokener *tok = json_tokener_new();
    struct json_object *o = tok ? json_tokener_parse_ex(tok, json, (int)len) : NULL;
    bool ok = o && json_tokener_get_error(tok) == json_tokener_success && json_object_is_type(o, json_type_object);
    if (tok) json_tokener_free(tok);
    if (!ok) { if (o) json_object_put(o); return -EBADMSG; }
    const char *schema = field(o, "schema");
    if (!schema || strcmp(schema, MF_BEHAVIOR_SCHEMA) != 0 || !field(o, "id")) { json_object_put(o); return -EBADMSG; }
    memset(out, 0, sizeof *out);
    copy(out->id, sizeof out->id, field(o, "id"));
    mf_iso8601_parse(field(o, "openedAt"), &out->opened_at);
    if (field(o, "sealedAt")) mf_iso8601_parse(field(o, "sealedAt"), &out->sealed_at);
    const char *reason = field(o, "sealedReason");
    for (int i = 1; reason && i < 5; i++) if (strcmp(SEAL_NAMES[i], reason) == 0) out->sealed_reason = (mf_seal_reason)i;
    struct json_object *input, *output, *provenance, *targets, *actions, *ambient;
    if (json_object_object_get_ex(o, "input", &input)) {
        copy(out->query, sizeof out->query, field(input, "query"));
        copy(out->prior_episode_id, sizeof out->prior_episode_id, field(input, "priorEpisodeID"));
        if (json_object_object_get_ex(input, "ambient", &ambient) && ambient) out->ambient_json = strdup(json_object_to_json_string_ext(ambient, SORTED));
    }
    if (json_object_object_get_ex(o, "output", &output) && json_object_object_get_ex(output, "actions", &actions) && json_object_is_type(actions, json_type_array)) {
        size_t n = json_object_array_length(actions);
        for (size_t i = 0; i < n && out->action_count < MF_BEHAVIOR_ACTIONS_MAX; i++) {
            struct json_object *r = json_object_array_get_idx(actions, i), *action, *skill, *v;
            mf_action_record *a = &out->actions[out->action_count++];
            memset(a, 0, sizeof *a);
            copy(a->id, sizeof a->id, field(r, "id"));
            if (json_object_object_get_ex(r, "action", &action)) {
                copy(a->intention, sizeof a->intention, field(action, "intention"));
                copy(a->arguments_json, sizeof a->arguments_json, field(action, "argumentsJSON"));
                if (json_object_object_get_ex(action, "skill", &skill)) {
                    copy(a->skill.package_id, sizeof a->skill.package_id, field(skill, "packageID"));
                    copy(a->skill.ability_id, sizeof a->skill.ability_id, field(skill, "abilityID"));
                    copy(a->skill.skill_id, sizeof a->skill.skill_id, field(skill, "skillID"));
                    copy(a->skill.invocation, sizeof a->skill.invocation, field(skill, "invocationName"));
                    copy(a->skill.version, sizeof a->skill.version, field(skill, "version"));
                    copy(a->skill.digest, sizeof a->skill.digest, field(skill, "digest"));
                }
                if (json_object_object_get_ex(action, "adapters", &v) && json_object_is_type(v, json_type_array))
                    for (size_t k = 0; k < json_object_array_length(v) && a->adapter_count < 2; k++) copy(a->adapters[a->adapter_count++], 32, json_object_get_string(json_object_array_get_idx(v, k)));
            }
            const char *disposition = field(r, "disposition");
            for (int k = 0; disposition && k < 7; k++) if (strcmp(DISPOSITION_NAMES[k], disposition) == 0) a->disposition = (mf_disposition)k;
            copy(a->summary, sizeof a->summary, field(r, "summary"));
            a->found_nothing = json_object_object_get_ex(r, "foundNothing", &v) && json_object_get_boolean(v);
            a->undoable = json_object_object_get_ex(r, "undoable", &v) && json_object_get_boolean(v);
            copy(a->container_key, sizeof a->container_key, field(r, "containerKey"));
            const char *initiator = field(r, "initiator");
            for (int k = 0; initiator && k < 3; k++) if (strcmp(INITIATOR_NAMES[k], initiator) == 0) a->initiator = (mf_initiator)k;
        }
    }
    if (json_object_object_get_ex(o, "provenance", &provenance)) {
        copy(out->engine, sizeof out->engine, field(provenance, "engine"));
        copy(out->lane, sizeof out->lane, field(provenance, "lane"));
        copy(out->app_version, sizeof out->app_version, field(provenance, "appVersion"));
    }
    if (json_object_object_get_ex(o, "abilityTargets", &targets) && json_object_is_type(targets, json_type_array))
        for (size_t i = 0; i < json_object_array_length(targets); i++) {
            struct json_object *t = json_object_array_get_idx(targets, i);
            mf_behavior_add_target(out, field(t, "abilityID"), field(t, "paradigm"));
        }
    json_object_put(o);
    return 0;
}
