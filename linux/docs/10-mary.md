# 10. Mary: the voice, the Spotlight conversation and skills

MaryOS exists to run Mary. On macOS she is a Swift app beside five sibling repositories; on MaryOS each of
those, and each of Mary's own targets, is a C package in `mary/` (its [README](../mary/README.md) and
[PORTING.md](../mary/PORTING.md) map every file to its Swift twin). This chapter is about what the image does
with them: which processes run, how the Mistral key is kept, what happens between "Hey Mary" and her answer,
and how she is allowed to use the apps.

## What runs

```
 desktop session (DEFAULT_USER)                          system services
 ┌───────────────────────────────┐                       ┌──────────────────────────────┐
 │ maryui-desktop                │                       │ sewnd   user sewn            │
 │   lp_mary ──┐                 │   /run/sewn/sewn.sock │   the Mistral key, TLS out   │
 │             │ newline JSON    │ ┌───────────────────▶ │   /var/lib/sewn (0700)       │
 │ maryd ◀─────┘ $XDG_RUNTIME_DIR│ │                     ├──────────────────────────────┤
 │   mic, speaker, wake word ────┼─┤ /run/thread/…sock   │ threadd user thread          │
 │   (user unit)                 │ └───────────────────▶ │   memory, gRPC, no network   │
 └───────────────────────────────┘                       └──────────────────────────────┘
```

- **sewnd** (`sewnd.service`, user `sewn`) is the only process that holds the Mistral API key and the only one
  that talks to Mistral: chat, Voxtral Realtime transcription and Voxtral speech, over TLS 1.2+ with the
  certificate and host checked. It listens on `/run/sewn/sewn.sock` (0660, group `sewn`) and never on the
  network.
- **threadd** (`threadd.service`, user `thread`) is Mary's memory: Conduit's `thread.v1` gRPC on
  `/run/thread/thread.sock` (group `thread`), documents per owner in `/var/lib/thread`. Peering with other
  MaryOS machines is declared and not built, so the unit has no network at all (`PrivateNetwork=yes`).
- **maryd** (`maryd.service`, a user unit) is Mary on the desktop: PipeWire's microphone and speaker, the wake
  word, turns through sewnd, deposits into threadd, and the desktop's socket at
  `$XDG_RUNTIME_DIR/mary/mary.sock`. It has no `[Install]` section: the desktop launcher
  (`/usr/lib/maryos/desktop`) starts it inside the graphical session, so a console or ssh login never opens
  the microphone.
- **The desktop** carries the client (`lp_mary`), Spotlight's conversation and the skills. It reconnects to
  maryd by itself whenever maryd comes back.

`hooks/desktop/64-mary.sh` creates the two system users (`usr/lib/sysusers.d/mary.conf`), puts the desktop
user in `sewn` and `thread` so it can reach their sockets, and enables both services. The desktop user is
already in `sudo`, which is who sewnd lets set the key.

## The key

System Settings › Mary has a masked field. Save hands the key to maryd, which passes it straight to sewnd's
`key.set` and zeroes every copy it saw; the field zeroes itself too. sewnd checks the caller's credentials as
the kernel reports them (`SO_PEERCRED`) and refuses anyone outside `sudo`, writes the key atomically to
`/var/lib/sewn/mistral.key` (0600, in a 0700 directory), and never logs it or sends it back: there is no
`key.get`. Verify asks sewnd to try the key with Mistral, and the pane shows the answer. From a terminal:

```sh
sewnctl set        # reads the key from standard input with echo off, never from the command line
sewnctl verify
sewnctl status
```

## A turn

