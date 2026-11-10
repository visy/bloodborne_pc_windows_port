# changes.d: changelog fragments

Do not edit the top section of CHANGES.md; that conflicts on every merge. Add ONE file per change instead:
`changes.d/<slug>.md` (slug = your agent / branch name or a short topic).

- The file holds one or more sections `### Added`, `### Changed`, `### Fixed` (only those three), each with
  markdown bullets in the CHANGES.md style: `- **bold lead** (credit, ids) player-facing text.`
- A bullet may continue on following lines; no merge-conflict markers, UTF-8, no player names / IPs / logs.
- `python tools/release/changelog.py check` validates, `preview` prints the assembled section.
- At release time `python tools/release/changelog.py assemble X.Y.Z --date YYYY-MM-DD` moves the entries into
  CHANGES.md (after the entries already there, fragments sorted by file name) and deletes the fragments.
