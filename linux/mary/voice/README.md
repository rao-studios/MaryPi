# voice — Swift twin: `Mary/Sources/MaryVoice`

Microphone to transcript, and reply audio to the speaker.

Working now, and tested row for row against MaryVoice's own test tables:

- `voice/vad.h` — `EnergyVAD` and `VADConfig`: start 0.015, continue 0.008, 850 ms hangover, 300 ms minimum
  utterance, the barge-in threshold boost, and the RMS of a frame.
- `voice/wake.h` — `WakePlanner` (the name must lead; the remainder is the request; `couldStillWake` for live
  partials; the exact "stop listening" exit) and `EndpointHold` (0.7 s more when the last word dangles).
- `voice/state.h` — the pipeline's states, named the way maryd tells the desktop.
- `voice/barge_in.h` — `BargeInGovernor`, declared; it never fires until echo cancellation exists.

Coming with the audio commit: capture and playback through PipeWire, and "Hey Mary" spotted on-device with
sherpa-onnx.

Deviations: keyword spotting gates the audio (Swift transcribes every utterance to find the name —
PORTING.md 1); Voxtral Realtime through sewnd replaces on-device transcription (2); no barge-in by voice
while Mary speaks (6).

Prefix `mv_`.
