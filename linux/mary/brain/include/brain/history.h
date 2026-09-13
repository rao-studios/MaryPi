/* The conversation a turn carries (MaryBrain+History.swift): user turns, Mary's
 * spoken replies, and Skill results, which stay local. appendHistory trims whole
 * exchanges from the front until the spoken count fits historyMessageLimit (12),
 * keeping a single giant exchange rather than splitting it. */
#ifndef MARY_BRAIN_HISTORY_H
#define MARY_BRAIN_HISTORY_H

#include <stddef.h>

#define MB_HISTORY_LIMIT 12

typedef enum mb_role {
    MB_ROLE_USER,
    MB_ROLE_ASSISTANT,
    MB_ROLE_SKILL_RESULT,
} mb_role;

typedef struct mb_turn {
    mb_role role;
    char *text;
} mb_turn;

typedef struct mb_history {
    mb_turn *turns;
    size_t count, cap;
    int limit;          /* historyMessageLimit; never below 4 */
} mb_history;

void mb_history_init(mb_history *h);
/* Appends and trims. 0, or -ENOMEM. */
int mb_history_append(mb_history *h, mb_role role, const char *text);
/* User turns plus non-empty assistant turns. */
size_t mb_history_spoken_count(const mb_history *h);
/* spokenMessages(): [{role, content}] for the wire. A new reference. */
struct json_object *mb_history_spoken_messages(const mb_history *h);
void mb_history_clear(mb_history *h);
void mb_history_free(mb_history *h);

#endif
