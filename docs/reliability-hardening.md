# Reliability and security hardening — 2026-10-15

## Scope and baseline

Reviewed the current implementation, not only the older `cc6f36a` audit target. Existing feature work was checkpointed separately as `6cd45e5`; subsequent commits address the handoff. No Proton Manager redesign or unrelated feature increment is included in the hardening commits.

Novelty's existing identity checks, supervised launches, trusted-peer TLS, immutable hash-verified snapshots, restore journals and recoverable removal remain its foundation. The defects mainly involved commit ordering and subsystem boundaries. The changes establish narrower local invariants instead of adding a transaction framework.

## Findings and disposition

| Priority | Finding | Verified disposition and regression coverage |
| --- | --- | --- |
| P0 | Game HTTP route pointer length | Already fixed in the checkpoint (`constexpr char[]`). Added a real loopback HTTP test of authenticated identity, empty HEAD, verified PUT, committed HEAD/GET, exact manifest/payload/hash, stale PUT 409, corrupt PUT rejection and unauthorized methods. [HTTP implementation](../src/save_server_main.cpp), [test](../tests/server_http_tests.py). |
| P0 | Play after failed automatic restore | Final recovery checks already existed after sync and at process execution. Extended real restore fault/interruption tests to check Play is blocked with an unfinished journal and works after recovery. Existing sync test injects recovery during a network request and separately proves transport failure permits local Play. [Launch](../src/launch.cpp), [execution boundary](../src/runtime.cpp), [restore faults](../tests/durability_recovery.py), [sync boundary](../tests/server_sync_tests.cpp). |
| P0 | Readable data mistaken for durable data | Added small Linux file/tree and parent-directory barriers. Snapshots and imports flush verified stages before publication, then persist publication before updating heads. Restore persists intent, incoming stages, live publication and commit before retiring originals; rollback also persists recovered originals. Server commits persist owned copy intent, verified payload, publication and pointer before clearing intent. Recovery reestablishes barriers before retiring records. LAN installs persist verified assets and configuration before clearing their marker. Cross-filesystem import retains its destination `syncfs` barrier and now persists intent and source retirement. [Helpers](../src/filesystem_durability.cpp), [restore](../src/save_restore.cpp), [snapshots](../src/save_snapshots.cpp), [server](../src/save_server_store.cpp), [LAN](../src/lan_games.cpp), [import](../src/folder_import.cpp). |
| P0 | Guessed discovery authorized destructive operations | Already fixed: ordinary recipe paths and singleton heuristics remain candidates. Existing confirmed mappings and historical snapshots are preserved. Regression verifies candidates cannot create snapshots or omit assets from a transfer. [Discovery](../src/save_discovery.cpp), [authority test](../tests/save_audit_tests.cpp). |
| P1 | Hidden pending/conflict status and recursive retry lock | Already fixed: status precedence exposes both states and retry uses the helper for an already-held mutation lock. Added missing launch-lock parent creation in retry/resolution. Real server workflow tests cover pending capture, retry, conflict, offline/restart and explicit choices. [Sync](../src/save_server_sync.cpp), [tests](../tests/server_sync_tests.cpp). |
| P1 | Local conflict choice rejected changed mappings/identity | Confirmed. Explicit resolution now sends the reviewed server identity with an optimistic revision. The authenticated server permits that explicit replacement and retains old revisions; normal uploads still reject incompatible identity/mappings. Remote restore is disabled with a reason when incompatible, rather than offering an action guaranteed to fail. Changed local/remote state still invalidates the preview. [Client](../src/save_server_client.cpp), [store](../src/save_server_store.cpp), [Manage](../src/window_manage.cpp), [workflow tests](../tests/server_sync_tests.cpp). |
| P1 | Remote approval implied human verification | Confirmed. Receiving users explicitly confirm comparing both displayed codes; a remote message cannot enable that confirmation. Unpaired names are labeled unverified. Shared name validation rejects control, formatting and common invisible characters. TLS verification and fingerprint pinning remain. Tests cover premature approval, no local confirmation/cancellation, mismatched codes, forged nonce reveal, misleading name and successful pairing. [Pairing](../src/lan_connection.cpp), [UI](../src/window_devices.cpp), [integration tests](../tests/lan_integration.cpp). |
| P1 | Recipe environment could configure host execution | Confirmed. A reviewed shared policy applies at parsing and actual game launch, including native launches. Loader/Python/shell startup controls are rejected by name, with no configuration rewrite. Wine/Proton options, locale, DXVK/VKD3D, TZ and namespaced application data remain supported. Tests exercise both boundaries. [Policy](../src/game_environment.h), [parser](../src/recipes.cpp), [launch](../src/launch.cpp). |
| P1 | HEAD hashing, transfer deadlines and resource exhaustion | Confirmed remaining gaps. Startup verifies committed revisions and upgrades legacy metadata; ordinary HEAD reads atomically published metadata without hashing payloads or waiting for a PUT lock. GET/archive and PUT verification/commit run in at most two bounded worker jobs, independent of disconnected Request ownership. Existing 64-connection/10-second header bounds remain; transfers retain 30-second inactivity and 10-minute absolute protection. Client transfer progress resets inactivity rather than consuming a fixed 120-second total deadline; setup is bounded. Real HTTP tests hold a commit at a flush boundary while HEAD/identity respond; controlled transport tests cover slow progress and stalled setup/body. [HTTP service](../src/save_server_main.cpp), [client](../src/save_server_client.cpp), [timeout tests](../tests/server_client_timeouts.py). |
| P2 | Runner removal failed for never-launched games | Confirmed. Create each launch-lock parent before locking; keep active/reference/install protections. [Removal](../src/runner_manager.cpp), [regression](../tests/config_tests.cpp). |
| P2 | LAN source lock retained | Confirmed. Explicit completion acknowledgement, disconnect cleanup and source inactivity handling release the lock. Worker cancellation is coordinated before release. Integration tests check successful/error/retry/disconnect lifecycles and reacquire the source lock. [LAN exchange](../src/lan_games.cpp), [tests](../tests/lan_integration.cpp). |
| P2 | Accepted descriptor leaked on TLS setup failure | Confirmed. Close the descriptor before returning when setup fails before socket ownership. Test checks EBADF after an actual identity/TLS setup failure. [Connection](../src/lan_connection.cpp), [test](../tests/lan_integration.cpp). |
| P2 | Save inventory errors appeared empty | Confirmed. Checked stat and directory enumeration distinguish missing locations from read failures. Mapping roots are checked before ordinary and empty captures. A real readdir EIO interposition test prevents false deletion/empty-state interpretation. [Filesystem checks](../src/filesystem_checks.h), [inventory](../src/save_snapshots.cpp), [test](../tests/durability_recovery.py). |
| P2 | Bundled Qt/shared-library variables escaped to host tools | Confirmed for detached Steam and cross-filesystem cp. Both now use the existing sanitized external-process environment. Normal runtime handling is preserved. [Steam launch](../src/launch.cpp), [copy](../src/folder_import.cpp). |
| P2 | Abandoned staging | Server copy intent now establishes ownership before writing. Startup removes only matching owned request stages after store recovery, under an exclusive service lock. Ambiguous stages remain. Import identity recovery and resumable LAN markers remain; no broad deletion sweep was added. HTTP tests verify owned cleanup and ambiguous preservation after process restart. |
| P2 | Disk space and retention | Snapshot preflight already existed. Added conservative incoming restore, server upload/commit and resumable LAN remaining-byte checks. ENOSPC and concurrent space consumption can still happen after a check; journals/old copies remain the safety mechanism. Retention remains explicitly unbounded; automatic pruning is deferred until protected references and independent recovery are covered. |
| P2 | Steam quoting | Confirmed double quotes alone allowed shell expansion. Refuse dollar signs, backticks and backslashes as well as quote/control separators in shortcut paths, leaving the existing VDF unchanged. [Shortcut](../src/steam_shortcut.cpp), [tests](../tests/core_tests.cpp). |
| P2 | Snapshot IDs/HTTP headers | Existing manifests, hashes, key encoding and header limits already provided validation. Tightened local snapshot tokens and server storage IDs to canonical UUIDs. No demonstrated header-injection exploit was established. |
| P2 | Cover redirects bypassed initial URL policy | Confirmed. Covers no longer follow redirects; non-200 responses are errors. Controlled UI/network test verifies a redirect is not requested or accepted as image data. [Metadata](../src/metadata.cpp), [test](../tests/ui_tests.cpp). |
| P2 | Broad relinked-folder removal | Existing identity, shared-folder, library/system/personal-root checks and removal recovery were already present. Extended personal-root protection to pictures/music/movies/config/data/cache. Arbitrary dedicated folders still require deliberate removal confirmation; Novelty cannot infer every unrelated file's ownership. [Removal](../src/removal.cpp). |
| P2 | Server mode versus manual P2P replacement/Undo | Confirmed UI-only protection. Core replacement and Undo now reject enabled automatic server mode before changing snapshots or saves. Test calls the core functions directly. [Restore operations](../src/save_restore.cpp), [tests](../tests/save_audit_tests.cpp). |

