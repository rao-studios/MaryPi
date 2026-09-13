/* json-c, the way every package uses it. Getters borrow from the object they
 * read and return false (or NULL) when the key is missing or holds another
 * type — a wrong type is never coerced. */
#ifndef MARY_COMMON_JSON_H
#define MARY_COMMON_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <json-c/json.h>

/* One JSON value spanning all of `text` (surrounding whitespace allowed). NULL
 * for malformed or trailing input. The caller releases it with json_object_put. */
struct json_object *mc_json_parse(const char *text, size_t len);

const char *mc_json_string(struct json_object *obj, const char *key);
bool mc_json_int64(struct json_object *obj, const char *key, int64_t *out);
/* Accepts an integer or a double. */
bool mc_json_double(struct json_object *obj, const char *key, double *out);
bool mc_json_bool(struct json_object *obj, const char *key, bool *out);
struct json_object *mc_json_object(struct json_object *obj, const char *key);
struct json_object *mc_json_array(struct json_object *obj, const char *key);

/* The message's "type" field: every wire Mary speaks tags messages this way. */
static inline const char *mc_json_type(struct json_object *obj) { return mc_json_string(obj, "type"); }

/* Compact text ("/" left unescaped), owned by obj and valid until it changes. */
const char *mc_json_compact(struct json_object *obj, size_t *len);

#endif
