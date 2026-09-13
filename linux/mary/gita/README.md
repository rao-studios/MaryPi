# gita — Swift twin: `Sewn/Sources/Gita`

Gita decides whose words shaped an answer. On the Mac it lives inside Sewn: royalty shares at search time,
`[[n]]` citation markers resolved into character spans after generation, and pricing against the turn's
token ledger. On MaryOS the span attribution is part of sewnd's turn (`sewn/attribution.h`, so a highlight
in Spotlight names the record it came from); this package is Gita's other job, for the milestone that
builds it — **retrieval by other machines**: a peer Thread node asking this hard drive for what it holds,
and this one asking them.

- `gita/peer.h` — discovery (mDNS `_thread._tcp`), pairing over mutual TLS with node ids proven by
  certificate keys, sync over conduit. Every call answers `-ENOSYS` until then.

Royalties, the wallet and pricing are not ported. Status: **declared**. Prefix `gita_`.
