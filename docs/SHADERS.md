# Shader compilation and the shader cache

Back to the [README](../README.md).

A PC GPU needs every shader combination compiled into a pipeline before it can draw it, which is where first-time stutter comes from. The port records each pipeline the game uses in `user\cache\CUSA03173\` and:
- **Precompiles the cache before the intro** (`BB_SHADER_PRECOMPILE`, default on): all cores build every cached pipeline before the logos; a progress screen appears when this takes longer than a moment (first launch, a new GPU, a grown cache). Later launches reuse the driver's own disk cache and are quick. `Esc` quits during the screen.
- **Async shader compilation** (`BB_ASYNC_SHADERS`, upstream's name `BB_ASYNC_PIPELINES` works too; default on since 0.5-pre4, can be switched live in the overlay): a new pipeline of a pass whose render targets were drawn to in each of the last 8 frames is built in the background and its draws are skipped for a frame or two instead of stopping the game. Compute work, frame-defining and screen-space passes, loading screens and one-off render targets are never skipped. `BB_ASYNC_SHADERS_POLICY=balanced` uses the looser 0.3.x rule (targets in 2 of the last 3 frames); `BB_ASYNC_PIPELINE_THREADS` / `BB_SHADER_THREADS` set the worker count. `BB_FRAME_STATS=1` logs `GPU: N pipelines compiled in the background, M draws went without theirs` and any wait over 50 ms.
- **Graphics pipeline library** (`BB_GPL`, default off, restart needed): builds pipeline parts separately and links them quickly, with an optimized relink in the background; needs `VK_EXT_graphics_pipeline_library` with fast linking (falls back to whole pipelines otherwise).
- **Compile indicator** (`BB_COMPILE_INDICATOR`, default on): "Compiling shaders: NN%" in a corner, only on loading screens and menus (frames without the game's 3D G-buffer pass), never during gameplay.

Shaders the game has never used on any machine cannot be prepared up front: a pipeline depends on draw state the game only sets while running. A shared cache from a player who visited more areas covers them.

## Sharing the shader cache

The launcher's **Export shader cache...** saves the cache as one `.zip`; **Import shader cache...** merges one into yours without overwriting anything (GPU-specific files are taken only when they were made for the same GPU; the rest is rebuilt before the intro). Caches from several players can be combined.

The pipeline cache in `user\cache\CUSA03173\` can be copied as-is to another PC, also one with a different GPU. Next to the compiled shaders it keeps each shader's source (`.src` files: the game's shader code and the inputs of its translation). When the cache was made for another GPU (or by a build with a changed shader translator), the first launch translates every shader again from these sources for the current GPU ("Shader cache: rebuilding N shaders for this GPU..." in the log) instead of throwing the cache away. `BB_SHADER_CACHE_REBUILD=1` forces this rebuild, `BB_SHADER_CACHE_REBUILD_THREADS=N` sets its thread count. `BB_SHADER_CACHE_SELFTEST=1` translates each newly compiled shader a second time from its stored source and logs any difference (a developer check). The cache contains data derived from the game's shaders: share it only with people who own the game, and never commit it to this repository.
