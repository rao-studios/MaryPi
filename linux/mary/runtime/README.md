# runtime — Swift twin: `Mary/Sources/MaryRuntime`

`maryd`, the composition root, and `maryctl`. maryd is a systemd user service the desktop's launcher starts,
so the microphone only opens inside a graphical session. It serves the desktop on
`$XDG_RUNTIME_DIR/mary/mary.sock` (0600, newline-delimited JSON, lines up to 64 KB, same user only) and
reaches sewnd and threadd on their own sockets, one connection per operation — a restarted daemon is simply
reached again next time.

- `runtime/daemon.h` — the main loop. It owns every piece of state; worker threads (the turn, the ears'
  transcription, key calls, Thread deposits, triage, the skill index, the skills lane) post events to
  `runtime/queue.h` and never touch it.
- `runtime/turn.h` — a turn through sewnd: tokens become `reply.delta`, audio goes straight to the speaker's
  ring, and the state is `speaking` from the first audio. Stopping shuts the socket down, which sewnd takes
  as cancel.
- `runtime/ears.h` — the voice thread: "Hey Mary" spotted in standby, then a session: pre-roll and live
  frames to sewnd's transcription, EnergyVAD with EndpointHold ending the utterance, and `mr_heard` applying
  WakePlanner to what was heard. A spoken reply is followed, once the speaker is quiet and 300 ms more have
  passed, by a follow-up session; six seconds of silence ends it.
- `runtime/transcriber.h` — sewnd's `transcribe.*` from the client side.
- `runtime/desktop.h` — the socket: `hello`, `state`, `level`, `transcript`, `reply.delta`/`reply.end`,
  `key.status`, `error` and `skill.invoke` out, and `voices` and `voice.sample` to the client that asked; `ask`,
  `listen`, `stop`, `dismiss`, `key.set`/`verify`/`status`, `config{wake, voice}`, `voices.list`, `voice.sample`,
  `skills` and `skill.result` in. The Mistral key only passes through: it is
  checked, handed to sewnd, and every copy maryd saw is zeroed.

Skills: the desktop publishes `skills{apps}`; `maryctl skill APP SKILL [JSON]` is decided against that
policy (unknown, denied, needs_confirmation) and otherwise sent to the desktop as `skill.invoke` through
computer-use's pipes. The skills lane parks a call that needs confirmation on the card in Spotlight; when the
person allows it, the `skill.invoke` carries `confirmed:true` and the desktop's own gate lets it through.
When the skills arrive maryd builds the skill index triage scores against (sewnd's
`embed`) and writes one `style` record per discipline into the Thread (over threadd's local socket);
nothing per skill or app is recorded (PORTING.md 15).

A turn (PORTING.md 14): the desktop is asked for the world, triage embeds the words, and the ambient engine
routes with triage's verdict (an action-shaped request is an action turn; a unique winner is the intent);
when nothing open serves the need, the realm's spawn — the closest application of it — leads, and the
lane's call opens it. A unique skill winner with a safe argument shape and a single clause dispatches with no model round —
the receipt is the reply, and the words become a `routing` habit in the Thread. An action turn (or a winner
that cannot dispatch) runs the skills lane: `complete` rounds through sewnd, calls through the desktop's
pipes, a protected skill parked on the desktop's confirmation card (`skill.confirm{call_id, app, skill, args,
summary}` out; `skill.confirm.reply{call_id, yes}` back, or a bare yes or no said to Mary); its answer is
spoken through sewnd's `speak`. Every other turn is the voice with retrieval (`turn.start`). `reply.end`
carries the runs, and the episode is sealed and deposited as a `behavior` record whenever a skill ran; the
turn itself is not deposited (Sewn's memory covers the conversation). `maryctl triage TEXT` shows who would
answer without a model.

Voice: turns are spoken in the voice the desktop sends (`config{voice}`, Marie until then). A sample speaks one
text through the speaker with sewnd's `speak`, with the ears muted, and stays out of the conversation and Thread.
When Mistral refuses a reply's voice, or the speaker takes nothing for two seconds, the words are kept and
`error{stage: speech | speaker}` says why; the speaker is opened again once Mary is idle.

```sh
maryctl status
maryctl ask "what's the capital of France?"
maryctl listen
maryctl skills
maryctl skill settings open_pane '{"pane":"sound"}'
maryctl voices
maryctl sample fr_marie_happy
maryctl triage play the music
maryctl abilities
```

Tested end to end against a fake sewnd, a fake threadd and fake desktop clients, with audio injected where
PipeWire's capture delivers it. Prefix `mr_`.
