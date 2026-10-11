# Novelty — Cross-device Saves & LAN Pairing

**Status:** Draft RFC v0.4 — save-discovery MVP scope defined for Codex handoff  
**Scope:** Game lifecycle / cross-device continuity  
**Target:** Linux desktop, Steam Deck, and other supported Novelty devices  
**Principle:** *Add a game, press Play, resume elsewhere. The networking should disappear into the background.*  
**Delivery boundary:** **MVP = local save management + accountless LAN sync + optional LAN game transfer.** Self-hosted server, login, WAN sync, and account-assisted pairing are **post-MVP only**, with no implementation work required during MVP.

## 1. Product goal

Novelty is an offline-first game lifecycle manager, initially optimized for visual novels but capable of launching arbitrary games through Proton. Its value is not another launcher UI: it removes the need to coordinate launchers, Wine/Proton tools, save managers, and file-transfer apps.

For the **MVP**, Novelty supports local save snapshots and **LAN P2P**: accountless discovery, trusted device pairing, save sync, and optional on-demand game-folder transfer between locally reachable peers.

A **self-hosted server** may be added **after the MVP** for username/password login, durable user-scoped save storage, and WAN save synchronization. That future system must reuse the same save/snapshot model rather than introduce a separate conflict engine. No server account, service API, or WAN transport is required to ship the MVP.

Novelty must remain fully usable offline, without an account, and without a running server.

## 2. Scope boundaries

### In scope for MVP (M0–M2)

- Local user/profile identity and isolated library, saves, prefixes, and peer credentials.
- MVP save discovery using game recipes, engine-specific rules, manual overrides, and bounded across-launch filesystem comparisons. Discovery stops only on `identified` or explicitly `unidentified`.
- Consistent local save snapshots, history, conflict handling, and manual **Sync Saves**.
- Same-LAN discovery, cryptographic device pairing, trust/revoke, and direct peer transfers.
- LAN transfer of a game installation from an authorized peer **only when requested**.
- Status that clearly distinguishes local saves, pending LAN transfer, synchronized, and conflict.
- Simple UI in normal usage; advanced controls discoverable rather than mandatory.

### Deferred until after MVP

- Optional self-hosted account service with user/password login and session management.
- WAN save upload/download and server-side snapshot history.
- Same-account device registration or account-assisted LAN pairing.
- Server deployment, storage, API contracts, retention policies, and cloud-related UI.

### Explicitly out of scope

- Storefront, game purchases, achievements, friends/chat, streaming, remote play.
- WAN P2P game transfer, NAT traversal, TURN/relay service, global peer discovery.
- Server-hosted game binaries or game distribution, including in the post-MVP server concept.
- Mandatory background daemon, mandatory homelab, public Novelty cloud service.
- Automatically sharing libraries or save data between different users.
- Blockchain, distributed consensus, seed phrases as a wallet, or custom cryptographic primitives.

## 3. Essential concepts and boundaries

| Entity | Meaning | Important rule |
|---|---|---|
| **Local profile** | A person's Novelty library and progress on a machine | Isolation boundary for games, saves, prefixes, and trusted peers |
| **Device** | One Novelty installation on one machine | Has a persistent cryptographic identity **per profile/device** |
| **Game** | Stable library entry identifying a title/edition | ID must survive path changes and transfers; VNDB/release IDs are metadata, not sole authority |
| **Installation** | Device-local directory and runnable configuration | Never sync Proton prefixes or absolute machine paths |
| **Save set** | Declared/learned save roots for one game | May include paths inside game directory, Wine prefix, Documents, AppData, etc. |
| **Snapshot** | Immutable manifest + contents of a consistent save set | Carries parent ID(s), hashes, author device, and timestamps |
| **Transport** | A way to exchange snapshots | LAN uses the save model in MVP; a future server adapter must reuse it |

P2P uses public-key device identities, with trust approved per local profile. A future server login token would be a secret credential, **not** a peer ID, and must never be advertised on the LAN.

