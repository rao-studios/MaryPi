# thread — Swift twin: [Thread](https://github.com/rao-studios/Thread)

`threadd`, MaryOS's memory — the hard drive's own record of everything on it — and `threadctl`.
One SQLite file, `/var/lib/thread/thread.db`, holds every owner's documents, their chunks and
embeddings, the knowledge graph, the groups, the file rows that prove parity with the disk, the
enrichment queue and a ledger of every deposit, search and mutation. Back the file up and the whole
memory moves with it.

threadd serves two sockets, both with the owner taken from the kernel's peer credentials:

- `/run/thread/thread.sock` — Conduit's `thread.v1` over gRPC, byte for byte the Mac's wire and all of
  it: `ThreadQuery` (Index, Search, Remove), `ThreadLibrary` (Library, Documents, ExportCorpus),
  `ThreadGraph` (Query), `ThreadUpdate` (UpdateGroup, UpdateDocument, Stats), with the semantics of
  Thread's handlers (`Sources/Conduit/*ServiceImpl.swift`).
- `/run/thread/local.sock` — newline JSON, the MaryOS-only ops (`thread/local.h`): `deposit`, `search`
  with `lanes[]`, `graph` and `graph.mutate`, `schemas`, `ledger`, `parity` and `parity.report`,
  `file.record` / `file.move` / `file.remove`, `policy`, `backup`. The desktop's Thread app, indexd and
  `threadctl` speak this.

One caller is trusted the way Sewn is the Mac's mothership: the `sewn` user may name the `owner_id` it
writes for, so auto-memory lands in the user's own groups. Everyone else is the owner their uid names.

## What Thread does, in C

| Thread (Swift) | here |
|---|---|
| `Database/PartitionTable` (chunks, embeddings, search) | `partitions` table + `vectors.c` (a float32 arena, exact scan, Thread's sub-vector distance) + `search.c` |
| `Database/GraphStore` (entities, relationships, provenance, repair) | `entities`, `relationships`, `predicates` + `graph.c` — the same ids (`numericHash` of `kind|name`, `subject|predicate|object`), the same upsert/detach counting |
| `Database/Utilities/TextChunker`, `TagGenerator`, `computeHash` | `text.c` |
| `Database/Graph/ExtractionPolicy`, `GraphExtractionParser`, `GraphEnrichment` | `graph.c` (policy, parser, co-mention edges, hub cap) + `enrich.c` (a worker over the `jobs` table: embed → extract → graph, revision-guarded, backoff) |
| `MistralEmbeddingProvider`, `MistralGraphExtractionProvider` | `embedder.c` — asks **sewnd** (`embed`, `graph.extract`); threadd itself never touches the network |
| `ThreadRegistry` (plists on disk) | `thread.db` (WAL; `PRAGMA user_version`), `backup` = `VACUUM INTO` |

Beyond Thread, for MaryOS: `families.c` declares the record families (file, memory, behavior, style,
routing) with their id and group patterns and the **lane** each belongs to — `personal` (file, memory,
style) and `behavioral` (behavior, routing) — so a search can be constrained to the lanes a purpose is
allowed to draw on; `parity.c` keeps the `files` rows indexd reports and answers what is missing, stale
or orphaned; `ledger.c` keeps the last 10,000 events. The Thread keeps what Mary learns: the files on the
drive, what Sewn remembered of the conversation, what Mary did and how the person asks. It does not keep
what she already knows as the operating system — the applications' functions are the registry's, read
from the desktop's `skills{apps}` — nor a record per turn: the `conversation`, `interaction`, `ability`,
`ability-schema` and `application` families were retired (`user_version` 2), and a file from before is
cleaned at open, through the same path Remove takes, with one ledger row saying how many went.

Mary's own calls into it (`MaryThread/ThreadDirectClient.swift`) are `thread/client.h` in this same
package — the Thread is MaryOS's hard drive, not a service Mary is a client of: `thread_turn_document_id`
mints a turn's request id as the Mac did, and `thread_client_library` / `thread_client_documents` read
back. Peering with other machines is Gita's (`gita/peer.h`).

A store that was JSON files (the first MaryOS release) is imported on the first open — its
`mary-conversations` stay behind, retired — and the old directories are renamed to `legacy/`.

Deviations (PORTING.md 7 and 11): SQLite instead of property lists; exact vector search instead of product
quantization; embeddings and extraction through sewnd; `owner_id` from credentials, never the request, except
for the `sewn` user; a unix socket instead of TCP 9090; no registration with Sewn; `top_k` honoured (Thread
ignores it).

```
threadctl stats | schemas | parity | policy | drain | checkpoint
threadctl library [--limit N] [--after GROUP-ID]
threadctl documents ID...
threadctl search [--lane LANE]... [--group GROUP]... [--top N] TEXT...
threadctl deposit [--group G] [--label T] [--family F] [--document ID] [--chunk] TEXT...
threadctl graph [--entity NAME] [--query TEXT] [--hops N] [--limit N] [--documents]
threadctl ledger [--limit N] [--kind KIND] [--document ID]
threadctl file PATH-OR-ID
threadctl backup PATH
```

Status: working. Prefix `thread_`. Needs libnghttp2, libprotobuf-c and libsqlite3.
