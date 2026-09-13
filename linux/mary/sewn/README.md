# sewn — Swift twin: [Sewn](https://github.com/rao-studios/Sewn)

`sewnd`, the only process on MaryOS that holds the Mistral key and the only one that talks to the
network, and `sewnctl`. Every request it sends out is a row in its calls ledger (`sewnctl calls`,
System Settings › Mary › Network activity): purpose, host, path, status, timing and bytes — never a
body, never the key. It speaks length-prefixed frames on `/run/sewn/sewn.sock`:

- Sewn's realtime turn wire (`Sources/API/Routes/Realtime/RealtimeWire.swift`: `turn.start`, `token`,
  `audio.begin`, PCM, `turn.end`, `cancel`) with Sewn's `handleChat` around it (see *A turn*).
- `complete{messages, tools?, instructions?, scope?}` — `/v1/skills/complete`: the skills lane, roles
  kept, the caller's tool roster offered, `tool_calls` answered.
- `embed{texts[]}` and `graph.extract{texts[], prompt}` — what threadd asks for, so that the Thread can
  embed and extract without a network of its own; `summarize{messages[]}` — the auto-memory note.
- `transcribe.*` for Voxtral Realtime, `voices.list` and `speak` for voices, `key.set/status/verify`,
  `calls.list` and `calls.stats`.

Every request names a `provider` or gets Mistral. Thinking Machines is on the wire and in the desktop's
toggles, as on the Mac, but not served this phase: `tinker` is answered with `error{stage: "engine"}`
before any socket is opened (`sewn/provider.h`; PORTING.md deviation 12).

Deviations: callers are authorized by peer credentials on a unix socket, not Supabase; one grounded
pass with no opener, no Sinatra and no Gita pricing; retrieval from this machine's Thread only.

## The key

The Mistral key lives in `/var/lib/sewn/mistral.key` (mode 0600, the `sewn` user, a 0700 state directory),
written atomically and never read if anyone else could read it. It arrives over the socket from System
Settings (through maryd) or from `sewnctl set`, which reads it from standard input. Only members of `sudo`
may replace it; there is no operation that returns it. `key.verify` asks Mistral's `/v1/models` whether it
is accepted, over libcurl held to HTTPS, TLS 1.2+, verified peers and no redirects. The key is never logged
and every buffer that held it is zeroed.

## A turn

`turn.start{request, tts}` carries Mary's ChatRequest, with `sewn{lanes[], groups[], entities[],
request_id}` saying what the reply may draw on. sewnd does what `Core/Sewn.swift` does:

1. **Search** — threadd's local socket (`sewn/retrieve.h`), for the connection's user, in the lanes named
   (`conversation` and `personal` when none are), top 3. The `retrieval` frame reports what came back.
2. **Compact** (`sewn/compact.h`, `Sewn+Compact.swift`) — at or under 6000 characters the partitions go in
   verbatim under **Memory**, **Documents**, **Conversation**, Mary's past actions and **Perspectives From
   Others** (`<external>`), each tagged `[n]`; above it the utility model writes the briefing with the tags
   kept adjacent to their content.
3. **Prompt** — the persona with `memoryInstruction`, the conversational instructions and base rules,
   then `--- CONTEXT ---` with the usage guide and the citation protocol: end a sentence that draws on
   source `[n]` with `[[n]]`.
4. **Generate** — Mistral's chat stream; `[[n]]` markers are stripped on the way out (`MarkerStreamFilter`,
   even split across deltas) so neither the screen nor the speech chunker sees them.
5. **Attribute** (`sewn/attribution.h`, Gita) — the contribution (owners, documents, influence by word
   share, royalty) with spans: exact ones from the markers, heuristic ones (n-gram overlap) for unmarked
   sentences. `turn.end{text, contribution, retrieved}` carries it; the desktop draws the highlights.
6. **Remember** (`sewn/memory.h`, `Sewn+AutoMemory.swift`) — every seventh user message, or when the last
   two user messages' embeddings drift apart, the conversation is summarised into a note and deposited in
   the user's `memory-<user>` group through threadd, which embeds it and folds it into the graph.

A single speech lane turns finished sentences into `audio.begin` and whole-sample PCM frames, in order.
A `cancel` frame or a closed connection stops both lanes at once. Mistral is reached through one
transport function (`sewn/transport.h`), so every op is tested against scripted streams.

## Transcription

`transcribe.start` opens a Voxtral Realtime session for one utterance. The client streams pcm_s16le frames,
then `transcribe.end` (or `cancel`); sewnd relays them as `input_audio.append` messages under Voxtral's
size limit and returns `transcribe.ready`, `transcript.delta` and `transcript.done`. Only sewnd opens the
WebSocket, because only sewnd holds the key; libwebsockets verifies Mistral's certificate against the system
CA bundle and sends the key once, as a header.

## Voices and a sample

`voices.list` reads Mistral's `GET /v1/audio/voices`, every page up to five, and answers with each voice's id to
speak by, its name and languages, and whether it is the account's own. `speak{voice_id, text}` speaks one text
as a turn's sentences are spoken (`audio.begin`, PCM, `speak.end`), for System Settings' Play Sample. Every
speech request goes through `sewn_speak` (`sewn/speech.h`): a refusal becomes `tts.failed{status, message}` with
Mistral's own reason (never the key or the words), and an answer too large to read, or with no audio, fails too.

```sh
sewnctl voices
sewnctl speak fr_marie_neutral "Bonjour" | pw-cat --playback --format f32 --rate 24000 --channels 1 -
sewnctl calls 20
```

Status: working. Prefix `sewn_`. Everything but the libwebsockets transport builds on macOS too
(getpeereid stands in for SO_PEERCRED). The unit joins the `thread` group to reach threadd's local socket.
