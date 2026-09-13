#include "brain/scope.h"

#include <stdio.h>
#include <string.h>

#include "common/json.h"
#include "skills/registry.h"

mb_recall mb_recall_default(void) { return (mb_recall){ true, true, true, true }; }

static bool allowed(const mb_recall *recall, const char *lane) {
    if (strcmp(lane, "personal") == 0) return recall->personal;
    if (strcmp(lane, "conversation") == 0) return recall->conversation;
    if (strcmp(lane, "application") == 0) return recall->application;
    if (strcmp(lane, "behavioral") == 0) return recall->behavioral;
    return false;
}

static void add_lane(mb_purpose *p, const mb_recall *recall, const char *lane) {
    if (!allowed(recall, lane)) return;
    for (int i = 0; i < p->lane_count; i++) if (strcmp(p->lanes[i], lane) == 0) return;
    if (p->lane_count < MB_SCOPE_LANES) p->lanes[p->lane_count++] = lane;
}

static void add_group(mb_memory_plan *plan, const char *group) {
    for (int i = 0; i < plan->group_count; i++) if (strcmp(plan->groups[i], group) == 0) return;
    if (plan->group_count < MB_SCOPE_GROUPS) snprintf(plan->groups[plan->group_count++], 80, "%s", group);
}

void mb_memory_plan_for(const ma_route *route, const mb_recall *recall, const char *owner, mb_memory_plan *out) {
    memset(out, 0, sizeof *out);
    mb_recall all = mb_recall_default();
    if (!recall) recall = &all;
    /* routing: what the applications offer, and how the person asks */
    add_lane(&out->routing, recall, "application");
    add_lane(&out->routing, recall, "behavioral");
    /* orchestration: what Mary did before, and what the applications offer */
    add_lane(&out->orchestration, recall, "behavioral");
    add_lane(&out->orchestration, recall, "application");
    /* the reply's context: the gate's threads over the storage lanes, in its priority */
    const ma_memory_plan *gate = route ? &route->gate.memory : NULL;
    unsigned lanes = gate ? gate->lanes : MA_LANE_PERSONAL;
    ma_lane order[2] = { MA_LANE_PERSONAL, MA_LANE_ABILITY };
    int order_count = 2;
    if (gate && gate->priority_count) {
        order_count = 0;
        for (int i = 0; i < gate->priority_count; i++) order[order_count++] = gate->lane_priority[i];
        for (int l = 0; l < 2; l++) {
            ma_lane lane = l == 0 ? MA_LANE_PERSONAL : MA_LANE_ABILITY;
            bool present = false;
            for (int i = 0; i < order_count; i++) present |= order[i] == lane;
            if (!present && order_count < 2) order[order_count++] = lane;
        }
    }
    for (int i = 0; i < order_count; i++) {
        if (!(lanes & order[i])) continue;
        const char *storage[2];
        int n = ma_lane_storage_lanes(order[i], storage, 2);
        for (int k = 0; k < n; k++) add_lane(&out->context, recall, storage[k]);
    }
    if (route) {
        switch (route->intent) {
        case MA_INTENT_PERCEIVE: case MA_INTENT_OPERATE: case MA_INTENT_COMPOSE: case MA_INTENT_REVISE:
            add_lane(&out->context, recall, "application");
            break;
        default: break;
        }
    }
    out->priority_count = 0;
    for (int i = 0; i < order_count; i++) if (lanes & order[i]) out->lane_priority[out->priority_count++] = ma_lane_name(order[i]);
    /* the groups: the owner's conversation and memory, the files, and the targets' ability groups */
    char group[80];
    snprintf(group, sizeof group, "conversation-%s", owner ? owner : "");
    if (allowed(recall, "conversation")) add_group(out, group);
    snprintf(group, sizeof group, "memory-%s", owner ? owner : "");
    if (allowed(recall, "personal")) add_group(out, group);
    snprintf(group, sizeof group, "files-%s", owner ? owner : "");
    if (allowed(recall, "personal")) add_group(out, group);
    if (gate && allowed(recall, "application")) {
        for (int i = 0; i < gate->target_count; i++) {
            sk_ability_group(owner ? owner : "", gate->targets[i].ability_id, ma_paradigm_name(gate->targets[i].paradigm), group, sizeof group);
            add_group(out, group);
        }
        for (int i = 0; i < route->gate.application_count; i++) {
            sk_ability_group(owner ? owner : "", route->gate.applications[i], "applicationExpertise", group, sizeof group);
            add_group(out, group);
        }
    }
    for (int i = 0; gate && i < gate->hint_count && out->entity_count < MB_SCOPE_ENTITIES; i++) out->entities[out->entity_count++] = gate->relationship_hints[i];
}

struct json_object *mb_purpose_json(const mb_purpose *p) {
    struct json_object *a = json_object_new_array();
    for (int i = 0; i < p->lane_count; i++) json_object_array_add(a, json_object_new_string(p->lanes[i]));
    return a;
}
