# mary-thread — Swift twin: `Mary/Sources/MaryThread`

Mary's client for the local Thread node, the calls `ThreadDirectClient.swift` makes, over Conduit's gRPC
on threadd's unix socket (`$THREAD_SOCKET`, else `/run/thread/thread.sock`), one connection per call.

- `mt_deposit_turn` — `deposit`: each turn becomes one document in the `mary-conversations` group
  (scope `personal`), id `mary-turn-<started ms>-<4 hex>`, texts `[question, reply]`, and metadata
  `{source, model, started_ms, ended_ms, cancelled}`. maryd deposits every turn, typed or spoken, after
  it ends — a cancelled one too, with what was said by then.
- `mt_library` — `library`, and `mt_documents` — `documents(ids:)`.

threadd takes the owner from the socket's peer credentials; `owner_id` is sent only to match Swift's wire.
Search, removal, the graph and the Fleet client are not ported yet.

Working. Tested against a fake threadd serving the three routes. Prefix `mt_`.
