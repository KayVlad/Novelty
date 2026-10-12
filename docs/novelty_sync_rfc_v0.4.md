# Novelty — Save Continuity and P2P Game Installs

**Status:** Agreed RFC v0.4, amended 2026-10-12. This amendment replaces the earlier LAN-first automatic save-sync plan in this same file.  
**Scope:** Local save safety, direct LAN game installation, explicit LAN save transfer, and optional self-hosted automatic save sync.  
**Principle:** Install → Play. When a server is configured, ordinary save sync should feel automatic. Without one, replacing saves from another device is an explicit choice. Advanced controls remain available without becoming routine steps.

## 1. Product boundary

Novelty is an offline-first game lifecycle manager for Proton games, initially optimized for visual novels. It must launch local games without a server, an account, another running device, or a network connection.

The agreed save architecture has two mutually exclusive modes:

| Mode | Routine save behavior | LAN game installation |
| --- | --- | --- |
| **Local/manual** (default) | Local snapshots after changed Play sessions. A user explicitly chooses **Transfer saves from another device…** for later P2P replacement. No automatic P2P save polling, offers, or queued pushes. | Direct paired-device game transfer remains. A genuinely fresh installation adopts one compatible source save automatically when mapped local roots are empty. |
| **Server auto-sync** (opt-in) | An optional self-hosted server supplies the current save revision. Novelty checks before Play and conditionally uploads after exit. Offline Play remains available. | Game files still transfer directly over LAN. The server supplies initial saves when enabled; manual P2P save transfer and its dedicated Undo are disabled until server sync is turned off. |

There is no public Novelty cloud, mandatory resident daemon, server-hosted game binary, WAN P2P game transfer, binary save merge, CRDT, Git-based save backend, or automatic mixed P2P/server reconciliation. The self-hosted server is one user's optional save service, not a general account platform.

The server is a synchronization point, **not an independent backup**. Its rolling recovery copies can live on the same host and fail with it. Backup to a separate destination is a distinct follow-up feature and must be labelled separately in the UI.

## 2. Identities and data boundaries

| Entity | Meaning | Rule |
| --- | --- | --- |
| Local profile | A person's Games library and progress | Isolate games, prefixes, save roots, peer trust and server credentials. Separate OS accounts provide stronger local security than two profiles under one OS login. |
| Device | One Novelty installation | LAN pairing uses its persistent authenticated device identity. Server credentials are separate secrets and are never advertised over LAN. |
| Game and edition | Stable save compatibility identity | Never match only by display name. Preserve established P2P installation identity; offer an explicit server-game key for independently installed copies. |
| Installation | Local game folder, runner and prefix | Do not transfer another device's prefix, runner path or absolute save paths. |
| Save set | Confirmed portable game/prefix save mappings | Include all known required roots. Unknown or ambiguous paths do not count as synchronized. |
| Local snapshot | Verified restorable save set | Reuse the current snapshot/restore store and recovery journals. Historical snapshots remain readable. |
| Server revision | Current accepted state for one server game key | A monotonic integer scoped to a persistent server identity. It is not a local snapshot ID or merely a revision the client observed. |

LAN pairing and game discovery retain their existing authenticated transport. Local discovery grants no save or game access by itself. Revoking a peer or disabling server sync never deletes local saves or recovery copies.

## 3. Save discovery and state representation

Novelty uses declarative game recipes, reliable engine rules, validated manual mappings and a bounded search of plausible save locations. The persistent states are `searching`, `identified`, and explicitly user-selected `unidentified`. An identified path may have no save files yet. Ambiguous candidates remain suggestions; they must not silently become an authoritative mapping. Manual paths can be edited later. Whole-prefix or arbitrary-system scans, registry capture and external save-location databases are outside this scope.

Mappings use portable game/prefix roots, never another machine's absolute path. Multiple roots are supported. Before any destructive operation or server comparison, read and hash the **actual mapped local files**. A cached local head, timestamp, or server revision alone does not prove that live files are unchanged.

