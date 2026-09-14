/* The memory plan of a turn (AmbientIntentGate's ThreadMemoryPlan and ThreadMemoryTopology's
 * scopes) applied to threadd's lanes: which lanes each purpose may retrieve from, the
 * groups that narrow it, and the relationship cues to search with. Settings › Mary ›
 * Recall gates the lanes globally; the route's intent widens and narrows them. */
#ifndef MARY_BRAIN_SCOPE_H
#define MARY_BRAIN_SCOPE_H

#include <stdbool.h>

#include "ambient/engine.h"

#define MB_SCOPE_LANES 2
#define MB_SCOPE_GROUPS 12
#define MB_SCOPE_ENTITIES MA_HINTS_MAX

typedef struct mb_recall {
    bool personal, behavioral;      /* Settings › Mary › Recall: the Thread's two storage lanes; both on by default */
} mb_recall;

mb_recall mb_recall_default(void);

typedef struct mb_purpose {
    const char *lanes[MB_SCOPE_LANES];
    int lane_count;
} mb_purpose;

typedef struct mb_memory_plan {
    mb_purpose routing;             /* triage: behavioral (the routing habits; the skills themselves are the registry's) */
    mb_purpose orchestration;       /* Lane B's background tier: behavioral */
    mb_purpose context;             /* the reply's context: the gate's threads over the storage lanes */
    char groups[MB_SCOPE_GROUPS][80];
    int group_count;
    const char *entities[MB_SCOPE_ENTITIES];
    int entity_count;
    const char *lane_priority[2];
    int priority_count;
} mb_memory_plan;

/* The plan for a route: the gate's threads mapped to the storage lanes (personal ↔ personal, ability ↔
 * behavioral), the Recall toggles removing lanes, `behavioral` added on perceive/operate/compose/revise,
 * the owner's memory, files and style groups, and the behaviour groups of the gate's targets
 * (sk_ability_group). */
void mb_memory_plan_for(const ma_route *route, const mb_recall *recall, const char *owner, mb_memory_plan *out);
/* A purpose's lanes as a JSON array. */
struct json_object *mb_purpose_json(const mb_purpose *p);

#endif
