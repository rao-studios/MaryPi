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
| `Conduit/Protos/thread.proto`, `fleet.proto` | `conduit/protos/` (verbatim; `make gen-check` compares) | working |
| `Conduit/Sources/Conduit/Generated/*` (swift-protobuf) | `conduit/include/conduit/*.pb-c.h`, `conduit/src/*.pb-c.c` (protobuf-c) | working |
| grpc-swift's HTTP/2 transport (`GRPCNIOTransportHTTP2`) | `conduit/include/conduit/grpc.h`, `src/grpc.c` (`conduit_serve`, `conduit_call`; nghttp2) | working — unary only, unix sockets |
| `Conduit/Sources/Conduit/Client/*`, `Server/*`, `Session/*` (registration, sessions, the mothership) | — | not ported: nodes do not register with Sewn on MaryOS |

## sewn ← Sewn

| Swift | C | Status |
|---|---|---|
| `Sewn/Sources/Utilities/StreamingSentenceChunker.swift` (StreamingSentenceChunker, TTSTextSanitizer) | `sewn/include/sewn/chunker.h`, `src/chunker.c` | working |
| `Sewn/Sources/Utilities/SentenceBoundary.swift` (`upToFirstSentence`) | `sewn_sentence_prefix_len` | working — code points stand in for Characters |
| `Sewn/Sources/Providers/ModelProvider+Stream.swift` (`runStreamMistral` body, `handle(payload:)`) | `sewn/include/sewn/mistral.h`, `src/mistral.c` (`sewn_chat_body`, `sewn_chat_event_parse`) | working |
| `Sewn/Sources/Utilities/MistralTTSStream.swift` (`makeRequest`, `extractPCM`) | `sewn_speech_body`, `sewn_speech_event` | working |
| — (Voxtral Realtime transcription, from mistralai client-python) | `sewn_stt_session_update`, `sewn_stt_append`, `sewn_stt_event_parse` | working — deviation 2 |
| `Mary/Sources/MaryVoice/STT/VoiceTranscriber.swift` (the seam Mary transcribes through) | `sewn/include/sewn/transcribe.h`, `src/transcribe.c` (`transcribe.*` on sewnd's socket) | working — deviation 2 |
| — (a WebSocket client) | `sewn/include/sewn/ws.h`, `src/ws.c` (libwebsockets) | working — Linux builds only; the relay is tested through `sewn_ws_ops` |
| `Sewn/Sources/API/Routes/Realtime/RealtimeWire.swift`, `Realtime.swift`, `RealtimeTurnEngine.swift` | `sewn/include/sewn/turn.h`, `src/turn.c` (`sewn_run_turn`) | working — deviation 5 |
| `Sewn/Sources/Core/Personality.swift` (`chatPersonaSection`), `Core/Sewn.swift` (`handleChat` prompt), `Core/Commands/Sewn+Compact.swift` (`memoryInstruction`) | `sewn_turn_system_prompt`, `sewn_turn_messages` | working |
| `Sewn/Sources/Core/ModelConfig.swift` (Mistral model names), `API/GenerationDefaults.swift` | `sewn_turn_request_parse` | working |
| `Sewn/Sources/Providers/ModelProvider+Stream.swift` (`sseStream`), `Utilities/MistralTTSStream.swift` (`stream`) | `sewn/include/sewn/transport.h`, `sewn_http_post_stream` | working — streams on Linux too |
| `Sewn/Sources/API/Middleware/AuthMiddleware.swift`, `TokenValidator.swift` | `sewn/include/sewn/peer.h`, `src/peer.c` | working — deviation 4 |
| `Sewn/Sources/API/Network/NetworkService+Center.swift` (`MISTRAL_API_KEY`) | `sewn/include/sewn/key.h`, `src/key.c` | working — a 0600 file set from System Settings, never the environment |
| `Sewn/Sources/SewnServer.swift` (the listener) | `sewn/bin/sewnd.c`, `sewn/include/sewn/server.h`, `src/server.c` | working for `key.status`, `key.set`, `key.verify`; turns and transcription planned |
| — (checking a key) | `sewn/include/sewn/http.h`, `src/http.c` (`sewn_mistral_verify`: GET `/v1/models`) | working |
| — | `sewn/bin/sewnctl.c`, `sewn/include/sewn/client.h`, `src/client.c` | working |

## thread ← Thread

| Swift | C | Status |
|---|---|---|
| `Thread/Sources/Conduit/ThreadQueryServiceImpl.swift` (`index`) | `thread/include/thread/store.h` (`thread_store_index`), `src/service.c` | working — deviation 7 |
| `Thread/Sources/Conduit/ThreadLibraryServiceImpl.swift` (`library`, `documents`) | `thread_store_library`, `thread_store_documents` | working — deviation 7 |
| `Thread/Sources/Conduit/ThreadGRPCServer.swift`, `Sources/ThreadServer.swift` | `thread/bin/threadd.c` | working — a unix socket, owner from credentials |
| `Thread/Sources/Utilities/Persistence/FilePersistence.swift`, `NodeIdentity.swift` | `thread/src/store.c` (JSON files, `node-id`) | working — deviation 7 |
| `Thread/Sources/Conduit/ThreadQueryServiceImpl.swift` (`search`, `remove`), `ThreadUpdateServiceImpl.swift`, `ThreadGraphServiceImpl.swift`, `Sources/Database/*` (embeddings, PQ, graph) | — | planned: answer UNIMPLEMENTED |
| — (peering between MaryOS machines) | `thread/include/thread/peer.h` | declared — answers ENOSYS |
| — | `thread/bin/threadctl.c` | working |

## mary-thread ← MaryThread

| Swift | C | Status |
|---|---|---|
| `MaryThread/ThreadDirectClient.swift` (`deposit`, `library`, `documents`) | `mary-thread/include/mary-thread/client.h`, `src/client.c` | working — search, removal and the graph are not ported |

## skills ← MaryPlugin, SkillSchema

| Swift | C | Status |
|---|---|---|
| `MaryFoundation/Ability/SkillSchemas.swift` (`SkillSchema`), `MaryPlugin/MaryAdapter.swift` (`skillBindings`) | `skills/include/skills/registry.h`, `src/registry.c` | working — deviation 10 |
| `MaryFoundation/Ability/SkillSchemas.swift` (`ModelExposureSchema`) | `sk_tools_json`, `sk_tool_lookup` | working — sent in the next milestone |
| `Mary/Abilities/*.mary`, `MaryFoundation/Package/*`, `MaryPlugin/Adapters/*` | — | not ported — deviation 10 |

## computer-use ← MaryComputerUse

| Swift | C | Status |
|---|---|---|
| `MaryBrain/Brain/AbilityDispatching.swift` (`dispatch`) | `computer-use/include/computer-use/invoke.h` (`mcu_invoke`), `src/invoke.c` | working — deviation 9 |
| `MaryComputerUse/Accessibility/*`, `Sight/*`, `Hands/*`, `Stage/*` | — | not ported — deviation 9 |
| `MaryComputerUse/Monitor/ComputerUseMonitor.swift` | — | planned |
| — (app state through the pipes) | `mcu_app_state` | declared — answers ENOSYS |

## brain ← MaryBrain

| Swift | C | Status |
|---|---|---|
| `MaryBrain/Prompt/PromptCatalog+Voice.swift` (`sewnPreamble`, `sewnCompany`, `sewnPersonaConverse`, `sewnRetrieval`), `PromptPlan.swift` (`voice`), `MaryPrompts+SewnModeTwo.swift` | `brain/include/brain/prompt.h`, `src/prompt.c` (`mb_sewn_instructions`) | working — deviation 8 |
| `MaryBrain/Prompt/PromptSection.swift` (`formatter`) | `brain/include/brain/clock.h`, `src/clock.c` | working — English names |
| `MaryBrain/Brain/MaryBrain+History.swift` (`spokenMessages`, `trimHistory`), `MaryBrain.swift` (`historyMessageLimit`) | `brain/include/brain/history.h`, `src/history.c` | working |
| `MaryBrain/Sewn/SewnWire.swift` (`ChatRequest`, `Persona.mary`), `SewnRealtimeWire.swift` (`TurnStart`) | `brain/include/brain/request.h`, `src/request.c` (`mb_turn_start`) | working — deviation 8 |
| `MaryBrain/Brain/MaryBrain+Turn.swift` (routing, Lane B, dispatch), `Abilities/*`, `Engine/*` | — | not ported |
| — (skills as Mistral tools) | `brain/include/brain/tools.h` | skeleton |

## voice ← MaryVoice

| Swift | C | Status |
|---|---|---|
| `MaryVoice/VAD/EnergyVAD.swift`, `VoicePipelineConfig.swift` (`VADConfig`) | `voice/include/voice/vad.h`, `src/vad.c` | working |
| `MaryVoice/VAD/EndpointHold.swift` | `voice/include/voice/wake.h` (`mv_endpoint_extra_silence`), `src/wake.c` | working |
| `MaryVoice/VAD/WakePlanner.swift` | `mv_wake_in`, `mv_could_still_wake`, `mv_is_stop_listening` | working |
| `MaryVoice/VoicePipelineEvent.swift` (`VoicePipelineState`) | `voice/include/voice/state.h`, `src/state.c` | working |
| `MaryVoice/VAD/BargeInGovernor.swift` | `voice/include/voice/barge_in.h`, `src/barge_in.c` | skeleton — deviation 6 |
| `MaryVoice/Wake/WakeWordListener.swift` | `voice/include/voice/kws.h`, `src/kws.c` (sherpa-onnx; phrases in `data/keywords.txt`, release and model pinned in `third_party.lock`) | working — deviation 1 |
| `MaryVoice/Capture/MicCapture.swift`, `TTS/Shared/KokoroStreamSpeaker.swift` (playback) | `voice/include/voice/audio.h`, `src/pipewire.c`; `voice/ring.h`, `src/ring.c` (the lock-free reply ring and the 20 ms framer between PipeWire and maryd) | working |
| `MaryVoice/STT/*`, `TTS/Kokoro/*` | — | not ported — deviations 2 and 3 |

## runtime ← MaryRuntime

| Swift | C | Status |
|---|---|---|
| `MaryRuntime/Runtime/MaryRuntime*.swift` (the composition root) | `runtime/include/runtime/daemon.h`, `src/daemon.c`, `src/queue.c`, `bin/maryd.c` | working |
| `MaryRuntime/Services/Chat/TextTurnRunner.swift`, `Services/Voice/*` (the Sewn lane of a turn) | `runtime/include/runtime/turn.h`, `src/turn.c` | working — deviation 5 |
| `MaryVoice/Wake/WakeWordListener.swift` and the voice loop around the VAD | `runtime/include/runtime/ears.h`, `src/ears.c` (`mr_heard` applies WakePlanner) | working — deviations 1 and 6 |
| `MaryVoice/STT/VoiceTranscriber.swift` (the client side) | `runtime/include/runtime/transcriber.h`, `src/transcriber.c` | working — deviation 2 |
| MaryUI's session window talking to the runtime | `runtime/include/runtime/desktop.h`, `src/desktop.c`, `bin/maryctl.c` (the desktop socket) | working — MaryOS only |
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
   With nothing retrieved, Sewn sends the model only the latest question and the system message; sewnd
   never retrieves, so it keeps up to ten earlier turns as real messages, the way Sewn does when it has
   verbatim context. Speech streams on Linux as well — Sewn's Linux build buffers each sentence whole.
6. **Barge-in.** No interrupting by voice while Mary speaks — there is no echo cancellation yet. Esc or the
   Ask Mary button stops her.
7. **Thread.** Documents are JSON files; no embeddings, product quantization or knowledge graph yet;
   `owner_id` comes from the caller's credentials; a unix socket replaces TCP port 9090.
8. **Persona and instructions.** Mary's persona says she lives on the user's Mac; on MaryOS she lives in
   MaryOS, and "another pass will close" is dropped because no follow-up pass exists yet. The voice
   instructions render the conversation persona for every turn and leave out `sewnRetrieval`'s reach and
   sight splices (and the heading, in-turn and capability sections): the conversation cannot call skills or
   look at the screen yet, and Mary must never promise what she cannot do.
9. **Computer use.** No accessibility tree, screen capture or synthetic input. MaryOS's applications are its
   own, so Mary reads their state and acts through `lp_app.perform` over the desktop socket.
10. **Abilities.** The `.mary` packages under `Mary/Abilities/` — recipes recorded against a live
    accessibility tree — are not ported. Applications declare their skills in code (`lp_skill`), and
    System Settings holds the policy per application: whether Mary may use it, which skills, and when she
    must ask first. `SkillSchema` maps to `lp_skill`; `AbilityDispatching.dispatch` maps to `mcu_invoke`.
