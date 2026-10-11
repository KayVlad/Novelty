# Novelty priority checklist — 2026-10-10

Living Lego priority list, reordered on 2026-10-10 around the product journey. Each block should leave a usable, testable improvement. This document owns delivery order; the older staged plan supplies supporting acceptance detail.

Scope follows [PRODUCT_VISION.md](PRODUCT_VISION.md): a VN-first game lifecycle manager for Proton games. Add the game folder or install the game, then select its executable; automatically select it when discovery finds one suitable candidate. Automatic metadata/presets, unobtrusive play, peer-to-peer LAN installation and save sync complete the intended journey. Deliver one fast application with progressive disclosure and editable configuration. Its owned files belong in the application folder and chosen Games folder; external prefix/Proton managers are optional.

The local launcher, folder/installer import, single-candidate executable selection, editable launch settings, generic/Ren'Py profiles, prefix preparation, runtime installers and cached VNDB metadata are implemented. P1 and P3 meet their current scope; P2 still has setup gaps, and P4 delivers the distribution package and platform acceptance. Local save management is implemented with remaining real-device acceptance; LAN sync and game transfer remain open. Steam scope is an Add to Steam shortcut that invokes Novelty's managed launch; Gaming Mode acceptance stays part of platform validation.

## Completed building blocks

- [x] Native C++20/Qt6 Widgets shell with SQLite installation records and stable IDs.
- [x] Games folder selection, cached library, background discovery, copy settling, Add folder / run external installer and Rescan. Installer entries retain ID/prefix and run detection after supervised completion.
- [x] Missing-game/library states, duplicate prevention, same-directory rename detection and manual relinking preserving prefix/settings.
- [x] Basic umu launch with per-game prefix, explicit runner, executable/cwd/arguments/locale/environment, logs and detached supervision.
- [x] Generic preparation with explicit attempt/step/completion records, normal-exit and registry validation, in-place legacy-prefix verification and retries, explicit Prepare repair, stable successful snapshots and setup input invalidation. Tests preserve fixture saves and cover persisted interruption and real supervisor kills.
- [x] New umu sessions retain their lock until backend descendants exit. A separate monitor keeps tracking after the headless caller dies and clears the persistent marker when descendants stop, permitting same-boot retry. If the monitor itself dies, the barrier remains until reboot. Real SIGKILL fixtures cover setup, gameplay and executable operations.
- [x] Revised cards/list/sidebar, Settings, Manage Overview/editing/logs and file-explorer link; manual VNDB matching, Metadata tab and cached sidebar/card artwork.
- [x] Confirmed full removal of game folder and Novelty-owned data, asynchronous cleanup, stale-path/shared-folder protection and rollback on SQL failure. Running-game protection covers live Novelty-supervised umu sessions. Steam removal is now blocked in the UI and core, including untracked requests across backend switches; durable removal recovery now restores uncommitted staging or resumes confirmed cleanup on startup/Rescan.
- [x] Legacy backend selector and requesting an existing Steam shortcut. This is existing behavior; the planned Steam addition is shortcut creation targeting Novelty's launch command, without a new Steam-owned setup/lifecycle adapter.
- [x] Automated checks: the 2026-10-10 build passed 456 core assertions, focused Add/first-Play UI, process recovery, removal recovery and Python installer suites. The larger UI lifecycle suite was blocked by this sandbox's socket restriction; the prior 2026-10-09 build passed all five then-existing suites including local VNDB integration. Earlier Template!! gameplay/save-load/ordinary relaunch and UI-close survival were user-confirmed. GE 7-18 subsequently failed with a user-reported “Windows 10 required” error; the cause is unverified and a clean GE baseline remains pending.
- [x] Preliminary performance: latest warm/offscreen 500-entry idle run reached its first UI event in 154 ms, used 53,984 kB RSS and consumed zero CPU ticks over two idle seconds.

- [x] Config-pinned official GE-Proton installation, SHA-512 verification, safe extraction/retry, side-by-side retention and offline reuse; asynchronous Get GE-Proton controls in Settings/Manage. Full GE-Proton11-7 archive validation passed; game acceptance is separate.

- [x] Versioned setup profiles with generic fallback, Ren'Py bundled baseline, explicit Manage choice/reset, legacy receipt compatibility and refusal of unknown profile versions. These do not install engine-specific dependencies.

- [x] Native pinned umu installation with archive/zipapp checksums, offline reuse, cancellation/retry and side-by-side retention.

- [x] Supervised Wine configuration/registry tools, saved-setting preservation, automatic preparation and shared launch/removal exclusion.

- [x] One-time patch/installer executables, operation records in Logs, literal arguments and protection of temporarily referenced folders from removal.

