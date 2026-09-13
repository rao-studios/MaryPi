# common — no Swift twin

The plumbing every other package leans on — what Swift gets from Foundation, URLSession and swift-nio:

| Header | What it is |
|---|---|
| `common/buf.h` | a growable byte buffer that stays NUL-terminated |
| `common/lines.h` | newline-delimited reading with a line cap (maryd ↔ desktop JSON lines) |
| `common/sse.h` | an incremental Server-Sent Events parser (Mistral chat and TTS streams) |
| `common/base64.h` | RFC 4648 base64 (Voxtral audio in both directions) |
| `common/frame.h` | `u32 LE length \| u8 kind \| payload` frames, capped at 1 MiB (sewnd's socket) |
| `common/json.h` | json-c helpers: strict parsing, typed getters, compact output |
| `common/secure.h` | `mc_secure_zero` and a constant-time compare, for the API key |
| `common/io.h` | whole writes, non-blocking and close-on-exec fds, clocks |
| `common/log.h` | one log call; priority prefixes when journald reads stderr |

The SSE parser follows the event-stream format (fields, comments, multi-line `data`, CRLF split across
reads). It is truly incremental: Sewn's own `sseStream` buffers the whole body on Linux
(`Sewn/Sources/Providers/ModelProvider+Stream.swift`), which would hold every token until the reply ended.

Status: working. Prefix `mc_`.
