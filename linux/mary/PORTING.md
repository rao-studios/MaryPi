# Porting Mary to C

Each row maps a Swift source to its C twin in this directory. Swift paths are relative to the sibling
checkouts under `~/Documents/rao/repositories/` (`Mary/Sources/…`, `Sewn/Sources/…`, and so on).

**Status:** *working* — ported and tested; *skeleton* — the types are declared and a smoke test
compiles, behaviour is not ported; *planned* — the package exists only as a README so far;
*deviation* — MaryOS does this differently on purpose (numbered below).

## foundation ← MaryFoundation

| Swift | C | Status |
|---|---|---|
| `MaryFoundation/Core/MaryValue.swift` | `foundation/include/foundation/value.h`, `src/value.c` | skeleton (build, compare, free; no Codable) |
| `MaryFoundation/Core/ValueEnvelope.swift` | `foundation/include/foundation/envelope.h` | skeleton |
| `MaryFoundation/Core/SourceScope.swift` | `mf_source_scope` in `envelope.h` | skeleton |
| `MaryFoundation/Core/ValueSchemas.swift` (DataPrivacyClass) | `mf_privacy`, `mf_privacy_name` | skeleton |
| `MaryFoundation/Ability/SkillSchemas.swift` (SkillSchema) | `skills/` and MaryUI's `lp_skill` | planned — deviation 10 |
| `MaryFoundation/Package/*` (`.mary` codec, digest) | — | not ported — deviation 10 |
| `MaryFoundation/Core/AXFrame.swift` | — | not ported — deviation 9 |

## ambient ← MaryAmbient

| Swift | C | Status |
|---|---|---|
| `MaryAmbient/Ambient/Engine/AmbientIntent.swift` (AmbientIntent, AmbientSignal) | `ambient/include/ambient/intent.h`, `src/intent.c` | skeleton (names, isActing, touchesExistingProse) |
| `MaryAmbient/Reference/WorkspaceFocusTracker*.swift` | `ma_focus` (desktop-reported) | skeleton — deviation 9 |
| `MaryAmbient/Selection/AX*.swift`, `SelectionHandoffCoordinator*.swift` | — | not ported — deviation 9 |

## fleet ← Fleet

| Swift | C | Status |
|---|---|---|
| `Fleet/Sources/FleetStore/FleetRegistry.swift` (LoRAEntry) | `fleet/include/fleet/lora.h`, `src/fleet.c` | skeleton |
| `Fleet/Sources/FleetCore/Gate/JSONGate.swift` (GateDecision, JSONGate) | `fleet/include/fleet/gate.h` | skeleton (answers ENOSYS) |
| `Fleet/Sources/FleetCore/Gate/TokenVocabulary.swift` | `fleet_vocabulary` | skeleton |
| `Fleet/Sources/FleetCore/Gate/SchemaAutomaton.swift`, `TokenTrie.swift` | — | planned |

## frigate ← Frigate

| Swift | C | Status |
|---|---|---|
| `Frigate/Sources/Frigate/FrigateEmbedder.swift` | `frigate/include/frigate/frigate.h` (`frigate_embedder`) | skeleton (answers ENOSYS) |
| `Frigate/Sources/Frigate/FrigateLLM.swift` | `frigate_llm`, `frigate_llm_generate` | skeleton (answers ENOSYS) |
| `Frigate/Sources/Frigate/FrigateBoost.swift` | `frigate_boost` | skeleton (answers ENOSYS) |

## common — no Swift twin

What Swift gets from Foundation, URLSession and swift-nio.

| Swift | C | Status |
|---|---|---|
| `Sewn/Sources/Providers/ModelProvider+Stream.swift` (`sseStream`) | `common/include/common/sse.h`, `src/sse.c` | working — incremental; Sewn buffers the whole body on Linux |
| `Mary/Sources/MaryBrain/Sewn/SewnRealtimeWire.swift` (frame handling) | `common/include/common/frame.h`, `src/frame.c` | working — length-prefixed frames on a unix socket replace WebSocket frames |
| Foundation `JSONSerialization` / `Codable` | `common/include/common/json.h` (json-c) | working |
| Foundation `Data(base64Encoded:)` | `common/include/common/base64.h` | working |
| — | `common/include/common/{buf,lines,secure,io,log}.h` | working |

## conduit ← Conduit

| Swift | C | Status |
|---|---|---|
| `Conduit/Protos/thread.proto`, `fleet.proto` | `conduit/protos/` (verbatim) | planned |
| `Conduit/Sources/Conduit/Generated/*` | `conduit/src/pb/*.pb-c.[ch]` (protobuf-c) | planned |
| `Conduit/Sources/Conduit/Client/*`, `Server/*` | `conduit_grpc_call`, `conduit_grpc_server` (unary, nghttp2) | planned |

## sewn ← Sewn

| Swift | C | Status |
|---|---|---|
| `Sewn/Sources/Utilities/StreamingSentenceChunker.swift` (StreamingSentenceChunker, TTSTextSanitizer) | `sewn/include/sewn/chunker.h`, `src/chunker.c` | working |
| `Sewn/Sources/Utilities/SentenceBoundary.swift` (`upToFirstSentence`) | `sewn_sentence_prefix_len` | working — code points stand in for Characters |
| `Sewn/Sources/Providers/ModelProvider+Stream.swift` (`runStreamMistral` body, `handle(payload:)`) | `sewn/include/sewn/mistral.h`, `src/mistral.c` (`sewn_chat_body`, `sewn_chat_event_parse`) | working |
| `Sewn/Sources/Utilities/MistralTTSStream.swift` (`makeRequest`, `extractPCM`) | `sewn_speech_body`, `sewn_speech_event` | working |
| — (Voxtral Realtime transcription, from mistralai client-python) | `sewn_stt_session_update`, `sewn_stt_append`, `sewn_stt_event_parse` | working — deviation 2 |
| `Sewn/Sources/API/Routes/Realtime/RealtimeWire.swift`, `Realtime.swift` | `sewn/src/realtime.c` | planned — deviation 5 |
| `Sewn/Sources/API/Middleware/AuthMiddleware.swift`, `TokenValidator.swift` | `sewn/src/peer.c` | planned — deviation 4 |