**Isolation requirement:** When two household members use Novelty, even on the same physical computer, their local profiles must have separate libraries, save namespaces, trusted peers, prefixes, and writable game directories. Post-MVP server accounts will also need strict separation. Two profiles must never auto-sync merely because they share a LAN or device. Within a shared OS login, profile separation is organizational, **not** a security boundary against someone who can read that OS user's files; separate OS accounts give stronger local protection.

## 4. Intended user experience

### A. Normal play on one device

1. User adds a game by selecting a directory/executable. Novelty fetches metadata, resolves an executable (ask only if ambiguous), applies a generic/engine/recipe preset, and tries to run it.
2. Novelty discovers save paths using engine rules, a game recipe, or a manual override. Unknown paths never silently imply that save sync is working.
3. On **Play**, Novelty checks for newer accessible snapshots, restores if safe, and launches. Network failure must not prevent offline play; show a modest warning if saves may be outdated.
4. On game exit, Novelty waits for the actual game process to finish, captures changed saves, retains a local snapshot, and queues synchronization. A user should not have to run an external app.

### B. Accountless LAN pairing

1. User opens **Settings → Devices → Pair device**. Novelty discovers nearby candidates via mDNS or equivalent local discovery. Discovery reveals no library or save metadata.
2. User selects a peer. Each displays a short, generated **Gelbooru-tag verification phrase** derived from an authenticated pairing handshake, e.g. `cat_ears · headpat · maid · hand_holding` (illustrative only).
3. User checks that the phrases agree and approves on both ends. Use a vetted authenticated key-exchange / short-authentication-string approach; **do not** treat a user-chosen four-tag phrase as a permanent password or invent crypto.
4. The profiles remember each other's device public keys. Future LAN connections authenticate automatically. Either side can revoke trust.
5. Pairing authorizes access only to the selected profile's data, not other profiles on either machine.

A separate short-lived *enter-code-on-other-device* flow can be evaluated later, provided its security properties are sound. The decorative tag dictionary must be stable and versioned; its changing live tag database is not a cryptographic primitive.

### C. Play on a second LAN device

1. The paired device sees available game entries and their install state; local games remain usable without peer availability.
2. If game files already exist, user links the local directory instead of transferring them.
3. If an authorized LAN peer has the game, user may choose **Install from peer**. Novelty transfers the game folder, verifies integrity, creates a local installation and device-specific prefix/configuration, and restores the newest eligible save snapshot.
4. If no peer is available, show **Locate game**. No WAN game-download or server-install fallback.
5. If game files are present and saves are synchronized, **Play** is one click.

LAN game transfer must **not** copy another device's Proton prefix. Known in-game save files should be excluded from the game-folder payload and restored from the save snapshot path to avoid stale/mixed saves. Reject path traversal and unsafe symlinks; stage and verify transfers before making installations visible.

### D. Two users in one household

- User A and User B each have an independent Novelty profile (prefer OS-specific data roots). A future server may associate different accounts with them.
- The same physical machine may host both profiles, but no save/game/library visibility crosses the profile boundary without an explicit future sharing feature.
- To prevent writable game-directory leakage, default to **separate writable installations per profile**. If importing an existing shared folder, detect/warn about overlap; read-only shared assets with separate writable overlays are future optimization, not an MVP requirement.
- Revoking a peer does not delete the user's local games/saves. Future server logout must follow the same principle.

## 5. Save engine (MVP foundation, transport-neutral)

**The save engine is authoritative for versioning; the LAN module only moves verified data.** Design its interfaces so a future server transport can reuse snapshots, but do not implement server plumbing in MVP.

- **Discovery:** For MVP, save roots come from game recipes, engine-specific rules, manually editable per-game paths, and lightweight filesystem comparisons across successive launches. External save-location databases are deferred. Use symbolic roots (e.g. `GAME_DIR`, `PREFIX_APPDATA`, `PREFIX_DOCUMENTS`), not absolute paths from another machine. Multiple roots are supported. Discovery is *not* limited to the first launch; see the persistent discovery lifecycle below.

