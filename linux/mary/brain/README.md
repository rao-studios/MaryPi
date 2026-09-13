# brain — Swift twin: `Mary/Sources/MaryBrain`

What a turn says to Sewn. `brain/prompt.h` renders the voice instructions exactly as
`MaryPrompts.sewnInstructions` renders `PromptPlan.voice`, for what a MaryOS turn holds: the clock and
spoken register (`sewnPreamble`), company first (`sewnCompany`), the conversation persona
(`sewnPersonaConverse`) and the retrieval doctrine (`sewnRetrieval`). `brain/history.h` keeps the spoken
history with `trimHistory`'s whole-exchange trimming at twelve messages. `brain/request.h` wraps them in
`turn.start` with `SewnWire.ChatRequest`'s defaults and Mary's persona; `brain/clock.h` supplies the clock.

Deviations (PORTING.md 8): the persona says Mary lives in MaryOS, not on a Mac, and drops "another pass
will close"; the retrieval doctrine drops its reach and sight splices, and no heading, in-turn or capability
sections render, because the conversation cannot call skills or look at the screen yet. Routing, Lane B,
the skill pipeline and dispatch are not ported; `brain/tools.h` declares the tools the next milestone renders
from the skill registry.

Status: working. Prefix `mb_`.