**Spoken.** While the desktop is idle, maryd feeds the microphone to sherpa-onnx's keyword spotter, on the
machine; nothing leaves it until the spotter hears "Hey Mary" (or "OK", "Okay", "Hi" Mary). Then Spotlight
opens on the conversation, listening, and maryd streams the last 600 ms and everything after it to sewnd,
which relays it to Voxtral Realtime. Energy VAD and an end-of-phrase hold decide when you have finished;
WakePlanner takes "Hey Mary" off the front, and what is left is the question. brain builds the request (the
voice instructions, the last twelve messages), sewnd streams Mistral's answer back token by token — Spotlight
shows it as it arrives — and cuts it into sentences for Voxtral's speech, which maryd plays. When she has
finished, a six-second follow-up window listens again; Esc stops her at any point.

**Typed.** Ctrl+Space opens Spotlight; type, then Ctrl+Enter or the Ask Mary orb at the bar's right end. The
same turn runs and the answer is spoken too. Pressing the orb with the bar empty opens the microphone without
the wake word. Plain Enter still launches what the search found. [Chapter 9](09-the-desktop.md) describes the
conversation's look.

**Remembered.** Each turn, spoken or typed, finished or stopped, becomes a document in Thread's
`mary-conversations` group with the question, the answer and when it happened:

```sh
threadctl library
threadctl documents mary-turn-1789264000000-3fa2
```

## Skills: what Mary may do with each app

Mary never reads the screen or synthesises input on MaryOS. The apps are the desktop's own, so each declares
its skills in code — an id, a title, a JSON Schema for the arguments, and whether it reads, acts or cannot be
undone — and performs one through the same code its menus run. The first are System Settings' `open_pane`,
the Media Player's `play_pause` and Calendar's `events_today`. System Settings › Mary has a group per app:
whether Mary may use it, when she asks first (never, before changes, always) and a switch per skill, saved in
`~/.config/maryui/skills.conf`. The desktop decides every call against that policy before anything runs.

```sh
maryctl skills
maryctl skill settings open_pane '{"pane":"sound"}'
maryctl skill calendar events_today
maryctl skill media play_pause     # "needs_confirmation": it acts, and the default is to ask first
```

The conversation does not call skills yet; that, and a confirmation card in Spotlight for calls that need
one, are the next milestone.

## The units

Both system services run with no capabilities, `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`,
private `/tmp` and devices, the kernel and control groups protected, `MemoryDenyWriteExecute`,
`SystemCallFilter=@system-service`, and only the address families they use (sewnd: unix and IP; threadd: unix,
and no network). `systemd-analyze security` rates sewnd 1.5 (OK) and threadd 0.8 (SAFE); sewnd's score is
higher only because it must reach the internet. maryd is a user unit, so it gets
what a user manager can apply: `NoNewPrivileges`, `LockPersonality` and native system calls only.

```sh
systemctl status sewnd threadd
systemctl --user status maryd
journalctl -u sewnd -b          # the key is never in here
maryctl status                  # state, whether a key is stored, whether the wake word is listening
maryctl ask "what's the capital of France?"
```

## The dev loop

`make mary` compiles every package and runs its tests in the builder (chapter 3), fetching sherpa-onnx and the
wake-word model pinned by SHA-256 in `mary/third_party.lock` and refusing anything that differs. With
`maryos.ui=dev` on the kernel command line (`ui.sh --dev`), the VM image's `mary-dev-run`
(`hooks/vm/20-mary-dev.sh`) runs sewnd, threadd and maryd from `/mnt/maryos-out/mary` and restarts each when
its binary changes, the way the launcher does for the compositor. The Pi image never gets it.

The VM hears through the Mac. `ui.sh` boots it with `maryos vm run --microphone`, which gives the guest's sound
device an input fed by this Mac's microphone; macOS asks once, and until it is allowed (System Settings › Privacy
& Security › Microphone) the guest hears silence — typed turns work either way. `./ui.sh --no-microphone` keeps
the microphone out of the VM.

## What is not there yet

- **Skills from the conversation**, and the confirmation card.
- **Barge-in by voice.** There is no echo cancellation, so Mary is stopped with Esc or the orb, not by talking
  over her.
- **On-device speech and models.** Speech is Voxtral's through sewnd; Frigate's on-device models come later.
- **Thread peering** between MaryOS machines.
