#include "common/json.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct json_object *mc_json_parse(const char *text, size_t len) {
    if (!text || len >= INT_MAX) return NULL;
    /* json-c needs a terminator to finish a top-level number, so parse a copy
     * with its NUL counted. */
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len);
    copy[len] = 0;
    struct json_tokener *tok = json_tokener_new();
    if (!tok) {
        free(copy);
        return NULL;
    }
    struct json_object *obj = json_tokener_parse_ex(tok, copy, (int)len + 1);
    enum json_tokener_error err = json_tokener_get_error(tok);
    size_t end = json_tokener_get_parse_end(tok);
    json_tokener_free(tok);
    if (err != json_tokener_success) {
        json_object_put(obj);
        free(copy);
        return NULL;
    }
    for (; end < len; end++) {
        if (!isspace((unsigned char)copy[end])) {
            json_object_put(obj);
            free(copy);
            return NULL;
        }
    }
    free(copy);
    /* A document that is only whitespace, or the literal null, parses to NULL. */
    return obj;
}

static struct json_object *member(struct json_object *obj, const char *key, enum json_type type) {
    struct json_object *value;
    if (!obj || !json_object_is_type(obj, json_type_object) || !json_object_object_get_ex(obj, key, &value)) return NULL;
    return json_object_is_type(value, type) ? value : NULL;
}

const char *mc_json_string(struct json_object *obj, const char *key) {
    struct json_object *v = member(obj, key, json_type_string);
    return v ? json_object_get_string(v) : NULL;
}

bool mc_json_int64(struct json_object *obj, const char *key, int64_t *out) {
    struct json_object *v = member(obj, key, json_type_int);
    if (!v) return false;
    if (out) *out = json_object_get_int64(v);
    return true;
}

bool mc_json_double(struct json_object *obj, const char *key, double *out) {
    struct json_object *v = member(obj, key, json_type_double);
    if (!v) v = member(obj, key, json_type_int);
    if (!v) return false;
    if (out) *out = json_object_get_double(v);
    return true;
}

bool mc_json_bool(struct json_object *obj, const char *key, bool *out) {
    struct json_object *v = member(obj, key, json_type_boolean);
    if (!v) return false;
    if (out) *out = json_object_get_boolean(v);
    return true;
}

struct json_object *mc_json_object(struct json_object *obj, const char *key) { return member(obj, key, json_type_object); }
struct json_object *mc_json_array(struct json_object *obj, const char *key) { return member(obj, key, json_type_array); }

const char *mc_json_compact(struct json_object *obj, size_t *len) {
    return json_object_to_json_string_length(obj, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE, len);
}
