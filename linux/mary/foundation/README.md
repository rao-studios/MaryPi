# foundation — Swift twin: `Mary/Sources/MaryFoundation`

The schema layer: the JSON-shaped `MaryValue` every schema carries, and the envelope a value travels in
(`ValueEnvelope`, `SourceScope`, `DataPrivacyClass`). Swift's foundation also holds the ability-package
grammar; on MaryOS skills are declared by the apps themselves, so that part lives in `skills/` and the
`.mary` codec is not ported (PORTING.md 10).

Status: **skeleton**. `mf_value` is built, compared and freed; the envelope is declared only. Codable
(MaryValue's `data` encoding, the integrity digest) is not ported. Prefix `mf_`.