## Durability contract and limits

The commit barriers target Linux filesystems which honor `fsync`/`syncfs`, atomic same-filesystem rename and directory flushes. A staged snapshot or incoming restore is verified and flushed before replacing live data. Publication and recovery state become durable before originals or intent records are retired. I/O failure leaves a recoverable journal or existing committed copy; it is never interpreted as an intentional empty save.

Tests interpose real libc flush/rename/read-directory calls, return EIO, or terminate the process at the boundary, then reopen the library/store and verify recovery and contents. These tests demonstrate software ordering, failure propagation and process-interruption recovery. They do not simulate cache loss or prove storage firmware honors barriers. Real power-loss testing has not been performed.

Staging disposition is deliberately conservative:

| Stage | Disposition |
| --- | --- |
| Server `.stage-*` with valid copying intent | Recover ownership, remove incomplete copy, retain prior revision. |
| Server verified intent/final revision | Verify, persist and finish publication; retain old revisions. |
| Server `.request-*` with matching owner marker | Remove at exclusive startup after revision recovery. |
| Unmarked/malformed request or orphan revision | Keep for inspection; never infer ownership from a prefix alone. |
| Folder import with valid identity journal | Existing conservative recovery checks source/stage/destination before cleanup. |
| LAN installation with transfer marker | Keep and resume only verified completed files; incomplete installations remain unplayable. |
| Snapshot/import partial stage without unambiguous recovery ownership | Keep for inspection. No broad crash-stage or snapshot-history pruning. |