An **empty save state is valid** when every configured mapping can be resolved and inspected successfully and no mapped save files exist. It needs a verified manifest/fingerprint and must be transferable/restorable, so an intentional deletion can reach another device. Missing or unreadable game folders, disconnected drives, unsafe paths, failed inventory and unknown mappings are errors, never evidence of an empty state. Existing libraries and old nonempty snapshots remain readable. Do not silently infer that a user intended deletion from an I/O failure.

Capture at safe points, chiefly after the actual game process exits. Never replace mapped saves while the game or a prefix tool is running. Stage and hash incoming data before replacement; verify game/edition/mapping compatibility; preserve a recoverable local copy; then use the existing restore journal. A crash must leave either the original or the committed new state recoverable.

## 4. Direct LAN game installation

An authorized user chooses **Install** on a paired device's game. Novelty transfers game assets into a verified resumable stage, excludes confirmed game-local saves from that payload, registers a local installation, chooses a local runner, and prepares its local prefix without starting the game. The ordinary result is **Install → Play**.

In local/manual mode, a fresh installation with one compatible source save and no local mapped progress adopts it automatically after prefix setup succeeds. Unexpected local files are preserved and require review. This one-time installation bootstrap is the only automatic P2P save adoption. In server mode, the server's accepted state is the initial save source; a source-device snapshot may remain available for explicit recovery but must not race server sync. A failed/cancelled setup keeps downloaded game files and save history; Play can retry setup.

LAN game transfer must remain usable without a save server. It never copies a Proton prefix or silently runs the downloaded game. Pairing, game catalogue, verified/resumable transfer, path validation and collision checks remain in scope.

## 5. Explicit P2P save replacement (local/manual mode)

Manage → Saves offers **Transfer saves from another device…** only when server auto-sync is disabled. The user chooses an authenticated reachable source and a compatible game. Novelty fetches the source's latest complete verified save state; it does not rank P2P branches, infer a timestamp winner, or poll peers before Play. Preview source and direction, then ask once before replacing nonempty local saves:

> Replace local saves? Saves in this game's configured locations will be replaced with saves from [device]. Novelty will keep the current state so you can undo this transfer.

Stage and verify the entire bundle, capture the pre-transfer state, and restore using the existing crash-safe journal. Keep a **persistent one-transfer Undo record** that survives restarting Novelty. It points to a verified rollback state, including a legitimately empty prior state, and records the installed-state fingerprint. Undo is enabled only while that rollback remains restorable. If live files changed after transfer, warn and preserve them before any Undo restore. A later successful manual transfer replaces the dedicated Undo point only after its own restore commits. Existing general snapshot history remains separately available.

Do not remove automatic P2P machinery until this manual path and Undo pass failure tests. Then disable automatic peer polling before Play in both the GUI and headless launcher, periodic timer checks, unsolicited save offers, and outgoing queue processing. Legacy queue records must be retired or parked without sending surprise save transfers and without deleting snapshots. Keep LAN discovery and exchanges needed for game installation and explicit transfer.

## 6. Optional self-hosted automatic save server

The service binds locally by default. The user supplies its HTTPS URL and an access credential; deployment behind a TLS reverse proxy is acceptable. Keep the setup small: address, credential, connection test and enable/disable. Do not add registration, multiuser account management, a dashboard, peer discovery, or game binary storage. Never place credentials in LAN advertisements, game transfers, logs, reports or exported diagnostics.

For each stable compatible game key the server stores one **current complete verified save state**, a monotonically increasing revision and a few rolling recovery copies. It stages and verifies an upload before atomically publishing revision `N+1` only when its current revision still equals the client's `expected_revision=N`. A stale conditional upload changes nothing and reports a conflict. Bound manifest and payload sizes; survive interrupted uploads, process restarts and disk errors without losing the last committed state. Protocol and storage versions must be explicit.

