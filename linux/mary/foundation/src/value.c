#include "foundation/value.h"

#include <stdlib.h>
#include <string.h>

#include "foundation/envelope.h"

static mf_value *make(mf_value_kind kind) {
    mf_value *v = calloc(1, sizeof *v);
    if (v) v->kind = kind;
    return v;
}

mf_value *mf_value_null(void) { return make(MF_VALUE_NULL); }
mf_value *mf_value_object(void) { return make(MF_VALUE_OBJECT); }
mf_value *mf_value_array(void) { return make(MF_VALUE_ARRAY); }

mf_value *mf_value_string(const char *s) {
    mf_value *v = make(MF_VALUE_STRING);
    if (v && !(v->as.string = strdup(s ? s : ""))) { free(v); return NULL; }
    return v;
}

mf_value *mf_value_boolean(bool b) {
    mf_value *v = make(MF_VALUE_BOOLEAN);
    if (v) v->as.boolean = b;
    return v;
}

mf_value *mf_value_integer(int64_t i) {
    mf_value *v = make(MF_VALUE_INTEGER);
    if (v) v->as.integer = i;
    return v;
}

mf_value *mf_value_number(double n) {
    mf_value *v = make(MF_VALUE_NUMBER);
    if (v) v->as.number = n;
    return v;
}

mf_value *mf_value_data(const unsigned char *bytes, size_t length) {
    mf_value *v = make(MF_VALUE_DATA);
    if (!v) return NULL;
    if (length) {
        v->as.data.bytes = malloc(length);
        if (!v->as.data.bytes) { free(v); return NULL; }
        memcpy(v->as.data.bytes, bytes, length);
    }
    v->as.data.length = length;
    return v;
}

int mf_value_object_set(mf_value *object, const char *key, mf_value *value) {
    if (!object || object->kind != MF_VALUE_OBJECT || !key) { mf_value_free(value); return -1; }
    for (size_t i = 0; i < object->as.object.count; i++) {
        if (strcmp(object->as.object.members[i].key, key) == 0) {
            mf_value_free(object->as.object.members[i].value);
            object->as.object.members[i].value = value;
            return 0;
        }
    }
    mf_member *grown = realloc(object->as.object.members, (object->as.object.count + 1) * sizeof *grown);
    char *copy = strdup(key);
    if (!grown || !copy) {
        if (grown) object->as.object.members = grown;
        free(copy);
        mf_value_free(value);
        return -1;
    }
    object->as.object.members = grown;
    grown[object->as.object.count++] = (mf_member){ .key = copy, .value = value };
    return 0;
}

const mf_value *mf_value_object_get(const mf_value *object, const char *key) {
    if (!object || object->kind != MF_VALUE_OBJECT || !key) return NULL;
    for (size_t i = 0; i < object->as.object.count; i++)
        if (strcmp(object->as.object.members[i].key, key) == 0) return object->as.object.members[i].value;
    return NULL;
}

int mf_value_array_append(mf_value *array, mf_value *value) {
    if (!array || array->kind != MF_VALUE_ARRAY) { mf_value_free(value); return -1; }
    mf_value **grown = realloc(array->as.array.items, (array->as.array.count + 1) * sizeof *grown);
    if (!grown) { mf_value_free(value); return -1; }
    array->as.array.items = grown;
    grown[array->as.array.count++] = value;
    return 0;
}

bool mf_value_equal(const mf_value *a, const mf_value *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case MF_VALUE_NULL: return true;
    case MF_VALUE_STRING: return strcmp(a->as.string, b->as.string) == 0;
    case MF_VALUE_BOOLEAN: return a->as.boolean == b->as.boolean;
    case MF_VALUE_INTEGER: return a->as.integer == b->as.integer;
    case MF_VALUE_NUMBER: return a->as.number == b->as.number;
    case MF_VALUE_DATA:
        return a->as.data.length == b->as.data.length &&
               (a->as.data.length == 0 || memcmp(a->as.data.bytes, b->as.data.bytes, a->as.data.length) == 0);
    case MF_VALUE_ARRAY:
        if (a->as.array.count != b->as.array.count) return false;
        for (size_t i = 0; i < a->as.array.count; i++)
            if (!mf_value_equal(a->as.array.items[i], b->as.array.items[i])) return false;
        return true;
    case MF_VALUE_OBJECT:
        if (a->as.object.count != b->as.object.count) return false;
        for (size_t i = 0; i < a->as.object.count; i++) {
            const mf_value *other = mf_value_object_get(b, a->as.object.members[i].key);
            if (!other || !mf_value_equal(a->as.object.members[i].value, other)) return false;
        }
        return true;
    }
    return false;
}

void mf_value_free(mf_value *v) {
    if (!v) return;
    switch (v->kind) {
    case MF_VALUE_STRING: free(v->as.string); break;
    case MF_VALUE_DATA: free(v->as.data.bytes); break;
    case MF_VALUE_ARRAY:
        for (size_t i = 0; i < v->as.array.count; i++) mf_value_free(v->as.array.items[i]);
        free(v->as.array.items);
        break;
    case MF_VALUE_OBJECT:
        for (size_t i = 0; i < v->as.object.count; i++) {
            free(v->as.object.members[i].key);
            mf_value_free(v->as.object.members[i].value);
        }
        free(v->as.object.members);
        break;
    default: break;
    }
    free(v);
}

const char *mf_privacy_name(mf_privacy privacy) {
    static const char *const names[] = { "publicDefinition", "private", "sensitive", "secret" };
    return (unsigned)privacy < sizeof names / sizeof names[0] ? names[privacy] : NULL;
}