Checks for available bytes reduce avoidable failures; they reserve no disk space and do not establish future availability. Current server revisions and local snapshots can grow without bound. A bounded retention option should be a separate explicit increment which protects the current revision, journals, Undo, pending/conflicting snapshots and ancestry references. Until then, monitor free space and maintain independent backups; sync is not an independent backup.

## Verification and acceptance

The Linux suite is CMake/CTest; the added GitHub Actions workflow builds on Linux with Qt and runs it. Workflow configuration is not a claim that a hosted CI run has occurred. The final local results are recorded below.

Real-world acceptance is **deferred**: a full real library, physical Deck and desktop, interrupted LAN connections and the deployed homelab server with actual storage/proxy/TLS/authentication are a separate milestone. Before that campaign, independently back up valuable saves and record baseline hashes. Automated fixtures are not a substitute.

Remaining risks include actual storage durability, unbounded history, deliberately retained ambiguous stages, and release/game/runtime compatibility on real machines. The implementation is intended to be ready for that controlled acceptance campaign, not for an unqualified save-safety guarantee.

### Tests actually executed

Final command: `bash scripts/build.sh` on Linux using the project Deck SDK fallback and offscreen Qt where specified by CTest. **17/17 suites passed, no skips, 67.70 seconds**:

- configuration, proton_manager, core, save_audit;
- server_store, server_sync, lan_integration;
- ui_lifecycle, executable_ui, peer_library_ui;
- server_http (64 MiB authenticated round trip, concurrent HEAD during held commit, connection/header limits, owned-stage restart cleanup);
- server_client_timeouts, durability_recovery, folder_import_recovery;
- runtime_recovery, removal_recovery, installers (16 Python installer tests).

`git diff --check` passed. Python regression scripts were syntax-checked. The hosted GitHub Actions workflow has been added but has not been run here. The separate real-save acceptance executable was built by the existing build, but was not run against valuable saves.

During repeated full runs, `ui_lifecycle` timed out once at its existing 30-second limit without identifying a failing assertion. Its targeted rerun passed in 16.16 seconds; the final full run passed in 14.54 seconds. The cause of that isolated timeout is not established and is retained as an observation for GUI acceptance, rather than described as a fixed production bug. The expanded HTTP fixture initially exceeded its former two-second staging allowance; the test now allows ten seconds to reach the deliberately held commit while keeping the independent sub-400 ms HEAD/identity responsiveness assertion.

The reviewed critical save-safety invariants and confirmed security defects now have fixes or already-landed implementations plus regression coverage. In my assessment the next appropriate milestone is the controlled real-device acceptance campaign, with independent backups and baseline hashes first. This is not a production durability certification.