The initial bundle stream is version 1: `NVSB`, a two-byte big-endian version, two zero reserved bytes, a four-byte big-endian manifest length, compact UTF-8 schema-2 snapshot manifest JSON, then raw file bytes in manifest item/entry order. Directory entries carry no payload. Limit the manifest to 1 MiB, file payload to 512 MiB and entries to 20,000. Reject unsafe or duplicate paths, incomplete or extra bytes, and any hash mismatch. Extract into a new private stage and verify the complete portable bundle before publication. This format includes a verified empty state with no file payload.

The initial HTTP protocol uses a bearer credential and closes each response. `GET /v1/identity` returns the persistent server ID. `HEAD /v1/games/{key}` reports `X-Novelty-Server-Id`, `X-Novelty-Revision`, `X-Novelty-Snapshot-Id` and `X-Novelty-Files` headers with no body; `GET` at the same path returns the archive for an existing revision (or JSON revision 0 when none exists). `PUT` at that path carries an archive, `Content-Length`, and `X-Expected-Revision`; a successful conditional commit returns the accepted revision and file count, while HTTP 409 reports the still-current revision. `{key}` is the unpadded base64url encoding of the UTF-8 server game key. A P2P installation uses its shared profile-game ID by default; independent installations can deliberately set the same advanced key in Manage → Saves. The service listens on loopback; external access requires an HTTPS reverse proxy. The first service process creates an owner-only token file under its private data directory. Clients never send credentials to peers or include them in diagnostics.

Each client persists the server's identity, its last **accepted** revision, and a fingerprint/manifest of the live files accepted at that revision. An observed remote revision is not accepted merely because a check returned it. If server identity changes or revision unexpectedly decreases, stop automatic apply/upload and require explicit reconciliation.

At each boundary, compare actual mapped local files with the accepted baseline:

| Server revision | Local files | Action |
| --- | --- | --- |
| Same as accepted | Same as accepted | Play; no transfer. |
| Newer | Same as accepted | Fetch, verify, preserve rollback if needed, restore, accept new revision, then Play. |
| Same as accepted | Changed | Conditionally upload the complete local state. After acknowledgement, accept the new revision. |
| Newer | Changed | Conflict. Preserve both states and ask which to use; never choose by timestamp. |
| Unreachable or timed out | Any | Allow offline Play; retain local pending state and do not advance the accepted revision. |
| No accepted baseline | Any | Reconcile: empty local + populated server → adopt; populated local + empty server → conditional publish; both populated → explicit choice; both empty → accept an empty baseline. |

Before Play, use a bounded network check and allow offline launch. After the real game process exits, capture changed saves and upload outside the GUI thread. A successful server acknowledgement, not game exit, justifies **Synced**. A failed upload remains **Pending** and retries with modest backoff while Novelty is open; no always-running daemon is required. Multiple offline sessions may coalesce into one latest pending state, but the expected revision remains the one from which local edits began. A stale upload rejection preserves local and server copies and surfaces a conflict. Choosing local still uses a fresh conditional revision check; choosing server uses verified journaled restore.

The GUI Play path and the headless/Steam-shortcut launch path must use the same before/after Play rules. If the server is disabled, ordinary Play performs no P2P save network request. Manual P2P transfer and its dedicated Undo are unavailable while server mode is enabled; ordinary local snapshot/restore remains available. Turning server sync off never deletes local saves or history. Re-enabling it performs a safe first-connection reconciliation.

## 7. Storage, retention and independent backup

Keep the current local snapshot store and restore journals during this change. New behavior must read historical snapshots and interrupted operation records. A later rolling-retention policy should target roughly four recent restorable backups per game **plus** protected current state, one-transfer Undo, unresolved conflict copies, in-progress restore/recovery, pending upload and active transfer references. This is a soft target, not permission to delete protected data.

Add a storage usage display for each game and the library: on-disk snapshot use, restorable count, protected/reclaimable use and server copy status. Scans belong off the GUI thread. Pruning is a separate, fault-tested increment: prove every reference, including recovery journals and legacy queue records, before deleting any payload. Existing ancestry manifests cannot simply be removed while code still uses them for peer classification; either retain required metadata or retire that dependency safely. If reference state is damaged or uncertain, do not prune. Storage pressure must be reported honestly rather than silently sacrificing recovery.

