#include "brain/history.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"

void mb_history_init(mb_history *h) {
    memset(h, 0, sizeof *h);
    h->limit = MB_HISTORY_LIMIT;
}

static bool spoken(const mb_turn *t) {
    return t->role == MB_ROLE_USER || (t->role == MB_ROLE_ASSISTANT && t->text && *t->text);
}

size_t mb_history_spoken_count(const mb_history *h) {
    size_t n = 0;
    for (size_t i = 0; i < h->count; i++) n += spoken(&h->turns[i]);
    return n;
}

static void drop_front(mb_history *h, size_t n) {
    for (size_t i = 0; i < n; i++) free(h->turns[i].text);
    memmove(h->turns, h->turns + n, (h->count - n) * sizeof *h->turns);
    h->count -= n;
}

/* trimHistory(). */
static void trim(mb_history *h) {
    size_t limit = (size_t)(h->limit < 4 ? 4 : h->limit);
    while (mb_history_spoken_count(h) > limit) {
        size_t first_user = h->count;
        for (size_t i = 0; i < h->count; i++) {
            if (h->turns[i].role == MB_ROLE_USER) {
                first_user = i;
                break;
            }
        }
        if (first_user == h->count) break;
        size_t next_user = h->count;
        for (size_t i = first_user + 1; i < h->count; i++) {
            if (h->turns[i].role == MB_ROLE_USER) {
                next_user = i;
                break;
            }
        }
        if (next_user == h->count) break;   /* one giant exchange — keep it */
        drop_front(h, next_user);
    }
}

int mb_history_append(mb_history *h, mb_role role, const char *text) {
    if (h->count == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 16;
        mb_turn *grown = realloc(h->turns, cap * sizeof *grown);
        if (!grown) return -ENOMEM;
        h->turns = grown;
        h->cap = cap;
    }
    char *copy = strdup(text ? text : "");
    if (!copy) return -ENOMEM;
    h->turns[h->count++] = (mb_turn){ role, copy };
    trim(h);
    return 0;
}

struct json_object *mb_history_spoken_messages(const mb_history *h) {
    struct json_object *messages = json_object_new_array();
    for (size_t i = 0; i < h->count; i++) {
        if (!spoken(&h->turns[i])) continue;
        struct json_object *m = json_object_new_object();
        json_object_object_add(m, "role", json_object_new_string(h->turns[i].role == MB_ROLE_USER ? "user" : "assistant"));
        json_object_object_add(m, "content", json_object_new_string(h->turns[i].text));
        json_object_array_add(messages, m);
    }
    return messages;
}

void mb_history_clear(mb_history *h) { drop_front(h, h->count); }

void mb_history_free(mb_history *h) {
    mb_history_clear(h);
    free(h->turns);
    memset(h, 0, sizeof *h);
}
