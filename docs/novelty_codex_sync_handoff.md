# Codex handoff — Novelty cross-device saves (M0 first)

**Reference:** `novelty_sync_rfc_v0.4.md` (source of truth for scope and product behavior).
**Status:** Proposed handoff, ready to supply to Codex; NOT an instruction to implement all milestones now.

## Your first task: inspect and plan, do not code yet

Inspect the existing Novelty repository before making architectural assumptions. Identify the actual language/framework, directory structure, library/profile/game/installation IDs, launch/exit lifecycle, Proton prefix management, persistence, engine-detection and `recipe.json` structure, current game-manage UI, and test setup.

Return a repository-grounded **M0 implementation plan** with precise module/file touchpoints, risk/edge cases, data/schema changes, migration needs, estimated implementation slices, and tests. Flag discrepancies between the RFC and existing architecture. Do not silently introduce new dependencies. Wait for approval before writing implementation code.

## Product goal

Novelty is an offline-first game lifecycle manager (VN-first, generic Proton support). A user points it at a game, presses Play, and can later resume on a different device without manually figuring out save locations. The complete sync MVP is M0 local saves, M1 accountless LAN save sync, M2 optional LAN game-file transfer. **Server, accounts, login, WAN sync, relay/NAT traversal, server-hosted game files: post-MVP, not to be implemented or scaffolded.**

## M0: Save Discovery & Management (the immediate implementation target after approval)

### Discovery sources — include all four, in this order

1. **Game-specific declarative `recipe.json` paths**, keyed safely to game/edition when possible; no remotely supplied executable script hooks.
2. **Engine-specific save-root rules**, reusing Novelty's existing engine recognition. Treat weak engine detection as a suggestion rather than proof.
3. **Manual, editable save roots**, supporting several locations for the same game; validated portable paths.
4. **Lightweight before/after filesystem comparison across successive launches**, constrained to likely save roots in the game directory and *that game's* Proton prefix. Baseline metadata prior to launch, evaluate after the true game process exits. Compare path/size/mtime before computing hashes for snapshots.

### Persistent discovery lifecycle

- `searching`: Observe on **every launch until an answer**, not only on first launch, with **no fixed attempt limit**. Three launches to check resolution/fonts should not end discovery; a new save on a fourth launch can identify it.
- `identified`: Reliable portable save root(s) are known; stop observing. A known path can be empty: distinguish **location identified** from **saved data exists**.
- `unidentified`: Explicit user decision to stop searching with no reliable path. Never infer it merely from no writes/too many launches.
- Sticky terminal states across restarts; **Rescan** returns to `searching`. Manually choosing a valid path can immediately identify it.
- Ambiguous candidate locations remain `searching` and are shown for manual confirmation; don't automatically label config/log/cache files as saves. Never block Play or falsely claim saves are synchronized.

### Snapshot/restore requirement

Store paths portably (`GAME_DIR`, `PREFIX_APPDATA`, `PREFIX_DOCUMENTS` or equivalent), never synchronize machine-specific absolute paths or full Proton prefixes. Support multiple save roots, immutable local snapshots with manifest/hashes/history, atomic-ish restore with rollback, idempotent no-change operations. Treat a save as truly handled only after **Discover → Snapshot → Restore → Load** is proven on a relocated/fresh prefix or installation. Prevent unsafe path escape/symlink traversal.

### Explicitly NOT in M0

- Ludusavi/external save-location manifest integration.
- inotify or other real-time filesystem watchers / mandatory background daemon.
- Large heuristic/full-install recursive scans, process tracing, `strace`, arbitrary filesystem scanning.
- Numeric confidence-score machinery.
- Wine registry save extraction/restore.
- LAN transport, pairing, server auth, WAN sync, or game-transfer features (those are later milestones).

### Acceptance criteria for M0

1. Engine preset locates a save root; if no save exists, UI says so without reporting false sync success.
2. Recipe-provided save root works and survives a different game installation/prefix path.
3. After three save-free launches state remains `searching`; a save introduced on the fourth can be found.
4. `identified` and user-chosen `unidentified` stop further observation; explicit Rescan works across application restarts.
5. Logs/config/cache changes alone do not auto-confirm a save root; ambiguous locations are presented for manual confirmation.
6. Manual override can define several valid roots and can be changed later.
7. Discover/snapshot/restore works in a fresh location, and repeated no-change snapshots/sync requests are idempotent.
8. Normal game launch stays lightweight; observation is bounded; no network account or service required.
9. Wrong-edition data, unsafe paths and symlink escapes are rejected or require safe, explicit recovery.

## Subsequent milestones — separate future tasks

**M1:** LAN discovery, vetted cryptographic accountless pairing (optional Gelbooru-tag human verification), authenticated snapshot transfer, multi-profile isolation, conflicts, recovery. Do not start before M0 is working.

**M2:** On-demand LAN game-folder transfers, no Proton-prefix copying, integrity checks, local setup, restoration of saves. Optional convenience, not required to prove LAN save sync.

**After MVP:** Optional self-hosted accounts and HTTPS WAN save storage. Server must not manage LAN P2P or host game installations.

## Expected first reply from Codex

Produce only: (a) repository findings with file paths, (b) short discrepancy/risk list, (c) proposed M0 file/module changes and data structures, (d) test plan with sample fixture games, (e) scoped incremental implementation sequence and questions that truly block work. **Do not write code until plan is reviewed.**
