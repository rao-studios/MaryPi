# runtime — Swift twin: `Mary/Sources/MaryRuntime`

`maryd`, the composition root, and `maryctl`. maryd is a systemd user service the desktop's launcher starts,
so the microphone only opens inside a graphical session. It serves the desktop on
`$XDG_RUNTIME_DIR/mary/mary.sock` (0600, newline-delimited JSON, lines up to 64 KB, same user only) and
reaches sewnd and threadd on their own sockets, one connection per operation — a restarted daemon is simply
reached again next time.

- `runtime/daemon.h` — the main loop. It owns every piece of state; worker threads (the turn, the ears'
  transcription, key calls, Thread deposits) post events to `runtime/queue.h` and never touch it.
- `runtime/turn.h` — a turn through sewnd: tokens become `reply.delta`, audio goes straight to the speaker's
  ring, and the state is `speaking` from the first audio. Stopping shuts the socket down, which sewnd takes
  as cancel.
- `runtime/ears.h` — the voice thread: "Hey Mary" spotted in standby, then a session: pre-roll and live
  frames to sewnd's transcription, EnergyVAD with EndpointHold ending the utterance, and `mr_heard` applying
  WakePlanner to what was heard. A spoken reply is followed, once the speaker is quiet and 300 ms more have
  passed, by a follow-up session; six seconds of silence ends it.
- `runtime/transcriber.h` — sewnd's `transcribe.*` from the client side.
- `runtime/desktop.h` — the socket: `hello`, `state`, `level`, `transcript`, `reply.delta`/`reply.end`,
  `key.status`, `error` and `skill.invoke` out; `ask`, `listen`, `stop`, `dismiss`, `key.set`/`verify`/
  `status`, `config{wake}`, `skills` and `skill.result` in. The Mistral key only passes through: it is
  checked, handed to sewnd, and every copy maryd saw is zeroed.

Skills: the desktop publishes `skills{apps}`; `maryctl skill APP SKILL [JSON]` is decided against that
policy (unknown, denied, needs_confirmation) and otherwise sent to the desktop as `skill.invoke` through
computer-use's pipes. The conversation does not call skills yet.

```sh
maryctl status
maryctl ask "what's the capital of France?"
maryctl listen
maryctl skills
maryctl skill settings open_pane '{"pane":"sound"}'
```

Tested end to end against a fake sewnd, a fake threadd and fake desktop clients, with audio injected where
PipeWire's capture delivers it. Prefix `mr_`.
