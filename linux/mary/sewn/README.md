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

Status: the chunker, the TTS sanitizer, Mistral's wire (`sewn/mistral.h`), the key store, peer checks and
`sewnd`'s `key.status` / `key.set` / `key.verify` are working; turns and transcription follow (commits 6–7).
Prefix `sewn_`. Everything builds on macOS too (getpeereid stands in for SO_PEERCRED).
