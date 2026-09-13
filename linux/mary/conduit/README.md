# conduit — Swift twin: [Conduit](https://github.com/rao-studios/Conduit) (`Protos/`, `Sources/Conduit`)

The wire between Thread nodes. `protos/thread.proto` and `protos/fleet.proto` are verbatim copies of
Conduit's, compiled with protobuf-c (the generated files are committed; `make gen-check` compares the
copies with a sibling Conduit checkout). Calls are unary gRPC over nghttp2 — HTTP/2 without TLS on a
unix socket for now, the same bytes grpc-swift speaks, so a Swift Thread node can one day be a peer.

Deviation: plaintext over a unix socket; Conduit's own transports are plaintext TCP. mTLS is declared
in `thread/peer.h`, not built.

Status: planned (commit 8). Prefix `conduit_`. Needs libnghttp2 and libprotobuf-c.
