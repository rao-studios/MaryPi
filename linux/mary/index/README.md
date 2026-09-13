# index — no Swift twin

`indexd`: every file in the home has a record in the Thread, so the hard drive's memory
is the hard drive. A user unit the desktop launcher starts beside maryd, as the desktop
user, with no key and no network:

- **Reconcile** (`index/reconcile.h`) at start, every six hours, and on `indexd --once`:
  walk the home (`index/walk.h`: regular files, no dotted names — `.cache`, `.config`,
  the Trash under `.local` — no symlinks, no other filesystems, nothing over 32 MB),
  report pages of `{path, size, mtime_ms, hash}` to threadd's `parity.report`, deposit
  the records threadd calls missing or stale, and let the last page remove the records
  whose files are gone. `journalctl --user -u indexd` shows `reconcile: N seen, 0
  missing, 0 stale, 0 orphaned` when the drive and the graph agree.
- **Watch** (`index/watch.h`, Linux inotify over every directory the walk admits): a
  save becomes one `deposit` after 500 ms of quiet, a rename `file.move`, a deletion
  `file.remove`, a new or moved folder a reconcile.

A record (`index/record.h`) is the desktop's idea of the file (`index/kind.h`, the same
extension lists and UTF-8 sniff as Finder): text and code are chunked and embedded by
threadd; images, audio, video and other documents get one line naming them. Every record
carries the path, kind, size, mtime and SHA-256 in its metadata and file row, and the
graph entities the `file` family declares — the file and its folders, named by their path
under the home, joined by `in` — so *View Thread* on a file in Finder opens its node.

The document id is threadd's: `file-<fnv(owner|path)>`, so a re-save replaces the record
and a rename re-keys it in place, keeping what the graph learned.

```sh
indexd --once                       # reconcile now and report
threadctl file ~/Documents/notes.txt
threadctl parity
```

Status: working. Prefix `ix_`. The watch builds on Linux; the walk, records and
reconcile are tested everywhere.