- **Capture:** Record save sets only at safe points (primarily game exit, manual sync when game is not running). Make immutable snapshots using per-file content hashes and a manifest. Do not blindly sync caches or entire prefixes.
- **Restore:** Translate symbolic paths to each device's paths. Stage/validate before replacing files, with rollback available. Never restore a snapshot from a different game edition without an explicit user decision.
- **History:** Preserve at least a configurable small history; users can inspect timestamp/device and restore a previous snapshot.
- **Divergence:** Detect ancestry, not merely newest filesystem mtime. If desktop and Deck independently advanced from the same parent, preserve both and prompt the user; **no silent last-writer-wins**. Snapshot IDs should be stable and duplicate-safe; this also prepares for an eventual second transport without building it now.
- **Idempotence:** Pressing **Sync Saves** 50 times with no changes does not generate 50 snapshots or corrupt saves. Show a clear *checked just now / up to date* result.
- **Durability:** Store queued upload/download work so a transient failure doesn't discard progress. If Novelty is shutting down with pending sync, finish if quick or clearly report that pending work will resume on next launch. A background service may be added later; it is not mandatory.
- **Conflict with running games:** Do not overwrite in-use save files. Defer restore, or ask the user to close the game.

### Save discovery — committed MVP implementation (M0)

The goal is *automatic where reliable, conservative otherwise*. Implement these four discovery sources in priority order:

1. **Game recipe (`recipe.json`):** A trusted, declarative game/edition-specific portable save-root definition. Do not execute arbitrary recipe scripts.
2. **Engine preset:** Well-understood engine-specific patterns (start with engines already detected by Novelty); resolve paths relative to the current installation or Proton prefix. Treat an engine guess as a candidate unless its save-root rule is sufficiently reliable.
3. **Manual save roots:** User can add/edit/remove one or more portable save-root paths. Manual confirmation overrides uncertain automatic guesses.
4. **Bounded filesystem comparison across launches:** While searching, capture lightweight before/after metadata for likely save locations; evaluate changes after the *actual game process* exits. Do not recursively hash or traverse an entire large installation on every launch.

**Do not include in M0:** Ludusavi or other external manifest integration, inotify/real-time watch services, elaborate numeric confidence scoring, registry capture, `strace`/process instrumentation, or heuristic scanning of arbitrary system directories. These can be revisited if real-world tests show specific gaps.

Discovery follows a **persistent per-profile/per-game state machine**, not a first-launch or fixed-attempt scan:

| State | Meaning | Automatic behavior |
|---|---|---|
| `searching` | No sufficiently reliable location identified | Baseline likely locations on **every** launch and evaluate differences on exit |
| `identified` | One or more portable save roots reliably declared or confirmed | Stop automatic discovery; snapshot existing save data when present |
| `unidentified` | User explicitly decides to stop searching without a reliable location | Stop automatic discovery; do not claim save sync works |

- **No launch-count timeout.** Three launches for Proton testing or menus should leave the game `searching`. A save created on the fourth (or fortieth) session can still be detected.
- No changes means **stay `searching`**. Logs, shader caches, settings/config files, and timestamps are not enough to identify a save root.
- For unknown engines, inspect only a bounded set of likely game-relative directories (`save`, `savedata`, `saves`, etc.) and common *current-prefix* user-data roots. Compare path, file size, and modification time first; compute content hashes only when making/verifying snapshots.
- A single strong, recognizable candidate may be accepted automatically. Ambiguous candidates remain suggestions and should be confirmable from **Manage Game → Saves** without blocking Play. Avoid inventing numerical confidence thresholds without tests.
- When a trusted recipe or sufficiently reliable engine rule declares a path, `identified` means the *location* is known, **not** that save files already exist; display `No saved data yet` until a snapshot can be created.
- A manual location becomes `identified` once the root is validated as a safe, resolvable portable path; it may legitimately be empty until the first save. Reject paths escaping permitted roots or requiring whole-prefix sync.
- `unidentified` is an **explicit user choice**, not the result of elapsed time or failed launches. Both terminal states persist across restarts. **Rescan** allows either state to return to `searching`; editing paths can directly resolve to `identified`.
- Save data may consist of multiple roots (e.g. ordinary saves plus persistent/gallery progress); do not falsely claim complete coverage if credible additional locations remain. Manual addition of roots always remains possible.
- Preserve symbolic portable paths (`GAME_DIR`, `PREFIX_APPDATA`, `PREFIX_DOCUMENTS`, etc.), never source-device absolute paths. Discovery is only useful if a snapshot can be restored on a different machine.
- Observation runs in the normal launch/exit lifecycle; no mandatory background service. Uncertain discovery must never block Play or announce that saves are synchronized.