## thread ← Thread

| Swift | C | Status |
|---|---|---|
| `Thread/Sources/Conduit/ThreadGRPCServer.swift` | `thread/bin/threadd.c` | planned — deviation 7 |
| `Thread/Sources/Utilities/Persistence/FilePersistence.swift` | `thread/src/store.c` | planned — deviation 7 |
| — (peering) | `thread/include/thread/peer.h` | planned (answers ENOSYS) |

## mary-thread ← MaryThread

| Swift | C | Status |
|---|---|---|
| `MaryThread/ThreadDirectClient.swift` | `mary-thread/src/client.c` | planned |

## skills ← MaryPlugin, SkillSchema

| Swift | C | Status |
|---|---|---|
| `MaryPlugin/MaryAdapter.swift` | `skills/` registry + MaryUI `lp_app.skills` | planned — deviation 10 |
| `MaryBrain/Brain/AbilityDispatching.swift` | `computer-use`'s `mcu_invoke` | planned — deviation 10 |

## computer-use ← MaryComputerUse

| Swift | C | Status |
|---|---|---|
| `MaryComputerUse/Hands/*`, `Sight/*`, `Accessibility/*` | `mcu_invoke` over the desktop socket | planned — deviation 9 |

## brain ← MaryBrain

| Swift | C | Status |
|---|---|---|
| `MaryBrain/Prompt/PromptCatalog+Voice.swift` (sewnPreamble, sewnCompany, sewnPersonaConverse) | `brain/src/prompt.c` | planned — deviation 8 |
| `MaryBrain/Brain/MaryBrain+History.swift` (spokenMessages) | `brain/src/history.c` | planned |
| `MaryBrain/Sewn/SewnWire.swift` (ChatRequest, Persona) | `brain/src/request.c` | planned |

## voice ← MaryVoice

| Swift | C | Status |
|---|---|---|
| `MaryVoice/VAD/EnergyVAD.swift` | `voice/src/vad.c` | planned |
| `MaryVoice/VAD/EndpointHold.swift` | `voice/src/endpoint.c` | planned |
| `MaryVoice/VAD/WakePlanner.swift` | `voice/src/wake_planner.c` | planned |
| `MaryVoice/Wake/WakeWordListener.swift` | `voice/src/kws.c` (sherpa-onnx) | planned — deviation 1 |
| `MaryVoice/Capture/MicCapture.swift`, `TTS/Shared/KokoroStreamSpeaker.swift` (playback) | `voice/src/pipewire.c` | planned |
| `MaryVoice/VAD/BargeInGovernor.swift` | `voice/include/voice/barge_in.h` | planned — deviation 6 |

## runtime ← MaryRuntime

| Swift | C | Status |
|---|---|---|
| `MaryRuntime/Runtime/MaryRuntime*.swift`, `Services/Voice/*`, `Services/Chat/TextTurnRunner.swift` | `runtime/bin/maryd.c` | planned |
| `MaryRuntime/Services/Servers/ServerSpec.swift` | systemd units in `distro/` | deviation — its hardcoded credentials are never copied |

## Deviations

1. **Wake word.** On-device keyword spotting (sherpa-onnx) gates the microphone. Swift runs energy VAD and
   a full transcription on every utterance and matches the transcript; streaming standby audio off the
   machine is not acceptable on MaryOS. `WakePlanner` is still ported, to take the request out of the
   transcript.
2. **Speech-to-text.** Voxtral Realtime through `sewnd`, instead of `SFSpeechRecognizer` /
   `SpeechAnalyzer`.
3. **Text-to-speech.** Voxtral TTS only. The Kokoro fallback waits for Frigate on Linux.
4. **Sewn authentication.** Callers are authorized by unix-socket peer credentials; there is no Supabase
   sign-in. The owner of a turn is the local user.
5. **Sewn's turn.** One grounded pass. No fast opener, retrieval, Sinatra tuning or Gita accounting.
6. **Barge-in.** No interrupting by voice while Mary speaks — there is no echo cancellation yet. Esc or the
   Ask Mary button stops her.
7. **Thread.** Documents are JSON files; no embeddings, product quantization or knowledge graph yet;
   `owner_id` comes from the caller's credentials; a unix socket replaces TCP port 9090.
8. **Persona.** Mary's persona says she lives on the user's Mac; on MaryOS it is reworded.
9. **Computer use.** No accessibility tree, screen capture or synthetic input. MaryOS's applications are its
   own, so Mary reads their state and acts through `lp_app.perform` over the desktop socket.
10. **Abilities.** The `.mary` packages under `Mary/Abilities/` — recipes recorded against a live
    accessibility tree — are not ported. Applications declare their skills in code (`lp_skill`), and
    System Settings holds the policy per application: whether Mary may use it, which skills, and when she
    must ask first. `SkillSchema` maps to `lp_skill`; `AbilityDispatching.dispatch` maps to `mcu_invoke`.
