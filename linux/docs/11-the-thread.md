# 11. The Thread: the hard drive is the memory

On the Mac, Thread is a service beside Mary that keeps what she has read and said. On MaryOS the Thread is
the drive's own record of itself: one SQLite file, `/var/lib/thread/thread.db`, holding every file in the
home, every memory Sewn wrote, every action Mary took and how you ask for things — chunked, embedded, and
folded into one knowledge graph. It keeps what Mary learns, not what she already knows: the applications'
functions are declared in code and read from the desktop, so they are not recorded, and the conversation
itself is not kept turn by turn — Sewn's memory notes cover it. Back the file up and the whole memory moves
with it; there is one drive per install, and Finder and Disk Utility show it as "MaryOS". This chapter is
what is in the file, how it stays in step with the disk, and how to look at it.

## What runs

- **threadd** (`threadd.service`, user `thread`) owns the file. It serves Conduit's `thread.v1` gRPC on
  `/run/thread/thread.sock` — the Mac's wire, byte for byte, for maryd, sewnd and any peer — and the
  MaryOS-only ops as JSON lines on `/run/thread/local.sock`, which the desktop's Threads app, indexd and
  `threadctl` speak. It has no network (`PrivateNetwork=yes`): embeddings and graph extraction are asked
  of sewnd, the one process that reaches Mistral.
- **indexd** (a user unit beside maryd) keeps every file in the home recorded: a reconcile at start and
  every six hours, and inotify in between, so a save, a rename or a deletion reaches the graph within a
  second.
- **sewnd** retrieves from the Thread for every reply, compacts what it found into the prompt, and writes a
  memory note every seventh exchange; **maryd** deposits a style record per discipline, every behaviour and
  every routing habit.

The owner of a record is the unix user that wrote it, read from the socket's peer credentials. One caller
is trusted the way Sewn is the Mac's mothership: the `sewn` user may name the owner it writes for, so
auto-memory lands in the user's own groups.

## The record families

Every document carries a `family`, declared once in `thread/src/families.c` and stamped by the writer; the
Threads app's Schemas tab and `threadctl schemas` read the same table. A **lane** is a set of families, so a
search can be constrained to what a purpose is allowed to draw on.

| family | lane | id | group | written by | what it is |
|---|---|---|---|---|---|
| `file` | personal | `file-<fnv(owner\|path)>` | `files-<owner>` | indexd | one record per file: text chunked and embedded, media by name; the graph holds the file `in` its folder |
| `memory` | personal | Thread's content hash | `memory-<owner>` | sewnd | auto-memory: a note every seven user messages, or on a change of topic |
| `style` | personal | `mary-style-profile-<fnv(owner\|discipline)>` | `mary-style-<owner>` | maryd | one profile per discipline: the craft, the apps that realize it, their skills' words; the graph holds each app `practices` the discipline |
| `behavior` | behavioral | `mary-behavior-<uuid>` | `mary-ability-<fnv(owner\|ability\|paradigm)>` | maryd | a sealed BehavioralEpisode (`mary.behavior`): the request, the ambient capture, every action and its outcome |
| `routing` | behavioral | `mary-routing-<intent>\|<skill>\|<epoch>` | `mary-routing-<owner>` | maryd | a request Mary settled without a model, so the router learns how you ask |

Two lanes: **personal** (file, memory, style) and **behavioral** (behavior, routing). The ids and groups
are minted exactly as the Mac's `ThreadMemoryTopology` mints them (FNV-1a over the lowercase,
whitespace-folded key), so a record written here reads like one written there. Five families of the first
pass were retired — `conversation`, `interaction`, `ability`, `ability-schema` and `application` — because
Mary is the operating system and already knows what its applications can do, and because Sewn's memory
covers the conversation; a file from before (`user_version` 1) is cleaned at open through the same path
Remove takes, with one ledger row saying how many records went.

## The graph

Entities are Thread's ontology — person, organization, place, event, work, concept, other — plus Mary's
own kinds: file, folder, app, ability (a discipline), episode, memory. Predicates are a closed vocabulary:
`in`, `opened with`, `about`, `records`, `practices`, and the automatic `appears with` between things
mentioned together. Ids are
Thread's own (`numericHash("<kind>|<name>")`), a mention counts once per document, and removing a document
detaches its provenance and deletes what nothing else mentions.

Every search is: candidates by lane and group → narrowing through the graph → an exact float32 scan of
the chunks (mistral-embed, 1024 dimensions) → a one-hop expansion over the entities the hits share, and a
trace of every step in the ledger. The Threads app's **Graph** tab draws the neighbourhood of a seed
(hops, kinds, documents) and carries the repair bench: rename, merge, set kind, delete, re-extract.

## Parity with the disk

indexd walks the home (not dotfiles, not `.cache`, not the Trash) and reports every file to threadd in
pages; threadd answers what is missing, stale or orphaned, and on the last page removes the orphans from the
graph. The Threads app's **Drive** tab shows the gauge — "1,204 of 1,204 files in the graph" — with
**Reconcile**; a file's right-click menu in the Finder has **View Thread**, which opens the graph on the
file's node with what the record holds.

```sh
threadctl stats                              # documents, chunks, entities, the file's size, the vector footprint
threadctl schemas                            # the families, with counts
threadctl file ~/Documents/Tides.txt         # the record, its chunks, its entities, its ledger rows
threadctl search --lane personal what did I write about the tide
threadctl graph --entity Tides.txt --hops 2 --documents
threadctl ledger --kind search --limit 20
threadctl parity
threadctl backup ~/thread-backup.db          # VACUUM INTO: one file, the whole memory
```

## The Threads app

**Drive** (the volume, the database, parity, the enrichment queue) · **Library** (groups by family, then
documents; a behaviour record opens in the Mac's codec view) · **Graph** (the node canvas and the repair
bench) · **Schemas** (one card per family: patterns, fields, graph mapping, count, an example) · **Ledger**
(every deposit, search, embed, extraction, reconcile and mutation) · **Retrieval** (per turn, what each
purpose asked of which lanes and what came back, with the contribution). Finder's and Disk Utility's volume
menus open it on Drive, Graph, Schemas or Parity; a highlighted passage in Spotlight opens "From the
thread", which names the documents the reply drew on and opens them here.
