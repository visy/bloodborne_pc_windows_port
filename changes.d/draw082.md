### Fixed

- **Vertex inputs with an empty descriptor** (Jgamrot/yharnam-pc d1d8810) an attribute whose vertex buffer descriptor is all zero is now read as zero through a valid float input instead of an undefined vertex format, which could lose the graphics device on some NVIDIA drivers. `BB_NULL_VERTEX_INPUT=0` restores the old behaviour.
- **Stretched shapes after a rewritten vertex table** (Jgamrot/yharnam-pc 54b2ca5) a draw prepared ahead of time is checked against the vertex buffer descriptors the draw really uses and reads them again when the game changed them in between. `BB_STALE_VSHARP_CHECK=0` turns the check off.
- **Shader reuse** (Jgamrot/yharnam-pc 17edac0, ayozetr 8d36a25b) a vertex shader that only uses the engine's own buffers is reused only when its bindings match the pipeline layout. `BB_SPEC_REUSE_BINDINGS=0` restores the old shortcut.
- **Texture tiling** (Jgamrot/yharnam-pc c993ce0) a tiling dispatch with no pipeline or no work is skipped.
