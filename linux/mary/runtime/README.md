# runtime — Swift twin: `Mary/Sources/MaryRuntime`

`maryd`, the composition root, run as a systemd user service by the desktop's launcher, and `maryctl`.
It wires voice, brain, sewnd, threadd and the skill pipes together and serves the desktop's Spotlight
on `$XDG_RUNTIME_DIR/mary/mary.sock` (newline-delimited JSON).

Status: planned (commit 14). Prefix `mr_`.
