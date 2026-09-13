# thread — Swift twin: [Thread](https://github.com/rao-studios/Thread)

`threadd`, MaryOS's memory, and `threadctl`. threadd serves thread.v1's `ThreadQuery/Index`,
`ThreadLibrary/Library` and `ThreadLibrary/Documents` over conduit's gRPC on `/run/thread/thread.sock`,
with the semantics of Thread's own handlers (`Sources/Conduit/ThreadQueryServiceImpl.swift`,
`ThreadLibraryServiceImpl.swift`): documents keyed by the caller's ids and joined to groups, groups paged by
id with `after_id` / `limit` / `has_more` or found by `document_ids`, contents returned in request order.
Every other method answers UNIMPLEMENTED.

The store (`thread/store.h`) is private JSON files under `/var/lib/thread`: `node-id`, `groups/<id>.json`,
`documents/<id>.json`. The owner of everything a connection reads and writes is its caller's login name,
from the kernel's peer credentials; nobody can replace or read another owner's documents, and ids must be
safe file names.

Mary's own calls into it (`MaryThread/ThreadDirectClient.swift`) are `thread/client.h` in this same
package — the Thread is MaryOS's hard drive, not a service Mary is a client of: `thread_client_deposit_turn`
puts each spoken or typed turn in the `mary-conversations` group (id `mary-turn-<started ms>-<4 hex>`, texts
`[question, reply]`, metadata `{source, model, started_ms, ended_ms, cancelled}`), and `thread_client_library`
/ `thread_client_documents` read back. Peering with other machines is Gita's (`gita/peer.h`).

Deviations (PORTING.md 7): JSON files instead of property lists; no embeddings, product quantization,
search or knowledge graph yet; `owner_id` from credentials, never the request; a unix socket instead of TCP
9090; no registration with Sewn.

Status: working. Prefix `thread_`. Needs libnghttp2 and libprotobuf-c.
