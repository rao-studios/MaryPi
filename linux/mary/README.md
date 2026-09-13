# Mary in C

MaryOS exists to run Mary. On macOS Mary is a Swift app beside five sibling repositories; here each of
them, and each of Mary's own targets, becomes a C package in this directory, so a file can be read
against its Swift twin. [PORTING.md](PORTING.md) is that map, row by row, with every place MaryOS does
something differently and why.

The first path that works end to end: "Hey Mary" opens Spotlight listening, the question is transcribed,
`sewnd` runs the turn through Mistral, and the reply streams into Spotlight while it is spoken. Typing into
Spotlight takes the same path.

Mary never reads the screen or the accessibility tree on MaryOS. The applications are MaryOS's own, so
she acts through direct pipes into them, and what she may do with each one is a **skill** that System
Settings lets you allow, restrict or require confirmation for, per application.

## Packages

| Package | Swift twin | What it is |
|---|---|---|
| [common](common/) | — | JSON, SSE, base64, frames, secret zeroing, log |
| [conduit](conduit/) | Conduit | `thread.v1` over gRPC (nghttp2 + protobuf-c) |
| [sewn](sewn/) | Sewn | `sewnd`: the Mistral key, chat, speech in and out |
| [thread](thread/) | Thread | `threadd`: MaryOS's memory |
| [mary-thread](mary-thread/) | MaryThread | Mary's Thread client |
| [skills](skills/) | MaryPlugin, SkillSchema | what Mary can do with each app |
| [computer-use](computer-use/) | MaryComputerUse | direct pipes into the apps |
| [brain](brain/) | MaryBrain | the converse turn and its prompt |
| [voice](voice/) | MaryVoice | microphone, wake word, VAD, speaker |
| [runtime](runtime/) | MaryRuntime | `maryd`, the composition root |
| [foundation](foundation/) | MaryFoundation | `MaryValue` and envelopes (skeleton) |
| [ambient](ambient/) | MaryAmbient | intents and focus (skeleton) |
| [fleet](fleet/) | Fleet | LoRA registry and the JSON gate (skeleton) |
| [frigate](frigate/) | Frigate | on-device models, for later (skeleton) |

Each package has `include/<package>/`, `src/`, `tests/test_*.c` and a README whose first line names its
Swift twin. A package links only the packages its `DEPS_` line in the Makefile names.

## Building

```sh
make            # static libraries, tools and daemons for every package whose libraries were found
make test       # every package's tests
make check-deps # which optional libraries were found, and which packages were skipped for it
```

json-c is required. libcurl, libwebsockets, openssl, libnghttp2, libprotobuf-c and libpipewire are
optional per package, so the pure packages build and test on a Mac; the MaryPi builder image has all of
them. Everything is written under `O=` (default `build/`).

sherpa-onnx, which spots "Hey Mary", is not packaged for Noble: the builder downloads the release and the
keyword model pinned in [third_party.lock](third_party.lock), refuses any file whose SHA-256 or size differs,
and hands the unpacked tree to make as `SHERPA_DIR`. Without it voice builds without the wake word.
