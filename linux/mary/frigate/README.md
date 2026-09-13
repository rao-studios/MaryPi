# frigate — Swift twin: [Frigate](https://github.com/rao-studios/Frigate)

On-device models: embeddings (`FrigateEmbedder`), a small LLM (`FrigateLLM`) and tree-ensemble
inference (`FrigateBoost`), on MLX. MaryOS uses Mistral through `sewnd` for all of this in the first
milestone; Frigate's on-device solutions are ported later, and these headers are the seam they fill.
Frigate vendors mlx-c, which is already a C API, so a Linux build could back `embedder.h` and `llm.h`
directly.

Status: **skeleton**. Every constructor answers `ENOSYS` and `frigate_available()` is false. Prefix
`frigate_`.
