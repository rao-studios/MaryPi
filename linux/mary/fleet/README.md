# fleet — Swift twin: [Fleet](https://github.com/rao-studios/Fleet)

LoRA-gated JSON state machines: adapters trained on small on-device models so that output matches a
fixed schema, with the schema enforced while decoding — structure *forced*, values *masked*. On MaryOS
this arrives with Frigate's on-device models, after the Mistral-backed milestone.

Status: **skeleton**. `fleet_lora_entry` declares `LoRAEntry` (FleetStore/FleetRegistry.swift) with its
named-slot rule; `gate.h` declares `GateDecision`, `TokenVocabulary` and `JSONGate`
(FleetCore/Gate/). `fleet_gate_new` answers `ENOSYS` until the automaton is ported. Prefix `fleet_`.
