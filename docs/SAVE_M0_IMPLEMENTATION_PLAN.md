# M0 implementation map (2026-10-10)

Source: [cross-device saves RFC v0.4](novelty_sync_rfc_v0.4.md) and its [handoff](novelty_codex_sync_handoff.md). This plan covers local save discovery and management only. LAN pairing/sync and game transfer follow after M0 validation. Server, accounts and WAN work are outside the MVP.

## Repository findings

- C++20/Qt 6 Widgets and SQLite. `src/core.h` defines a `Game` with an installation UUID; `src/core.cpp` stores its JSON in `games`, plus per-library settings. `<Games>/.novelty/prefixes/<installation UUID>` is the per-game Proton prefix; `<Games>/.novelty/installations/<installation UUID>` holds operation state.
- `src/launch.cpp::supervise` runs detached from the window, holds `launch.lock`, and waits for the tracked umu/game process to finish. It is the right place for bounded before/after observation and post-exit capture. `src/runtime.cpp` handles process/crash recovery.
- `src/core.cpp::inspect` has positive Ren'Py runtime signals; XP3 and Siglus labels are candidates only. `src/profiles.cpp` has versioned *setup* profiles. `src/setup.cpp` writes `setup.json` and `recipe.lock.json`; these are prefix setup receipts, not declarative game save recipes. A separate trusted save `recipe.json` format and location are needed.
- `src/window.cpp::manage` contains per-game settings. A new Saves tab and `src/saves.cpp` have preliminary manual mappings, snapshots, restore and pending work. Their schema/behavior need revision below before M0 is complete.
- `tests/core_tests.cpp` covers local storage/launch recovery; `tests/ui_tests.cpp` covers Manage and detached launch. Local metadata TCP fixture cannot bind in this sandbox, so its test must run in an environment that permits localhost sockets.

## Data and behavior changes

1. Extend `saves.json` to a versioned record with a profile/library UUID, a stable profile-game UUID distinct from the local installation UUID, edition and save-compatibility identities, portable save roots, `searching|identified|unidentified`, evidence/candidates, and explicit/manual override provenance. Keep existing schema-1 mappings readable and migrate without moving saves.
2. Resolve roots through validated `GAME_DIR`, `PREFIX_APPDATA`, `PREFIX_DOCUMENTS` and a guarded prefix-relative escape hatch for advanced paths. Never export absolute device paths. Keep several non-overlapping roots. A trusted `recipe.json` lives in Novelty-managed data and must match an exact game/edition identity; never load a script or accept a recipe just because a game folder contains it.
3. Add conservative Ren'Py rules using verified local evidence. Advisory engine labels only produce candidates. During `searching`, record bounded path/size/mtime metadata for likely game and current-prefix save locations before every real launch, then compare after true exit. Reject logs/config/cache-only signals; preserve ambiguous candidates for confirmation. Three or any number of save-free launches remain `searching`. Manual path confirmation can immediately identify; only an explicit user action selects `unidentified`; Rescan re-enters `searching`.
4. Make snapshots immutable and idempotent: no snapshot when there is no saved data, no new snapshot when mapped content equals the latest one, and manifest hashes plus parent snapshot ID, author/profile/device identity and edition/compatibility fields when content changes. Preserve existing schema-1 local snapshots. Restore validates all input before touching originals, keeps a verified backup and a recovery journal, and translates symbolic roots into the current installation. Add an explicit import/restore path for a fresh installation with matching identity/edition. Keep game launch responsive and allow offline play while unknown discovery stays honest.
5. Put automatic observation and post-exit capture in the detached supervisor, with durable pending state. On reopen/Rescan, retry unfinished safe work. The UI shows `Searching`, `Identified — no saved data yet`, `Local snapshot`, `Unidentified`, and actionable failures; no sync-success wording before LAN work exists.

## Risks and checks

- Save directories can change during copying even after the game exits, and a Wine process can outlive its wrapper. Continue using the tracked process barrier and verify copied hashes; test interrupted staging and rollback.
- Multiple profiles under one OS login are not yet implemented. For M0, use one profile identity per Games library and retain it across moves. Distinct Games libraries have distinct local data roots. Do not claim a security boundary between profiles sharing an OS account.
- No verified game-specific save recipe exists in the current catalog. Test exact-match declarative fixture recipes, and ship real recipes only after a game/save-load and relocation check.
- Current preliminary snapshots create duplicates and include empty sets; current `prefix` mappings can encode a machine-specific Wine user path. These are known gaps, not completed M0 behavior.
- The first pass excludes registry progress, external save databases, full-install scans, process tracing, real-time watchers, networking and server scaffolding.

## Implementation slices and evidence

1. Fix preliminary snapshot/recovery tests, then add schema migration, identities, portable root translation and explicit discovery state. Test migration, symlink/path rejection, multi-root mappings, sticky states and empty known roots.
2. Add exact-match trusted recipe and engine rule resolution. Use a small fixture game/recipe and a Ren'Py fixture with an empty save directory. Test wrong-edition refusal and relocated roots.
3. Add bounded launch/exit observation with a scripted fake game: three launches without saves, save on fourth; log/config-only changes; multiple ambiguous locations; terminal-state skip and Rescan.
4. Add idempotent, ancestry-aware snapshots and fresh-install restore. Test repeated capture, independent content changes, interrupted copy/restore, unavailable paths, and a changed installation/prefix location. The device game must then load the restored progress before M0 acceptance is claimed.
5. Exercise Manage controls and detached-window behavior, rerun the socket-dependent UI suite where allowed, and perform real-game save/load/relocation acceptance. Only then begin M1 LAN sync.