Server revisions and rolling copies are sync/recovery facilities, not an independent backup. A separate verified export or backup to another destination, restorable without the original library or server, remains a follow-up. The UI must distinguish **Synced** from **Backed up**.

## 8. Implementation order and acceptance

Make separate reviewable changes while keeping the build and old libraries usable:

1. **Compatibility baseline:** document existing snapshot formats, restore journals, P2P install dependencies and all automatic P2P save entry points. Add focused tests before removing behavior.
2. **Manual P2P mode:** implement one-way verified transfer, explicit replacement and persistent Undo (including empty states). Disable automatic P2P save paths only after this works; park legacy queues safely. Preserve automatic fresh-install adoption.
3. **Server service:** implement a small authenticated revision protocol, atomic verified storage, conditional upload, restart and fault tests with disposable data.
4. **Client server mode:** add opt-in settings, accepted baseline and live-file comparison, before/after Play integration for GUI and headless launches, offline pending state, conditional conflict handling and mode gating.
5. **Follow-ups:** storage usage display, fault-tested rolling pruning and independent backup. Keep these visible in the roadmap rather than implying server sync supplies them.

Required acceptance scenarios:

- No server: game Install adopts compatible initial saves on a genuinely empty receiver; later manual **Transfer saves → Confirm → Undo available** works after restart. A second transfer replaces only the prior dedicated Undo point after success.
- Empty states: intentional deletion is transferred/restored as empty, while unreadable paths, missing media and unknown mappings never appear empty or successful.
- Transfer faults: disconnect, bad hash, mismatched edition/mapping, disk full, process kill during stage/restore, running game and changed files before Undo preserve recoverable progress.
- Server: desktop upload → Deck download before Play → Deck upload → desktop download works with the other client off. Changed local plus newer server revision prompts instead of overwriting. Stale conditional upload leaves server unchanged.
- Offline: game launches, pending state survives restart, reconnect retries from the original accepted revision, and status never claims **Synced** before acknowledgement.
- Migration: historical snapshots and recovery journals remain readable; legacy P2P queues cannot trigger surprise transfers; game transfer, launch, save-audit and recovery tests continue to pass. Real two-device and clean-device acceptance is reported separately from fixtures.

**North star:** *P2P for games; an optional server for automatic saves; explicit P2P replacement otherwise; verified recovery everywhere.*

## Appendix — Existing library and recovery formats

The compatibility baseline for incremental changes is the current `.novelty` library layout:

| Record | Existing format and obligation |
| --- | --- |
| Per-game `saves.json` | Schema 1 and 2 configurations are read and normalized. Stable profile-game identity, confirmed portable mappings, edition labels and peer links remain intact. |
| Per-game `snapshots/<id>/manifest.json` | Schema 1 snapshots remain restorable locally. Schema 2 is the portable manifest with mapped items, counts, hashes and parent IDs; newly verified empty states use schema 2 with zero files. Imported snapshots are immutable. |
| `save-head.json` | Schema 1 pointer to the locally applied snapshot. An imported snapshot is not applied merely because it was fetched. |
| `save-restore.json` | Schema 1 restore journal with staged/held items, a safety backup and committed cleanup phase. Startup/Rescan recovery runs before transfer-intent recovery. |
| `save-pending.json` | Schema 1 pending post-Play capture. Keep its identity/mapping checks and retry path; verified empty capture can complete it. |
| `sync/queue.json` | Schema 1 legacy automatic P2P offer queue. Keep it readable and parked; ordinary snapshots no longer add tasks and no background sender processes old tasks. Do not delete its referenced snapshots during future pruning. |
| `save-transfer-intent.json`, `save-transfer-undo.json` | New schema 1 per-game manual-transfer records. The intent resolves a crash between restore commit and Undo publication. The Undo record points to one verified pre-transfer snapshot and the installed snapshot. It is replaced only after a later transfer commits. |

Recovery and historical records are part of the storage graph. A later protocol or pruning change must migrate or retain their references explicitly, rather than assuming only the current head is live.
