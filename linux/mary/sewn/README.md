# sewn — Swift twin: [Sewn](https://github.com/rao-studios/Sewn)

`sewnd`, the only process on MaryOS that holds the Mistral key, and `sewnctl`. It speaks Sewn's
realtime turn wire (`Sources/API/Routes/Realtime/RealtimeWire.swift`: `turn.start`, `token`,
`audio.begin`, PCM, `turn.end`, `cancel`) over length-prefixed frames on `/run/sewn/sewn.sock`, plus
`transcribe.*` for Voxtral Realtime and `key.set/status/verify`. The sentence chunker and TTS sanitizer
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

Status: the chunker, the TTS sanitizer, Mistral's wire (`sewn/mistral.h`), the key store, peer checks,
`key.status` / `key.set` / `key.verify` and `turn.start` are working; transcription follows (commit 7).
Prefix `sewn_`. Everything builds on macOS too (getpeereid stands in for SO_PEERCRED).
