# voice — Swift twin: `Mary/Sources/MaryVoice`

Microphone to transcript, and reply audio to the speaker.

Working now, and tested row for row against MaryVoice's own test tables:

- `voice/vad.h` — `EnergyVAD` and `VADConfig`: start 0.015, continue 0.008, 850 ms hangover, 300 ms minimum
  utterance, the barge-in threshold boost, and the RMS of a frame.
- `voice/wake.h` — `WakePlanner` (the name must lead; the remainder is the request; `couldStillWake` for live
  partials; the exact "stop listening" exit) and `EndpointHold` (0.7 s more when the last word dangles).
- `voice/state.h` — the pipeline's states, named the way maryd tells the desktop.
- `voice/barge_in.h` — `BargeInGovernor`, declared; it never fires until echo cancellation exists.

Audio and the wake word:

- `voice/audio.h` — PipeWire. One capture stream, 16 kHz mono s16, cut into 20 ms frames on PipeWire's
  thread; one playback stream, 24 kHz mono float (Voxtral's voice), fed from a ring that Esc can empty
  mid-sentence. maryd runs as the desktop's user, so both reach that session's PipeWire.
- `voice/ring.h` — that ring (one writer, one reader, no locks) and the framer, tested without PipeWire.
- `voice/kws.h` — "Hey Mary" spotted on the machine by sherpa-onnx's keyword spotter with its 3.3 M
  gigaspeech zipformer. The release and the model are pinned by SHA-256 in
  [third_party.lock](../third_party.lock); the builder fetches them and passes `SHERPA_DIR` to make.
  sherpa-onnx ends the process when a keyword names a token its model lacks, so every keyword file is
  checked against `tokens.txt` before the spotter loads it.

`data/keywords.txt` spells HEY, OK, OKAY and HI + MARY in the model's BPE pieces, and each `@name`, read as
words, is a phrase WakePlanner accepts — a test holds the file to both. To change the phrases, write them
plainly (`HEY MARY :2.0 #0.25 @HEY_MARY`) and tokenize them with the model's `bpe.model`:

    pip install sherpa-onnx sentencepiece click pypinyin
    sherpa-onnx-cli text2token --tokens tokens.txt --tokens-type bpe --bpe-model bpe.model raw.txt keywords.txt

The spotter's own test runs against the real model when `MARY_KWS_MODEL` names it, as the builder does; on a
Mac the keyword checks run and the spotter answers -ENOSYS.

Deviations: keyword spotting gates the audio (Swift transcribes every utterance to find the name —
PORTING.md 1); Voxtral Realtime through sewnd replaces on-device transcription (2); no barge-in by voice
while Mary speaks (6).

Prefix `mv_`.
