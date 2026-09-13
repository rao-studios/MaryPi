/* The ambient layer on the wire: what the desktop publishes to maryd (PARITY D28) and what
 * maryd shows the Ambient app (PARITY D29), as json-c objects.
 *
 *   desktop → maryd   world{places: [{place, capturedAt, surface: {application{name, id, pid}, activeWindow{title},
 *                     windowCount, minimizedCount, elements[{identity, ordinal, role, kind, label, focused, enabled}],
 *                     focused{…}, pageNotYetRead, document{name, path, text, total, lower, upper}}}], focus, windows[…]}
 *                     selection{place, applicationID, text, surrounding, subject, document, lower, upper, total, editable, capturedAt}
 *                     selection.clear{applicationID}
 *   maryd → desktop   ambient{places[{place, name, order, focus, surfaceLine, age, facts[mention lines], factCount}],
 *                     lead, selection?, world?}, trace{records[{id, date, utterance, route{…}, …}]}
 *
 * Times on the wire are milliseconds since the epoch. */
#ifndef MARY_AMBIENT_WIRE_H
#define MARY_AMBIENT_WIRE_H

#include "ambient/engine.h"
#include "ambient/store.h"
#include "ambient/trace.h"

struct json_object;

/* A published surface. `now` stamps a capturedAt that is missing. 0, or -EINVAL. */
int ma_surface_parse(struct json_object *entry, double now, ma_surface *out);
/* A document fact from a surface's `document` (the text window and its bounds), or 0 when it has none. Returns 1 when filled. */
int ma_surface_document_fact(struct json_object *entry, const ma_surface *surface, ma_fact *out);
int ma_selection_parse(struct json_object *msg, double now, ma_selection *out);
struct json_object *ma_place_json(const ma_place *p, const ma_roster *r);
struct json_object *ma_surface_json(const ma_surface *s, double now, const ma_roster *r);
struct json_object *ma_fact_json(const ma_fact *f, double now, const ma_roster *r);
struct json_object *ma_world_json(const ma_world *w, double now);
struct json_object *ma_rendering_json(const ma_rendering *rendering);
struct json_object *ma_route_json(const ma_route *route, const ma_roster *r);
struct json_object *ma_trace_record_json(const ma_trace_record *record, const ma_roster *r, double now);
struct json_object *ma_trace_json(const ma_trace_log *log, const ma_roster *r, double now);
/* The Ambient app's World and Realms tabs: every fresh surface with its facts, the lead, the selection. */
struct json_object *ma_ambient_state_json(ma_store *store, const ma_roster *r, const ma_focus_signal *focus, double now);

#endif
