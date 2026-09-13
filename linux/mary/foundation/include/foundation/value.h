/* MaryFoundation/Core/MaryValue.swift in C: the JSON-shaped value every schema
 * carries — null, string, boolean, integer, number, object, array or raw data.
 * A tree of heap nodes; mf_value_free releases a node and everything under it.
 * Codable (the `data` case's encoding) is not ported yet. */
#ifndef MARY_FOUNDATION_VALUE_H
#define MARY_FOUNDATION_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum mf_value_kind {
    MF_VALUE_NULL,
    MF_VALUE_STRING,
    MF_VALUE_BOOLEAN,
    MF_VALUE_INTEGER,       /* Int64 */
    MF_VALUE_NUMBER,        /* Double */
    MF_VALUE_OBJECT,        /* [String: MaryValue] */
    MF_VALUE_ARRAY,
    MF_VALUE_DATA,
} mf_value_kind;

typedef struct mf_value mf_value;

typedef struct mf_member {
    char *key;
    mf_value *value;
} mf_member;

struct mf_value {
    mf_value_kind kind;
    union {
        char *string;
        bool boolean;
        int64_t integer;
        double number;
        struct { mf_member *members; size_t count; } object;   /* insertion order; keys unique */
        struct { mf_value **items; size_t count; } array;
        struct { unsigned char *bytes; size_t length; } data;
    } as;
};

/* Constructors return NULL only when out of memory. */
mf_value *mf_value_null(void);
mf_value *mf_value_string(const char *s);
mf_value *mf_value_boolean(bool b);
mf_value *mf_value_integer(int64_t i);
mf_value *mf_value_number(double n);
mf_value *mf_value_object(void);
mf_value *mf_value_array(void);
mf_value *mf_value_data(const unsigned char *bytes, size_t length);

/* Takes ownership of `value` (also on failure). Setting an existing key replaces
 * it, as assigning into a Swift dictionary does. 0, or -1 when out of memory. */
int mf_value_object_set(mf_value *object, const char *key, mf_value *value);
const mf_value *mf_value_object_get(const mf_value *object, const char *key);
/* Takes ownership of `value` (also on failure). 0, or -1. */
int mf_value_array_append(mf_value *array, mf_value *value);

/* Hashable: deep equality, with objects compared as dictionaries (order-free). */
bool mf_value_equal(const mf_value *a, const mf_value *b);
void mf_value_free(mf_value *v);

#endif
