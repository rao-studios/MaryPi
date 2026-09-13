# ambient — Swift twin: `Mary/Sources/MaryAmbient`

What a turn is for, and what Mary already knows is in front of the person. Swift builds that knowledge
from the accessibility tree in tiers; on MaryOS the desktop publishes it (PORTING.md deviations 9 and 13):
each application's own surface, the document it shows, the selection.

- `intent.h` — the nine shapes of a turn and the signals that decide them (the Swift raw values).
- `place.h` — where a fact lives: one of Mary's lanes, or an application on the `applications` lane; the
  roster of applications and abilities (`ma_roster_maryos`, renamed by the desktop's `skills{apps}`).
- `store.h` — short-term awareness in tiers: surfaces (drop at 25 s), facts keyed (place, slot) with fresh
  and retain windows, the 30 s selection packet, the world snapshot, the lead.
- `classify.h` — the structural classifiers: the edit intent (replace / insert / delete / move), the named
  part, the question forms, a bare yes or no.
- `ranker.h` — relevance, the three-way rule over places, deixis and anaphora, rendering under a budget
  (three blocks in full, the rest as mentions, never silence), the merged-worlds lines.
- `render.h` — one phrasing for the prompt and the pane: ages, the surface line, mention lines, blocks.
- `realm.h` — the focus ledger and its signal; need → candidates → place.
- `engine.h` — the route: the classify ladder, the intent gate's memory plan (the `ability` and
  `personal` threads over threadd's four lanes), the realm, the needs, the ranking mode.
- `prompt.h` — the ambient section of the voice's instructions (`sewnLiveWork`) and the capability line.
- `trace.h` — a ring of fifty resolved turns with their skill runs and what retrieval returned; the
  Mac's RouteReport as text.
- `wire.h` — the JSON that crosses maryd's socket: `world`, `selection`, `ambient`, `trace`.

Prefix `ma_`. Pure C11 over json-c; times are seconds so a test can state a clock. `make test` runs the
eight suites; their case names follow the Swift tests where one exists.
