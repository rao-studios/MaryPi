# foundation — Swift twin: `Mary/Sources/MaryFoundation`

The schema layer: the JSON-shaped `MaryValue` every schema carries, and the envelope a value travels in
(`ValueEnvelope`, `SourceScope`, `DataPrivacyClass`). Swift's foundation also holds the ability-package
grammar; on MaryOS skills are declared by the apps themselves, so that part lives in `skills/` and the
`.mary` codec is not ported (PORTING.md 10).

`foundation/behavior.h` is `BehavioralEpisode`, `BehavioralActionRecord` and `BehavioralCodec`: one turn's
episode (query, ambient capture, prior episode, the actions with their dispositions, the ability targets,
the seal that the first reason wins), encoded as one JSON line with sorted keys and ISO-8601 dates in UTC
with milliseconds, so a MaryOS record reads like a Mac one. `mf_uuid_v4`, `mf_iso8601` and
`mf_canonical_json` (sorted keys, for the repeat guard) live there too.

Status: `mf_value` is built, compared and freed; the envelope is declared only; the behaviour codec is
working. Codable (MaryValue's `data` encoding, the integrity digest) is not ported. Prefix `mf_`.