**M0 validation cases:** known Ren'Py-like engine; game-specific recipe; unknown engine creating `savedata/` only on a later launch; unrelated log/config changes; multiple ambiguous directories; manual path override; discovery-state persistence/rescan; empty but known save directory; restore into a fresh, differently located prefix/install. Include tests for repeat snapshot idempotence and avoiding symlinks/path traversal. The end-to-end path is **Discover → Snapshot → Restore → Load**; merely locating a `.dat` file is not sufficient evidence of correct backup.


Illustrative snapshot manifest (schema intentionally provisional):

```json
{
  "schema_version": 1,
  "profile_game_id": "uuid-stable-across-this-profile's-devices",
  "edition_id": "release-or-install-variant",
  "snapshot_id": "content-derived-or-generated-id",
  "parent_snapshot_ids": ["previous-id"],
  "created_by_device": "public-key-fingerprint",
  "created_at": "2026-10-10T17:00:00Z",
  "files": [
    {"root": "GAME_DIR", "path": "savedata/save01.dat", "sha256": "...", "size": 12345}
  ]
}
```

The actual schema, content store, and conflict algorithm should be finalized during implementation research. External save-location database integration is explicitly post-MVP unless the team re-scopes it after testing. Game recipes should be able to declare save locations without requiring remote executable scripts.

## 6. Component architecture (MVP)

```text
Novelty UI (Qt)
     |
Novelty application/core
     |-- Profile & library service
     |-- Installation / runtime manager
     |-- Save discovery & snapshot store
     `-- Sync coordinator (queue, versioning, conflicts)
             `-- LAN adapter
                   |-- discovery
                   |-- local pairing / trust store
                   `-- direct authenticated transfer