- [x] New-game install from an external `.exe` with a new Games child folder, dedicated prefix, N: mapping, completion detection, pending/retry/Detect controls, persisted crash evidence and no automatic rerun. Fake-backend/UI/crash tests pass; real GE install acceptance remains in the validation track below.

- [x] `cards.png` inspired grid, four portrait cards at Deck width, right-click Manage/Delete with a compact Yes/No warning, and aligned list text/header. Cards launch by double-click, show title/engine at bottom left and status light at bottom right. The disabled Delete synced saves checkbox marks a future save-sync choice. Unlinked games use generated artwork.

- [x] One-launch Wine debug traces and local JSON diagnostic export in Manage, with environment values/arguments omitted from the report.

- [x] Add game → Select folder moves a dedicated folder into Games, scans it there, preserves registered umu configuration, rejects collisions and verifies cross-filesystem copies before source cleanup. Japanese locale is selectable for a new installer and editable for an existing game.

## Lego blocks, in delivery order

### P1 — Executable selection after adding a game · completed

Already implemented: Add folder / Run installer, folder scanning, automatic selection of a sole suitable executable, and manual selection in Manage → Launch. Folder ownership and import stay based on the selected game folder.

- [x] When Add folder or installer completion leaves several candidates or none, offer executable selection directly as the next step. Keep the single-candidate path automatic and retain later editing in Manage. Focused offscreen UI and core fixtures cover both Add paths.
- [x] When a game still has no executable, the first Play attempt opens that same picker. Saving a choice continues launch; Later leaves the game untouched. The card uses double-click for Play and right-click for Manage/Delete.

Gate: add a game folder; a sole candidate reaches Play automatically, multiple candidates offer a choice, and no candidate offers browsing or a clear pending state. The selected executable/cwd and existing settings survive reopening. This is a completion of the existing Add flow.

### P2 — Play handles setup

Already implemented: automatic prefix preparation on Play, retry/completion records, versioned generic VN/generic game/Ren'Py defaults, per-game overrides, and native Get umu / Get GE-Proton installers. Generic VN/game profiles prefer the pinned GE-Proton11-7 build when a game has no explicit runner or valid library default. First Play now sequences missing GE, umu and Steam Linux Runtime through one cancellable preparation dialog; an explicit `auto_download: false` still requires a Start click. Manual installers remain in Settings/Manage. The GE pin follows the official release but game compatibility remains unvalidated.

- [ ] The tested resolver now selects an exact game/release rule before an engine rule, then a generic VN/game baseline; saved per-game pins remain authoritative and Manage previews the source. No game-specific recipe is shipped without release-level compatibility evidence. Populate verified recipes and validate them on real games.
- [x] Manage → Runtime now previews the pinned setup profile, the source of the effective Proton choice and locale, updating as those fields change. Verified game/engine recipes and their resolution order remain open.
- [x] On first Play in a new managed library, identify the Steam Linux Runtime required by the selected Proton manifest and prepare it through umu with an in-app activity display, log details, cancellation and retry. Reuse a complete runtime offline. Fixture tests cover the Play path, failure/retry, cancellation and temporary probe cleanup.
- [x] Join missing GE, umu and runtime installation on the automatic first-Play path into one preparation dialog with progress text, details, Cancel and Retry. Fixture tests cover explicit-start policy, failed GE retry, complete GE→umu→runtime sequencing and cancellation. Downloads remain out of routine library browsing.
- [ ] Validate the selected GE pin with real games and verify the combined setup on clean devices. A user should not have to install or open Bottles, protontricks or ProtonUp-Qt.
- [ ] Validate the existing generic/Ren'Py baseline and a representative non-VN game on pinned GE; preserve existing configuration pins and overrides as the preset catalog expands.

Gate: on a clean supported environment, Add → Play provisions what is missing, prepares and launches; interrupted setup retries safely. Subsequent Play works offline and retains user edits and saves. A larger recipe catalog can grow after this gate.

### P3 — Metadata arrives automatically · complete at current scope

VNDB search/link/refresh/unlink, metadata/artwork download and offline caching are implemented. Add, completed installer and background discovery search VNDB in the background. An exact normalized title or alternate-title hit wins; otherwise the first VNDB searchrank result is linked. Failures leave the game playable, and manual corrections take precedence. The user accepts this behavior for MVP and can browse VNDB results manually in Manage → Metadata.

- [x] Trigger background VNDB identification and metadata/artwork fetching on add, reusing the existing cache and manual matching controls. Prefer an exact name hit, then use VNDB's first ranked result. Games without results remain playable.
- [x] Allow manual VNDB search, relink, refresh and unlink. Keep automatic results from overwriting manual links; cache details and artwork for offline reopening.

