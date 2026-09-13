# sewn — Swift twin: [Sewn](https://github.com/rao-studios/Sewn)

`sewnd`, the only process on MaryOS that holds the Mistral key, and `sewnctl`. It speaks Sewn's
realtime turn wire (`Sources/API/Routes/Realtime/RealtimeWire.swift`: `turn.start`, `token`,
`audio.begin`, PCM, `turn.end`, `cancel`) over length-prefixed frames on `/run/sewn/sewn.sock`, plus
`transcribe.*` for Voxtral Realtime, `voices.list` and `speak` for voices, and `key.set/status/verify`. The sentence chunker and TTS sanitizer
are ports of `Sources/Utilities/StreamingSentenceChunker.swift`.

Deviations: callers are authorized by peer credentials on a unix socket, not Supabase; one grounded
pass, with no opener, retrieval, Sinatra or Gita.

## The key

The Mistral key lives in `/var/lib/sewn/mistral.key` (mode 0600, the `sewn` user, a 0700 state directory),
written atomically and never read if anyone else could read it. It arrives over the socket from System
Settings (through maryd) or from `sewnctl set`, which reads it from standard input. Only members of `sudo`
may replace it; there is no operation that returns it. `key.verify` asks Mistral's `/v1/models` whether it
is accepted, over libcurl held to HTTPS, TLS 1.2+, verified peers and no redirects. The key is never logged
and every buffer that held it is zeroed.

## A turn

`turn.start{request, tts}` carries Mary's ChatRequest. sewnd composes Sewn's system prompt for a turn with
no retrieved context, streams Mistral's chat reply as `token` frames, and feeds the sentence chunker; a
single speech lane turns finished sentences into `audio.begin` and whole-sample PCM frames, in order. A
`cancel` frame or a closed connection stops both at once. Mistral is reached through one transport function
(`sewn/transport.h`), so the turn is tested against scripted streams.

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
```

Status: the chunker, the TTS sanitizer, Mistral's wire (`sewn/mistral.h`), the key store, peer checks,
`key.status` / `key.set` / `key.verify`, `turn.start`, `transcribe.start`, `voices.list` and `speak` are working. Prefix `sewn_`.
Everything but the libwebsockets transport builds on macOS too (getpeereid stands in for SO_PEERCRED).
