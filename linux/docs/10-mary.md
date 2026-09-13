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
  on the machine that talks to the network: chat, the skills lane, embeddings and graph extraction for
  threadd, Voxtral Realtime transcription and Voxtral speech, over TLS 1.2+ with the certificate and host
  checked. Every request it sends is a row in its calls ledger (`sewnctl calls`). It listens on
  `/run/sewn/sewn.sock` (0660, group `sewn`) and never on the network. A turn retrieves from the Thread
  (in the lanes maryd names), compacts what it found into the prompt, asks the model to mark the sentences
  that drew on each source, strips the marks and hands the desktop the spans to highlight, and every seventh
  exchange writes a memory note back into the Thread.
- **threadd** (`threadd.service`, user `thread`) is Mary's memory, the hard drive's own record of what is on
  it: one SQLite file, `/var/lib/thread/thread.db`, with every owner's documents, their embeddings, the
  knowledge graph and a ledger. It serves Conduit's `thread.v1` gRPC on `/run/thread/thread.sock` and the
  MaryOS ops as JSON lines on `/run/thread/local.sock` (both group `thread`). Embeddings and graph extraction
  come from sewnd; threadd itself has no network at all (`PrivateNetwork=yes`). Peering with other MaryOS
  machines is declared and not built.
- **maryd** (`maryd.service`, a user unit) is Mary on the desktop: PipeWire's microphone and speaker, the wake
  word, turns through sewnd, deposits into threadd, and the desktop's socket at
  `$XDG_RUNTIME_DIR/mary/mary.sock`. It has no `[Install]` section: the desktop launcher
  (`/usr/lib/maryos/desktop`) starts it inside the graphical session, so a console or ssh login never opens
  the microphone.
- **indexd** (`indexd.service`, a user unit started beside maryd) keeps every file in the home recorded in the
  Thread: it reconciles the home with threadd at start and every six hours (`journalctl --user -u indexd`
  reports `reconcile: N seen, 0 missing, 0 stale, 0 orphaned` when they agree) and watches the home so a
  save, a rename or a deletion reaches the graph within a second. `indexd --once` reconciles on demand.
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
`key.get`. Verify asks sewnd to try the key with Mistral, and the pane shows the answer.

In the VM, copy the key on the Mac and paste it into the field (right-click › Paste, or Ctrl+V): while the
VM's window is in front, what you copy on the Mac becomes the guest's clipboard. It only goes that way, and
`maryos vm run --no-clipboard` keeps the Mac's clipboard out. From a terminal:

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
shows it as it arrives — and cuts it into sentences for Voxtral's speech in the voice chosen in Settings, which maryd plays. When she has
finished, a six-second follow-up window listens again; Esc stops her at any point.

**Typed.** Ctrl+Space opens Spotlight; type, then Ctrl+Enter or the Ask Mary orb at the bar's right end. The
same turn runs and the answer is spoken too. Pressing the orb with the bar empty opens the microphone without
the wake word. Plain Enter still launches what the search found. [Chapter 9](09-the-desktop.md) describes the
conversation's look.

**Remembered.** Each turn, spoken or typed, finished or stopped, becomes a document in Thread's
`conversation-<you>` group with the question, the answer and when it happened, and is embedded and folded
into the knowledge graph as soon as sewnd has a key:

```sh
threadctl library
threadctl documents mary-turn-1789264000000-3fa2
threadctl search --lane conversation what did I ask about Paris
threadctl graph --entity Paris --documents
threadctl stats
```

## Her voice

Mary speaks in Marie (`fr_marie_neutral`) until you choose otherwise. System Settings › Mary › Voice asks
Mistral for its voices, through maryd and sewnd (which holds the key), and offers them in a pop-up: Marie first,
then Mistral's by language, then any voices of your own. A second pop-up chooses the mood where a voice has
several (neutral, sad, happy, excited, curious, angry), and Play Sample speaks a sentence in the voice's own
language. The choice is `mary_voice` in `~/.config/maryui/settings.conf`; the desktop sends it to maryd when it
changes and whenever maryd starts, and the next sentence is spoken in it.

```sh
sewnctl voices                       # Mistral's list, as sewnd reads it
maryctl voices                       # the same, through maryd
maryctl sample fr_marie_happy        # one sentence through Mary's speaker, or why not
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
the microphone out of the VM. Apple's device plays on PCM 0 and records on PCM 1, while PipeWire's default profile
set looks only at device 0, so the image hands the card a profile set that names both
(`overlay-vm/usr/share/maryos/alsa-card-profile/virtio-snd.conf`). Before it the VM had no microphone source, and
WirePlumber gave Mary's wake word the speaker's monitor instead.

## When she says nothing

A reply that arrives as text but is not heard says why under it in Spotlight ("Not spoken: …"):

- **speech**: Mistral refused the voice, and its own reason follows. HTTP 403 means Mistral would not speak the
  text (moderation, or an account without speech); 401 means the key. The words are kept.
- **speaker**: the audio was made but never reached a speaker. PipeWire could not be opened, or the playback
  stream took nothing for two seconds, so the rest of the reply was dropped rather than leaving Mary speaking
  forever. maryd opens the speaker again once she is idle.

Each half can be heard on its own:

```sh
sewnctl speak fr_marie_neutral "Bonjour" | pw-cat --playback --format f32 --rate 24000 --channels 1 -
maryctl sample fr_marie_neutral      # sewnd, Mistral and maryd's speaker together
journalctl -u sewnd -b | grep -iE "speech failed|turn ended"
journalctl --user -u maryd -b | tail -40
wpctl status                         # the default sink; mary-speaking under Streams while she talks
```

Play Sample in System Settings › Mary gives the same reasons under its button. If it ends without one and nothing
was heard, PipeWire did take the audio and it was lost after that: look at the default sink and its volume and mute
in `wpctl status`, and in the VM at the Mac's own output.

maryd listens and speaks through PipeWire's default devices, so System Settings › Sound chooses both. In the VM the
output is whatever the Mac is playing through (Control Center › Sound): when that is a Bluetooth receiver, Mary is
heard there and not from the MacBook. A microphone exists only with `--microphone` and macOS's permission. The image
starts the VM's speaker at full volume, so the Mac's volume alone sets how loud it is (WirePlumber would start it at
40%, about −24 dB). It also gives the virtio sound card WirePlumber's buffer settings for virtual machines, which
WirePlumber applies only when the DMI tables name QEMU or VMware, and a guest started by VZLinuxBootLoader has no DMI
tables (`overlay-vm/etc/wireplumber/main.lua.d/51-maryos-vm.lua`). Both arrive with `./ui.sh --rebuild`; in a VM
booted before that, `wpctl set-volume @DEFAULT_AUDIO_SINK@ 100%` raises the volume.

## What is not there yet

- **Skills from the conversation**, and the confirmation card.
- **Barge-in by voice.** There is no echo cancellation, so Mary is stopped with Esc or the orb, not by talking
  over her.
- **On-device speech and models.** Speech is Voxtral's through sewnd; Frigate's on-device models come later.
- **Thread peering** between MaryOS machines.