```

**MVP boundary:** No server daemon, account service, login UI, WAN connector, server schemas, or server startup dependencies. Do not build empty speculative server abstractions solely to satisfy the future roadmap. Keep save manifests, stable identities, and transfer interfaces transport-neutral where straightforward; add the second adapter when it is actually needed.

UI code should not own snapshot logic or protocol implementation; leave the LAN transport testable without Qt. Credentials and private keys must never appear in exports, game transfers, logs, or discovery broadcasts. Enforce authorization for every peer operation. Provide clear errors for multicast blocked, peer offline, missing save paths, disk full, and edition mismatch.

## 7. UX surfaces (minimum)

**Library card / detail panel:** Installed / Not installed; save state (`Local only`, `Syncing`, `Synced`, `Conflict`); Play; optional `Install from LAN` / `Locate game`; manual **Sync Saves**.

**Settings → Sync & Devices:** trusted devices, last seen, pair/revoke; LAN-only operation by default. No server or account settings in MVP.

**Per-game Manage:** discovery state (`Searching`, `Identified`, `Unidentified`), editable paths, **Stop searching** and **Rescan** controls; snapshot history and restore; sync status; executable and Proton overrides. Leave advanced details out of the normal Play flow.

**Notifications:** only actionable failures/conflicts. Avoid blocking splash screens, repeated prompts, mandatory scans, or startup waits for peers/server. Library should load from local data immediately.

## 8. Security & operational constraints

- Authenticate and encrypt P2P connections (TLS or a proven secure session protocol), with per-profile authorized public keys. Local discovery alone grants **zero** access.
- Pairing needs mutual approval with a vetted authenticated handshake and fingerprint verification. Human-readable tags are verification UX, not authentication magic.
- Server authentication, password security, and API authorization are post-MVP concerns; see the deferred section, not MVP implementation tasks.
- Validate manifests and received files, bound sizes/paths, prevent traversal/symlink escapes, and make writes atomic where practical. Game executables are untrusted data; transfer does not imply trust or execution.
- No central service, telemetry, or public infrastructure required. LAN operation should function without internet access.
- Avoid mandatory resident processes. If sync requires the UI to remain open, make the lifecycle and pending status honest.

## 9. Suggested implementation milestones

**M0 — Save discovery & management:** stable per-profile/game/install IDs, isolated local data roots, game recipe + engine rule + manual path + bounded cross-launch before/after discovery, sticky states (`searching` / `identified` / `unidentified`), portable save roots, local immutable snapshots, restore and history. No external database, watcher, registry, process tracing, or networking. Test without network.

**M1 — LAN sync:** local discovery; secure accountless pairing/revocation; authenticated save snapshot exchange; idempotence, offline recovery, conflicts. Two actual machines required for acceptance.

**M2 — LAN installation convenience:** peer game inventory, explicit on-demand copy, progress/resume/integrity verification, separate local runtime setup, restored saves. Must remain optional.

**MVP ends after M2.** Ship and stabilize M0–M2 before deciding whether a server is worth building. M2 is a convenience feature; it may be deferred from the first playable LAN-sync beta without blocking save continuity.

**Post-MVP candidate P1 — Optional self-hosted server:** user/password auth, account-scoped save snapshots, HTTPS WAN sync, retention, conflict integration; LAN must still work if server disappears.

**Post-MVP candidate P2 — Convenience/polish:** optional account-assisted LAN trust, better pairing UX, background sync option *only if justified*, selective performance optimizations.

Do **not** implement all MVP milestones as a single unreviewed Codex task. Each should have tests and a human-verifiable demo before the next.

## 10. Acceptance scenarios

1. **Zero-network:** A fresh user adds a game and presses Play without creating an account. Startup and launch are not delayed waiting for a server.
2. **LAN handoff:** Play on Deck → exit → snapshot sent to paired desktop → play on desktop at matching progress. Reverse direction also works.
3. **No simultaneous presence:** Play offline on Deck → snapshot remains pending → later reconnect to LAN with an authorized peer online → sync completes without loss.
4. **Manual sync:** Repeated clicks with no file changes produce no redundant snapshots.
5. **Conflicting branches:** Both devices play offline from snapshot X. When reconnected, neither branch is discarded; the user can select/restore.
6. **Local install transfer:** Fresh paired device receives a game directory over LAN, validates files, creates its own prefix, and restores its profile's saves.
7. **Household isolation:** Two profiles on the same LAN, including a shared host, cannot discover each other's game catalogue/saves or overwrite each other's files without explicit pairing/authorization.
8. **Revocation:** A revoked peer cannot request new data. Previously copied data is not magically erased.
9. **Failure resilience:** A crashed transfer, unavailable peer, or partially received snapshot cannot destroy the last playable save.
10. **No silent false success:** If a game's save paths are unknown, UI does not misleadingly claim save synchronization is configured.
11. **Repeated test launches:** Launch and quit a game three times without creating a save; discovery remains `searching`. Save on a fourth session; detection can advance to `identified`.
12. **Sticky terminal states:** After `identified` or user-selected `unidentified`, no automatic observation is performed on subsequent launches; explicit **Rescan** re-enables it.
13. **Ambiguous changes:** Logs/configuration changes alone do not get silently labeled save files, and ambiguous results do not terminate searching.
14. **Known engine:** A supported engine preset identifies a resolvable save location without first requiring an actual save file; UI shows `No saved data yet` until one exists.
15. **Recipe and manual paths:** A game recipe or validated manual path is used instead of speculative filesystem observation, including paths under a relocated Proton prefix.
16. **Portable restore:** Saves discovered in a game-local directory or Proton user-data root can be snapshotted and restored into a fresh installation at a different absolute path, then recognized by the game.
17. **No expensive scans:** Unidentified games do not recursively hash the full install on every launch; normal launch remains responsive and the scan is bounded.
18. **Out-of-scope implementation:** M0 has no external save DB dependency, inotify watcher, registry importer, process tracer, server, or WAN code.

## 11. Open decisions for review / repo inspection

- **Profiles:** One profile per OS account for the initial release, or multiple profiles switchable inside one Novelty installation? Support the latter in data modeling even if UI comes later.
- **Identity and library index:** How is a game entry linked across devices? Prefer explicit stable Novelty library IDs preserved by pairing/import, with VNDB ID + edition as matching hints. Avoid matching different releases solely by title.
- **Save path database (post-MVP candidate):** Revisit Ludusavi/other manifests only after measuring gaps in recipes, engine rules, and lightweight observation. Do not include it in initial M0.
- **Discovery evidence / observation scope:** Define a small allowlist of plausible roots and unambiguous patterns from repository knowledge; provide confirmation for ambiguous candidates. No numeric score or launch-count timeout for M0.
- **Transport choice:** Small built-in LAN authenticated file-transfer service versus bundled existing sync engine. Judge against startup speed, operational dependencies, resumability, testing burden, and binary size.
- **Pairing protocol:** Choose a vetted protocol and whether tags are a short authentication string versus a short-lived pairing secret. Don't design cryptography in the handoff.
- **Server deployment:** Not an MVP decision. Revisit packaging, storage, authentication, and REST contract only after LAN MVP is proven.
- **Shutdown behavior:** Do saves finish uploading before Novelty exits, or remain queued until next launch? Both are acceptable if transparent and lossless; decide based on actual client lifecycle.
- **Local save-file writers:** How to handle saves written to the game installation directory and shared imported directories without leaking between profiles.

## 12. Post-MVP roadmap — optional self-hosted server and WAN saves

**Explicitly excluded from MVP implementation and MVP acceptance testing.** Reconsider only after the LAN save-sync flow is stable and useful on two physical devices.

The possible server feature provides username/password accounts, durable per-user save snapshots, and remote save upload/download over HTTPS for devices that are not on the same LAN. It **never hosts game binaries or routes P2P traffic**, and server unavailability must never block play or LAN sync.

Proposed user journey (not yet a committed specification):

1. User enters the URL of their own Novelty server and logs in securely.
2. Client uploads snapshots to an account-isolated save namespace when online.
3. Other authenticated devices can retrieve snapshots without being simultaneously online.
4. Optional account-assisted LAN pairing might register device public keys; never reuse bearer login tokens as P2P device IDs.
5. Unconnected users still get the complete MVP LAN workflow.

Future work will need an independent threat model, session/auth design, server deployment and backups, storage quotas, API authorization, and mixed-transport conflict tests. None should delay MVP or produce placeholder server components in the Codex handoff.

## 13. Handoff guidance (for the next step, not an implementation request yet)

Before coding, Codex should inspect the actual Novelty repository and report: existing application structure, profile/library/game identity model, Qt/framework choices, current Proton prefix layout, launch process lifecycle, save path support, persistence/database, dependency policy, and automated tests. Then produce a concrete **M0 proposal** mapping the **MVP-only** portions of this RFC onto the codebase, noting differences and reducing scope where necessary. The proposal must explicitly cover the four discovery inputs (recipe, engine, manual, bounded across-launch comparison), sticky three-state lifecycle, portable root representation, snapshot/restore proof, and exclusions. Codex must **not** implement, scaffold, or plan server/WAN work for the MVP. Only after review should implementation begin.

**North-star test:** *I can close a VN on my Steam Deck, open the same VN on my desktop, hit Play, and continue — without knowing where the save lives or managing five other apps.*