Gate: automatic VNDB identification and manual correction work without blocking Add or Play. A metadata match alone must not apply a release-specific fix without compatibility evidence. Ambiguity prompts and a general-game provider are deferred refinements, not MVP blockers.

### P4 — A self-contained local product on Deck and desktop

Already implemented: the native application and local SDK build/run scripts. New Games libraries direct pinned umu 1.4.4 runtime/cache storage under `.novelty`; existing libraries retain their previous umu location to avoid silently breaking offline launches. The GUI keeps its two preferences beside the executable and migrates a simple legacy config, and managed umu commands keep temporary work inside Games. An unpacked native test package bundles Qt libraries/plugins, Python 3.12+ and installer helpers; clean-device acceptance and remaining write paths still need an audit before claiming a self-contained release.

- [x] Build an unpacked, relocatable native test package with Qt/SQLite/image/TLS plugins, bundled Python and installer helpers. Smoke-test opening it and running the focused helper/UI fixture from a disposable folder; prevent bundled library paths from leaking into Proton commands. This is not clean-device acceptance.
- [x] Redirect the GUI's known preferences, cache and temporary work to application-local storage, migrate simple legacy preferences and direct managed umu temporary work into the Games cache.
- [ ] Package the required application libraries/plugins and manage runtime provisioning within Novelty. Verify a clean Deck/user and Bazzite desktop installation with no required external game/prefix manager.
- [ ] Put Novelty-owned settings, caches, runner downloads, prefixes, logs and transient work only in the application folder or selected Games folder. Audit QSettings, Qt, umu, Steam Linux Runtime, Python helper/interpreter dependencies and all helper write paths; redirect or clean any unavoidable temporary writes. Document system libraries and the optional Steam shortcut as explicit integration boundaries.
- [x] Add an explicit Settings migration for existing libraries using umu's prior home-directory runtime. It copies and hashes completed Steam Linux Runtime folders under Games, keeps the original, and switches the library only after verification. Cancellation/collision/cutover fixtures pass. Real-library offline acceptance remains open.
- [ ] Verify the existing add/install/play/exit/reopen flow from the distributed package on clean target devices, addressing failures found there.
- [ ] Measure packaged cold/warm startup and overhead while playing. Verify supported library paths, relocation and read-only behavior, documenting actual constraints. Test game removal and app/Games deletion for orphaned Novelty data without deleting retained user files.

Gate: a user can unpack/install Novelty, add a game and reach Play on both target devices without separately installing Bottles, protontricks, ProtonUp-Qt or other manager. Uninstalling the game or the app plus Games removes Novelty-owned state without OS clutter. The validation track below passes for the supported workflow. This closes the local Stage 1 milestone and establishes two usable endpoints for sync.

### P5 — Know and protect the saves

- [x] Store a local library profile ID, stable profile-game ID, game/release/save-compatibility labels and distinct installation ID. Add exact-match declarative save recipes, a verified Ren'Py rule, manual portable roots and bounded observations on every searching launch. Keep ambiguous candidates visible for manual confirmation and persist searching/identified/unidentified states.
- [x] Make verified local snapshots and journaled restore across multiple game/AppData/Documents roots. Empty known roots produce no snapshot; unchanged saves reuse the latest one. Restore works into a new folder and prefix in fixture tests; active games cannot be restored over. Registry-based progress is outside the M0 scope in the [save RFC](novelty_sync_rfc_v0.4.md).
- [x] Record pending post-exit capture and launch observations durably, finish them after the tracked game exits, and recover safe unfinished work on startup/Rescan. An empty known root clears its pending record without claiming success.

Gate still open: load a real game's restored progress after moving to a fresh prefix/install, and check unavailable-drive and interrupted-copy behavior on target devices. A disposable Template!! copy has been snapshotted and restored; the user saw its saved slot, and choosing Load to resume gameplay remains unconfirmed. Local snapshot fixtures pass; the TCP-dependent acceptance path cannot bind in this sandbox. Do not claim real save continuation until the Load action succeeds.

### P6 — Continue on a paired device

- [ ] Pair trusted devices and persist their identities. The P-256 certificate, exact-fingerprint trust/revocation store, manual-IP pairing phrase and two-sided approval are implemented. A trusted listener opens with the library. The pairing screen now advertises and lists nearby UDP discovery hints; manual IP remains available. Run real UDP/TLS pairing on a TCP-capable host and two devices before accepting this gate.
- [ ] Reconcile saves before Play and after exit when the peer is available. A remembered peer endpoint now supplies a bounded save-history lookup and missing-ancestor transfer before ordinary UI and Steam-shortcut Play. A single safe newer branch is restored before launch; conflicts keep both copies and require a choice. Post-exit capture enqueues each trusted peer durably; the window offers verified heads and clears a queue entry only after a durable peer acknowledgement. Manual Manage → Saves uses the same ancestry fetch. TCP and two-device tests, interruption, sleeping-peer and conflict acceptance remain open.
- [ ] Keep pairing and sync status inside Novelty. Devices lists trusted peers and endpoints, manual Save Sync reports results, and Play reports unavailable peers. The selected game's detail panel now shows pending outgoing transfers, available incoming saves and conflicts, and the pairing screen lists nearby discovery hints. Validate these states on real devices.

