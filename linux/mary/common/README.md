# common — no Swift twin

The C plumbing every other package leans on, and that Swift gets from Foundation and swift-nio:
JSON through json-c, a streaming Server-Sent Events parser, base64, length-prefixed frames over a
socket, `mc_secure_zero` for secrets, and one log call.

Status: planned (commit 2 of the plan). Prefix `mc_`.
