# brain — Swift twin: `Mary/Sources/MaryBrain`

What a turn says to Sewn. `brain/prompt.h` renders the voice instructions exactly as
`MaryPrompts.sewnInstructions` renders `PromptPlan.voice`, for what a MaryOS turn holds: the clock and
spoken register (`sewnPreamble`), company first (`sewnCompany`), the conversation persona
(`sewnPersonaConverse`) and the retrieval doctrine (`sewnRetrieval`). `brain/history.h` keeps the spoken
history with `trimHistory`'s whole-exchange trimming at twelve messages. `brain/request.h` wraps them in
`turn.start` with `SewnWire.ChatRequest`'s defaults and Mary's persona; `brain/clock.h` supplies the clock.

The abilities side (PORTING.md 14):

- `brain/triage.h` — `TurnTriage`, `EmbeddingRouting` and the spoken-argument extractors. Every skill's
  text (title, summary, phrases, tokens) is embedded once through sewnd's `embed`; a turn embeds its words
  once and the affinities are cosines. One skill above 0.62 with 0.04 over the runner-up is the unique
  winner; it dispatches with no model round when its argument shape is safe (no required argument, one
  spoken string, one enum the sentence names) and the sentence is a single clause. `mb_deterministic_decision`
  is the bare yes or no that answers a parked confirmation.
- `brain/lane.h` — Lane B, the silent skills loop (`runOrchestratorLane`): rounds of `complete` through
  sewnd, each tool call decided by the registry, parked for the person when it needs confirmation, refused
  by the repeat guard when it repeats a failed or unproven call, dispatched through the desktop's pipes with
  its result riding back as a tool message; ten rounds at most, the continuation nudge once. The hooks
  block, so maryd runs the lane on a thread and bridges to its loop.
- `brain/scope.h` — the memory plan: which of the two storage lanes each purpose (routing, orchestration,
  the reply's context) may retrieve from, the Recall toggles applied, the owner's groups (memory, files,
  style, the targets' behaviour groups) and the relationship cues.
- `mb_system_prompt` in `brain/prompt.h` — the orchestration prompt (`PromptCatalog+System`) worded for
  MaryOS, with the executor addendum and the continuation nudge.

Deviations (PORTING.md 8, 14): the persona says Mary lives in MaryOS, not on a Mac, and drops "another pass
will close"; the retrieval doctrine drops its reach and sight splices. There is no intent corpus: the lexical
ladder decides the intent, and a unique skill winner promotes a would-be conversation to the skills lane.

Status: working. Prefix `mb_`.
