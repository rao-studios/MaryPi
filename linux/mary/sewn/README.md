# sewn — Swift twin: [Sewn](https://github.com/rao-studios/Sewn)

`sewnd`, the only process on MaryOS that holds the Mistral key, and `sewnctl`. It speaks Sewn's
realtime turn wire (`Sources/API/Routes/Realtime/RealtimeWire.swift`: `turn.start`, `token`,
`audio.begin`, PCM, `turn.end`, `cancel`) over length-prefixed frames on `/run/sewn/sewn.sock`, plus
`transcribe.*` for Voxtral Realtime and `key.set/status/verify`. The sentence chunker and TTS sanitizer
are ports of `Sources/Utilities/StreamingSentenceChunker.swift`.

Deviations: callers are authorized by peer credentials on a unix socket, not Supabase; one grounded
pass, with no opener, retrieval, Sinatra or Gita.

Status: planned (commits 4–7). Prefix `sewn_`. The daemon needs Linux, libcurl, libwebsockets, openssl.
