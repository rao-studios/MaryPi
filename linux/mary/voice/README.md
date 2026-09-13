# voice — Swift twin: `Mary/Sources/MaryVoice`

Microphone to wake word to transcript, and reply audio to the speaker. Ports `VAD/EnergyVAD.swift`,
`VAD/EndpointHold.swift`, `Wake/WakePlanner.swift` and the pipeline's states; captures and plays through
PipeWire; spots "Hey Mary" on-device with sherpa-onnx.

Deviations: keyword spotting gates the audio (Swift transcribes every utterance to find the name);
Voxtral Realtime through sewnd replaces on-device transcription; no barge-in by voice while speaking.

Status: planned (commits 11 and 13). Prefix `mv_`.
