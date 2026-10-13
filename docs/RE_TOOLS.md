# Reverse-engineering lookups (Ghidra)

Starting headless Ghidra costs 30-60 s (JVM + project open). Look things up in this order:

1. **Grep the text export** in `E:\games\bbport\deps\ghidra-export\` (instant, no Ghidra):

   | file | columns |
   |---|---|
   | `functions.tsv` | entry, name, size, body_end |
   | `strings.tsv` | address, type, string (tabs/newlines escaped) |
   | `xrefs_to_strings.tsv` | string_addr, from_addr, func_entry, func_name, string (first 120 chars) |
   | `calls.tsv` | caller_entry, caller_name, call_site, callee_entry, callee_name, kind |

   ```
   grep -i "summon" /e/games/bbport/deps/ghidra-export/xrefs_to_strings.tsv
   grep -P "^0x16439b0\t" /e/games/bbport/deps/ghidra-export/functions.tsv
   ```

2. **Decompile through the cache**: `python tools/agent/ghidra_decomp.py ADDR [ADDR...]` prints
   `E:\games\bbport\deps\ghidra-cache\0x<addr>.c` per address (function containing it, with a
   callers/callees header). Cached addresses return instantly; all missing ones are decompiled in
   ONE headless run (default timeout 900 s). `--cat` prints the C, `--force` re-decompiles.
   Batch everything you need into one call.

3. `--callers ADDR` / `--callees ADDR` answer from `calls.tsv` without Ghidra (ADDR must be a
   function entry; callers of a mid-function address: look up its entry in `functions.tsv`).

Addresses are our guest offsets (raw ELF VA of `eboot.elf`; bbhost addresses minus 0x400000).

The tool opens the agent's Ghidra project copy (`BB_GHIDRA_PROJECT`, from
`agent_env.py create NAME --ghidra`; read from the agent's `env.txt` when the variable is unset).
Never open `deps\ghidra-proj` directly (the tool refuses). The export and cache are derived from the
game binary: they stay under `deps\`, never in git.

Rebuild the export (only after the shared project was re-analysed):
`python tools/agent/ghidra_decomp.py --export`. Free-form queries still work with
`tools/re/refs.py` and `tools/re/decomp.py` (uncached).