Gate: with the game already installed on Deck and desktop, play/save/exit on one, open Novelty and continue on the other, then reverse direction. Test both devices changing saves offline, a sleeping peer and an interrupted transfer without losing either copy.

### P7 — Install from a LAN peer and Play

- [ ] Add game now offers **Install from paired device**. The trusted peer catalogue lists portable umu games and checks local profile-game/build collisions. Run it on two devices before accepting this step.
- [ ] Game transfer stages under Games, hashes a source manifest and completed retry files off the UI thread, verifies each received file, reports progress, retains complete staged files across cancellation/disconnect and refuses an existing destination. Socket-free malformed-manifest checks pass. The TCP integration fixture covers install and save reconciliation but its live portion skips in this sandbox. Test a larger game, interrupted retry and malformed paths on real endpoints.
- [ ] Completed transfer registers a local umu installation with copied relative launch paths, arguments, locale, bounded metadata and confirmed save labels/mappings. It uses the local runner, prepares through the existing Play path and checks peer save history. Validate launch and saved progress on Deck and desktop, plus differing-build behavior, before accepting this step.

Gate: from a paired device without the game, one install action obtains the game, prepares it and reconciles saves; Play resumes saved progress. Repeat with an interrupted transfer and a differing game build, reporting any incompatibility before applying saves.

### P8 — Add to Steam

- [x] Manage → Add to Steam writes a validated non-Steam shortcut that invokes Novelty's managed `--run` path through the portable package wrapper when present. It preserves unrelated binary entries, refuses unsupported files, avoids duplicates and updates paths on repeat use after a move. A copied real Steam account file passes the parser/write fixture; the live account was not modified during development.

Gate still open: select Add to Steam on a target device, restart Steam, launch from Desktop and Gaming Mode and reach the same game with its Novelty settings and saves. Scope ends at this shortcut convenience; a separate Steam-managed execution/setup system and broad integrations are not planned. Existing advanced settings, logs, prefix tools and runner selection remain editable.

## Validation and recovery track · applies throughout

- [ ] Rerun the UI lifecycle suite where localhost sockets are available; the latest sandbox run could not start the VNDB fixture.
- [ ] Establish the clean pinned GE baseline: text, audio, movies, input, save/load and offline relaunch. Investigate the reported GE 7-18 error without guessing at dependencies.
- [ ] Test real GE installer/N: behavior, Japanese installer text and prefix tools. Installer reruns remain explicit user actions.
- [ ] Verify UI-close survival, force-exit, suspend/resume, Gaming Mode focus/controller/touch and SD-card disconnect/reconnect. The monitor and recovery fixtures support these checks but do not establish device behavior.
- [ ] Retain current conservative fallbacks for monitor death/legacy process markers and ambiguous import stages. Fix any demonstrated data-loss or unsafe-lifecycle defect before extending the affected path. Verify physical interruption behavior before claiming power-loss durability.

Run device checks as the relevant block lands and user observations become available. Independent implementation can continue while a device check is pending; release and compatibility claims require its evidence. Basic frontend performance and preservation of user overrides are gates for every block.

## Code structure assessment

The program remains small and has useful separation between UI presentation, the core library and headless supervision. Generic setup completion has its own setup.cpp module, and tracked process execution/crash barriers live in runtime.cpp. Launch construction/supervision now have their own launch.cpp module. The main maintenance pressure remains concentration: core.cpp combines storage, discovery and relinking; window.cpp combines the main view, dialogs and operation coordination. Further small extractions alongside new features are appropriate. A broad rewrite is not justified by this review.

## Editable configuration within scope

GE-Proton remains the primary runner family. Existing runner/profile selection, locale/environment/arguments, prefix tools, one-time executable operations and diagnostics remain available through Manage. Further recipe or compatibility changes belong to the relevant tested game/setup work in P2. The core journey does not require a separate expansion of integrations or configuration panels.

Primary references: [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom), [umu launcher manual](https://github.com/Open-Wine-Components/umu-launcher/blob/main/docs/umu.1.scd), [umu protonfixes](https://github.com/Open-Wine-Components/umu-protonfixes).
