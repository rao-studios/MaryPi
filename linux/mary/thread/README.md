# thread — Swift twin: [Thread](https://github.com/rao-studios/Thread)

`threadd`, MaryOS's memory, and `threadctl`. Serves `thread.v1.ThreadQuery/Index`,
`ThreadLibrary/Library` and `ThreadLibrary/Documents` over conduit; every other RPC answers
UNIMPLEMENTED. Documents are JSON files under `/var/lib/thread/documents/<group>/`. `peer.h` declares
discovery (mDNS) and pairing (mTLS) with other MaryOS machines; both return `-ENOSYS` for now.

Deviations: JSON files rather than property lists; no embeddings, product quantization or graph yet;
`owner_id` comes from the caller's credentials, never from the request.

Status: planned (commit 9). Prefix `thread_`.
