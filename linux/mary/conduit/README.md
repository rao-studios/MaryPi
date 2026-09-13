# conduit — Swift twin: [Conduit](https://github.com/rao-studios/Conduit) (`Protos/`, `Sources/Conduit`)

The wire between Thread nodes.

- `protos/thread.proto` and `protos/fleet.proto` are verbatim copies of Conduit's. `make gen` compiles them
  with protobuf-c into `include/conduit/*.pb-c.h` and `src/*.pb-c.c`, which are committed; `make gen-check`
  fails when either the generated code or the copies (against a sibling Conduit checkout) are stale.
- `conduit/grpc.h` serves and calls gRPC's unary methods over HTTP/2 with nghttp2 — the same bytes grpc-swift
  sends, checked against Python's grpcio in the tests: `:path` routing, length-prefixed messages capped at
  4 MiB, and `grpc-status` / `grpc-message` trailers (trailers alone for a call that fails before a message).

Deviations: HTTP/2 in the clear over unix sockets on one machine (Conduit's transports are plaintext TCP);
mutual TLS is declared for peering in `thread/peer.h`, not built. Unary calls only: the streaming `Session`
and `Train` RPCs answer UNIMPLEMENTED. Conduit's session manager, registration client and mothership server
are not ported.

Status: working. Prefix `conduit_`. Needs libnghttp2 and libprotobuf-c (and protoc-c for `make gen`).
