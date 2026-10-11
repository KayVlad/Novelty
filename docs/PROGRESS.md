# Novelty progress log

## 2026-10-08 — Staged implementation proposal

The following response is preserved verbatim for future reference.

---

**I propose two stages: the base launcher, then saves.** Each stage gets smaller working increments, with a test gate before the next. Metadata fetching, text extraction and playtime stay beyond MVP.

I’ve recorded this in the [updated plan](/home/deck/projects/Novelty/docs/MVP_PLAN.md). Implementation hasn’t started.

**For the stack, I recommend C++20 + Qt 6 Widgets.**

| Component | Choice |
|---|---|
| Interface | Qt Widgets, with a model-backed library list |
| Library and setup state | SQLite |
| Engine presets and per-game configuration | Versioned JSON |
| Execution | Separate umu and Steam adapters |
| Build and tests | CMake, Ninja, Qt Test |
| Initial distribution | Self-contained application folder; evaluate AppImage afterward |

This changes my earlier Python recommendation because fast startup and low overhead are now explicit priorities. C++ adds development effort, but suits this relatively small native application. Qt already provides the list models, filesystem notifications and process handling we need. [Qt model/view documentation](https://doc.qt.io/qt-6/model-view-programming.html)

Development tools would live in an isolated environment on the Deck. The application would run directly on SteamOS.

**Stage 1 would have four increments.**

| Increment | What you can use | What we verify before continuing |
|---|---|---|
| **1A: Basic launch** | Small library window, manually select one game and runner, launch through umu, view errors/logs | Actual launch, exit, offline relaunch, Japanese/spaced paths, closing the launcher without killing the game |
| **1B: Games folder** | Pick the main folder; discover new games automatically; correct ambiguous executable choices | Copying games into the folder, duplicate detection, missing SD card, rescanning without freezing the interface |
| **1C: Automated setup** | Generic setup first, then engine presets added individually | Clean-prefix setup, interrupted setup recovery, text, movies, audio and save/load for each tested preset |
| **1D: Steam and Deck completion** | Backend setting and per-game override; finished Manage menu | Both launch paths, Gaming Mode focus and controls, suspend/resume, force-exit and packaged startup |

For discovery, the starting convention would be **one game per subfolder**, with a bounded search inside it. Novelty shows the cached library immediately and scans afterward. Ambiguous folders get a quick executable selection.

New games get prepared automatically on **first Play**, with a separate **Prepare** action available. That avoids creating large prefixes for every game merely because the folder was scanned.

**Engine setup needs its own research and evidence.**

Detection and configuration will be separate: recognising an engine does not establish which fixes a particular version needs.

For each engine, we would:

1. Establish detection signals from engine sources, documentation and actual installations.
2. Test a clean generic prefix with a fixed runner.
3. Add dependencies or overrides only where they solve an observed problem.
4. Record which game/build was tested and what worked.

Kirikiri/Kirikiri Z, BGI/Ethornell and Siglus are reasonable first research candidates; the available test games should determine the order. For example, Kirikiri’s source establishes XP3 support, but finding an `.xp3` file alone doesn’t identify every relevant engine variation. [Kirikiri source](https://github.com/krkrz/krkrz/blob/master/base/XP3Archive.cpp)

The **generic VN preset** would be conservative: dedicated prefix, selected runner, correct working directory, and appropriate locale/font settings. Codec and DLL changes would be targeted. Successful settings stay pinned, and preset updates preserve your overrides.

**The interface would be a practical library window.**

- Search, Rescan and Settings at the top.
- Readable game rows with name, engine and readiness.
- Clearly visible **Play** and **Manage** buttons.
- Controller focus, touch targets and text sized for the Deck’s 1280×800 screen.

Manage would contain executable, arguments, working directory, backend, runner, engine preset, locale, environment variables, DLL overrides, dependency tools and logs. Preset values and your overrides would be distinguishable, with reset-to-preset available.

For colours, I’d start from VNDB’s dark-blue skin: near-black background, navy panels, pale text, light-blue accents and blue borders. I retrieved its [skin source from a mirror](https://github.com/vgtuemgav/yorhel-vndb/blob/main/css/skins/angel.sass); we’ll verify the visual match and adjust contrast for the Deck.

**Performance becomes an acceptance criterion from the first build.** Proposed targets are an interactive cached library within one second warm/two seconds cold, under 100 MiB idle frontend memory, and near-zero idle CPU. These are targets to measure here, not promised results. Scanning and downloads run outside the UI thread; while playing, Novelty stays quiet.

Stage 2 then adds **save mappings and local backup/restore**, followed by **one sync transport with conflict handling**.

The remaining clarification is the Steam option: I’m currently assuming you mean **launch through the Steam client as a non-Steam game**. Selecting a Steam-installed Proton version while launching through umu is a different, simpler feature.

---

## 2026-10-08 — Implementation authorised

User instruction: “put this response in a progress log inside the project for future reference, then proceed with implementation”. Beginning with increment 1A; later gates require actual game testing. Steam backend meaning remains open.

## 2026-10-08 — First runnable launcher increment

Implemented C++20/Qt6 Widgets library and Manage screens, SQLite persistence, a main Games folder, background discovery, manual executable/runner selection, advisory engine evidence, generic prefix preparation, detached umu supervision, logs, retry/interrupted state and per-installation locking. The Steam adapter can request an existing shortcut; registration and complete Steam lifecycle handling remain future work.

Saved the user's test library `/home/deck/TestGames`. Imported Template!! with `Tenpure.exe` and detected Ren'Py from its runtime directory. Its `.xp3.rpa` filenames are covered by a regression check. Selected existing Proton 10.0 for the upcoming real test; no game execution/save changes yet.

The Deck had runtime Qt libraries but no development headers/compiler on PATH. Built using its existing Freedesktop SDK compiler plus project-local public Qt headers and generated host-matching configuration. Build/test helper scripts preserve this fallback; no system package or Steam configuration changes.

Validation: C++ core checks and a UI integration check pass; the latter confirms that destroying the UI does not kill the supervised fake game. Python installer tests pass. Offscreen preview generated at `artifacts/library.png`. Preliminary 500-entry measurement: 89 ms to first queued UI event, 45,172 KiB RSS, zero CPU ticks over two idle seconds. Actual display/Gaming Mode performance remains unmeasured.

Blocking the real-game gate: umu is missing. A pinned upstream umu 1.4.4 installer was prepared and its download attempted; sandbox DNS resolution failed. Next: install it from the normal Deck terminal, then test Template!! through the built application, including text/movies/audio/save-load and exit. Increment 1A is not yet declared compatibility-tested. Engine-specific dependency setups and automatic downloads remain increment 1C; saves remain Stage 2.

## 2026-10-08 — Real umu launch unblocked

User reported installation complete. Verified library-local umu 1.4.4 and the first actual Template!! launch: its dedicated Proton 10.0 prefix exists and the supervisor recorded a clean exit at 17:54:13 UTC. Started a second launch at 18:03:32 UTC and inspected its rendered startup splash. No engine dependency changes were needed to reach that point. Left the second session open for user checks and requested observations about text, sound, movies and save/load. Those checks, offline relaunch, real UI-close survival and Deck Gaming Mode remain pending; increment 1A is still gated on this evidence.

## 2026-10-08 — Gameplay confirmation and first discovery hardening

User replied “works perfect” to the requested gameplay/font/audio/movie, save/load, closing Novelty while playing and quit/reopen/relaunch checks. Recorded those as user-reported passes for Template!! / Proton 10.0 with the generic setup. The actual launch journal returned to Ready at 18:06:20 UTC. Offline operation, actual Japanese installation paths, suspend/resume and Gaming Mode remain separate checks.

Found an early-import issue: an executable arriving before its assets could be imported immediately. Automatic imports now wait for a four-second unchanged metadata interval, using full-tree names/sizes/mtimes in a worker for new candidates only. No game asset contents are hashed. Watch relevant empty/nested folders, rescan only while a candidate is settling, and keep configured games launchable. Quiet time is a heuristic and cannot prove an externally paused copy is complete.

Missing libraries retain rows in the existing window, disable Play and reconnect through Rescan. A missing remembered database is not recreated at an empty mount point. No test game assets, prefixes or Steam configuration were changed in this increment.

Validation: 71 core assertions and the extended UI integration check pass. UI checks cover copy delay, existing-game availability, duplicate prevention, unavailable/reconnected roots and an empty remembered mount point, in addition to detached-game survival. Volume disconnection is simulated, not a physical SD-card test. Updated 500-entry warm/offscreen measurements: first UI event 79 ms, RSS 44,300 KiB, zero CPU ticks over two idle seconds. Folder rename/relink handling is next within increment 1B; the full 1B gate is not yet declared complete.

## 2026-10-08 — Folder rename and relinking

User instructed “proceed”. Added native Linux directory identity (device, inode and creation timestamp) to recognise the same game directory under a new direct-child name. Older records learn identity during discovery while their folders are present. Copies and unsupported identities do not trigger guessed matches. Automatic relinking preserves the installation ID, prefix, custom settings/name and accepted Proton revision.

Added Manage → Relink folder for other moves and changed executable layouts. The dialog previews new executable/cwd and the existing prefix. It can consolidate an unused automatically discovered destination record; configured/prepared/running entries are protected. Both entries are locked, stale previews are rejected, and database updates/deletion commit together. Game assets and prefixes are never moved or deleted by relinking. Steam shortcut relinking remains disabled until that adapter can update Steam itself.

Validation: 127 core assertions pass, including identity migration, native rename, different-copy identity, replaced folders, prefix/save preservation, runner-update detection, both locks, duplicate conflicts and rollback. The extended UI test opens the actual Manage/Relink dialogs, applies a transfer to a Japanese/spaced path outside Games, then launches the fake backend with the new executable/cwd and original prefix. Both CTest checks pass. These use temporary fixtures. The user's Novelty UI is open, so Template!! and its real prefix were left untouched; reopen Novelty to run the new build. Physical SD-card, offline, suspend/resume and Gaming Mode gates remain pending.

Next increment: harden interrupted generic-prefix preparation before adding engine dependency recipes. The folder discovery mechanics now have automated coverage, but full increment 1B hardware acceptance is not yet declared complete.

Final warm/offscreen performance check for this build with 500 entries: first UI event 100 ms, RSS 45,008 KiB, zero CPU ticks over two idle seconds. This remains a development measurement, not a cold-start or Gaming Mode result.

## 2026-10-08 — Diagnosing Needs attention after relinking

User reported a relinked game still showing Needs attention. Its saved paths correctly pointed to `/home/deck/TestGames/Template!!?`, but the subsequent real launch exited with code 1. A diagnostic Proton trace in `/tmp/novelty-relink-debug/steam-default.log` reported `Failed to create process` for the executable at that path, with Windows error 2. The game log had not advanced since the previous successful session. This was a launch failure after a correct relink, rather than proof that the relink lost its paths or prefix.

The renamed component contained `?`, a reserved Win32 filename character under [Microsoft's documented naming rules](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file). Restored the test copy's original `Template!!` directory name, allowed identity-based reconciliation to restore its configured paths, then launched it successfully: the same installation ID/prefix is retained, the journal changed to Running and Ren'Py wrote a new startup log at 19:13:31 UTC. No prefix recreation or save migration.

Added preflight diagnostics for reserved path characters/device names and trailing periods/spaces in the umu executable/cwd. The library shows Path needs rename, the selected game's details and Relink preview explain the offending name, and launch fails with that actionable message before invoking umu or preparing a prefix. Display titles, arguments, Japanese characters, interior spaces and ordinary punctuation remain supported. These checks describe conventional Win32 paths; native/Linux and alternate Wine path mappings are not implemented by this backend yet.

Validation: 139 core assertions and the UI integration test pass. The restored real game completed its session with exit code 0; its journal returned to Ready at 19:14:44 UTC with an empty error message. Reopen the currently running UI to load the diagnostic update and refreshed status.


## 2026-10-08 — Mock layout revision and visual metadata scaffolding

User instruction: “before we continue, i put a mock.png in desktop as a revision of the app's layout, do that, prepare some metadata scaffolding but no need to implement it yet, just a visual thing for now”. Inspected `/home/deck/Desktop/mock.png` and preserved a reference at `docs/design/layout-reference.png`.

Replaced the default row-only library with an adaptive cover-card grid, a working grid/list toggle and a larger selected-game details pane. Reorganised the header to match the reference: Novelty branding, Add game, Settings and Games folder; search, view mode and Rescan beneath. Kept the VNDB-inspired navy/blue palette, real engine/readiness/path information and working Play/Manage actions. The card ellipsis opens Manage. Grid/list share selection, including when returning from a table engine/status cell, and remember the chosen view. Play/Manage remain accessible below a scrolling details area at smaller sizes.

Added a presentation-only `GameMetadata` contract for cover, description, tags, language, playtime and last played. Current metadata remains empty: generated gradient/initial covers, “No description yet”, “No tags yet” and em dashes. No metadata fetching/persistence, artwork discovery, playtime tracking or tag editing. No example games/artwork from the mock were imported. Existing game files, prefixes, saves and launch adapters were left unchanged.

Cards use a native item delegate rather than allocating a widget hierarchy for every game. A nine-entry preview exposed an icon-grid sizing issue when Qt reserved scrollbar space; corrected the layout budget and added checks for all three columns, hit testing and scrolling to the last card. Card actions require an actual press/release click rather than a drag ending over a button.

Validation: release build and both CTest checks pass (139 core assertions plus the extended UI integration check). UI coverage includes grid/list switching, selected details, actual card Play/Manage, metadata placeholders, search/empty results, smaller-window controls, three-column layout/scrolling, detached-game survival, copying, rename, relink and library reconnection. Inspected 1280×800 real-library and nine-entry fixture previews, plus the 900×620 fixture preview. These use offscreen rendering and fake launch fixtures; they do not establish additional real-game/controller/Gaming Mode compatibility.

Final warm/offscreen 500-entry measurement: first queued UI event 126 ms, RSS 49,644 KiB (about 48 MiB), zero CPU ticks over two idle seconds. Previews: `artifacts/layout-revision.png`, `artifacts/layout-grid-fixture.png`, `artifacts/layout-small.png`. Details and regeneration instructions are in `docs/UI_LAYOUT.md`. The existing launcher window is still open; reopen it to load the new build. The next backend increment remains interrupted generic-prefix preparation, after this layout revision is reviewed.

## 2026-10-08 — Simpler cards/sidebar, Overview and full removal

User requested removing the header logo, moving Games folder into Settings, image-led cards with only title/compact Play/Manage controls, no ellipsis, a sidebar containing cover/title/status/description/tags/playtime, a default Manage Overview, a clickable footer path and full removal with a warning/confirmation. Implemented these revisions. The saved mock remains the initial reference; this follow-up overrides its controls and information layout.

Cards retain the existing launch/Manage actions with fewer displayed details. The sidebar's configuration fields and action buttons are gone; metadata remains visual placeholders. Manage Overview shows the saved engine/backend/executable/cwd/runner/prefix/locale/arguments/environment and evidence, with the existing editing tabs available. Overview paths update after relinking. The footer opens the main Games path through the default file explorer. List view retains Play/Manage through its context menu and Ctrl+M.

Manage → Remove game lists the game directory, dedicated Novelty prefix and per-game state, explains permanent deletion and requires typing the saved game name. Cancellation and incorrect names do not delete anything. Saves within those directories are included; external files/saves and Steam-owned shortcut/prefix data are explicitly retained. The action is bound to the game being managed, rather than a later background selection.

Removal revalidates configuration, directory identity and ownership boundaries under the launch lock and a SQLite write transaction. Running games, changed/replaced folders, linked targets, broad personal/system/library folders, shared game folders and another game's runner are protected. Game/prefix folders stage by same-filesystem renames; failed SQL restores them. After commit, deletion runs in a worker while the modal progress UI remains responsive. Partial cleanup reports remaining paths. Power-loss-atomic removal and automatic recovery of hidden staged folders are not implemented; those limits are documented in UI_LAYOUT.md. Old discovery snapshots are invalidated so they cannot reimport a removed game.

Validation: release build and both CTest checks pass, with 184 core assertions plus UI integration. New core checks cover busy/stale/shared/linked folders, symlink targets, missing-game cleanup, another game's retention and rollback after an injected SQL deletion failure. UI checks verify Overview, Settings folder access, list actions, the clickable footer markup, cancellation and exact-name-confirmed removal. All destructive checks use temporary fixtures. Real-library, nine-card, smaller-window, Overview and confirmation previews are in `artifacts/`; inspected the revised layout and confirmation screen. The default file-explorer dispatch uses Qt's local-file URL API; actual desktop handler configuration is not established by offscreen tests.

Latest warm/offscreen 500-entry check: first queued UI event 112 ms, RSS 48,412 KiB (about 47 MiB), zero CPU ticks over two idle seconds. No extra UI runtime or permanent worker/daemon was added. Reopen Novelty to load the new build.

User asked “what's next in line” during this work. Next remains interrupted generic-prefix preparation/retry and actionable setup failures, followed by independently researched/tested engine recipes with the generic fallback, then fuller Steam integration. Saves are Stage 2. This turn completes the UI/removal revision; the next setup increment has not started.

## 2026-10-08 — Quick code and priority review

User requested an overview of how the staged build is going and a priority-ordered done/todo checklist. Reviewed current implementation, staged plan and latest recorded tests; saved `docs/STATUS_CHECKLIST.md`. No source changes or new test execution in this pass.

Assessment: the launcher/library/UI foundation is working and remains lightweight, while Stage 1 setup/engine/Steam acceptance is incomplete. Highest-priority findings are the weak `system.reg` setup-completion test, Steam's absent lifecycle tracking (and consequent gap in removal's running-game guard), and recovery of removal staging after abrupt shutdown. Next priorities are real Deck acceptance, independently researched engine recipes, runtime provisioning/full Steam integration, packaging, then Stage 2 saves. Small module extractions should accompany new setup/launch work rather than a broad rewrite.

## 2026-10-08 — GE-Proton target clarified

User clarified: “i mean GE-Proton, not just valve's imlementation”. Treat official GE-Proton builds as the primary runner family for configuration-driven selection/download and subsequent engine-recipe validation. The earlier Template!! compatibility test used the available Valve Proton 10 build; it does not establish GE-specific compatibility.

Tinkering planning should include GE/umu protonfixes selection for verified game IDs, visibility into applied fixes and interaction with Novelty recipes, plus controls supported by the selected GE release. Prefix tools, targeted Winetricks/dependency installation, overrides, repair and diagnostics remain applicable. Validate a clean GE baseline before adding media/font workarounds. GE documents additional media patches, protonfixes and GE-specific controls in its official README: https://github.com/GloriousEggroll/proton-ge-custom. This clarification updates the target; it does not change the installed runner or launch a game.

## 2026-10-08 — GE checklist and explicit generic setup completion

User requested “add it to the checklist and let's continue”. Added the GE-Proton target to STATUS_CHECKLIST.md: configuration-driven version pinning/downloads and verification, clean GE baseline acceptance, engine/preset overrides, verified protonfixes and visible applied changes, selected-prefix tools, targeted dependency/DLL controls, recovery/backup and one-shot diagnostics, plus release/hardware-aware GE options. These are planned increments; no GE download or runner change was made.

Continued the next foundation increment. Extracted generic preparation into a small setup.cpp module. A registry file alone no longer implies Ready: setup records an attempt and step, requires successful backend exit plus a registry file, and commits matching completion/snapshot records. Interrupted, failed, corrupt or mismatched records trigger another in-place preparation. The schema-2 successful recipe snapshot stays unchanged during ordinary launches. Runner/version, locale/environment or supported prefix-identity changes invalidate it; title/arguments and relinking retain it. A runner revision change during preparation fails completion. Explicit Prepare reruns setup using saved settings, without deleting the prefix.

Existing legacy prefixes show Needs setup and receive in-place preparation on next Play/Prepare. The sidebar explains this. TestGames was not launched, its runner was not changed and its prefix/saves were not modified in this increment. Reopen Novelty and test Template!! to exercise this transition on the real game.

Full removal is now disabled for Steam games in Manage and rejected by the core during both planning and application. Steam dispatch writes a persistent untracked-request marker before sending the URI, so a backend switch or later status update cannot enable deletion while lifecycle remains unknown. Old Requested in Steam journals are preserved into this marker before settings/status replacement. The marker is conservative, including uncertain dispatch failures; safely resolving it awaits Steam lifecycle tracking.

Validation: release build and both CTest checks pass, with **248 core assertions and UI integration**. New fixtures cover legacy setup, failure/crash after registry creation, zero exit without a registry, interrupted persisted state across reopening, corrupt/mismatched receipts, accepted/mid-preparation runner changes, locale/environment changes, replacement prefixes, explicit repair, unchanged ordinary-launch snapshots and byte-for-byte save preservation. Steam tests cover dispatch, backend switching/reopening, legacy status replacement and removal guards in both core and UI. All game/removal checks used temporary fixtures.

Remaining P1 work is explicit: surviving umu/Wine processes after a forcibly killed supervisor are not yet tracked, and abrupt removal staging still lacks a durable recovery journal. The interruption test here reconstructs persisted partial-setup state after the backend has stopped; it is not a force-kill process-tree acceptance test. Engine recipes, GE acceptance/provisioning and full Steam lifecycle remain later tested increments. No new runtime dependency or permanent worker was added.

## 2026-10-08 — Descendant supervision and conservative crash recovery

User reported that GE 7-18 crashes with “Windows 10 required”, suspected an incompatible version, and requested proceeding; then sent “continue” while this work was underway. Recorded the report in ENGINE_RESEARCH.md as an unverified failure, without assigning the cause to GE versus the existing prefix's Windows-version setting. No real game launch, prefix edit, runner switch or dependency workaround was performed.

Implemented the next P1 increment in a small runtime.cpp module. Before each umu setup/launch process starts, the headless supervisor enables Linux child-subreaper behavior and persists a runtime marker containing the kernel boot identity and diagnostic supervisor PID. After the direct backend exits, the supervisor retains its installation lock while waiting for remaining descendants, including children detached into new sessions. Setup does not receive a completion receipt before those descendants stop. Normal return/error paths remove the marker after child collection; abrupt supervisor termination leaves it behind. The earlier 1 MiB log cap and detached frontend behavior remain.

A stale marker from the current boot now produces Recovery required and blocks Play, Prepare, game edits, relinking and full removal, including plans made before the marker appeared. Status checking distinguishes a live supervisor from a recovery barrier, so reopening the UI does not monitor an absent process forever or disable unrelated games. The sidebar and action messages explain the recovery path. **This increment requires a system restart after supervisor death**: a different boot identity establishes that the old process tree cannot still run. It intentionally does not infer safety merely because a PID has exited. Corrupt markers remain blocked for inspection. Automatic recovery without reboot is still planned; Steam's separate untracked-request guard remains in force. Protection applies to new sessions started by this build.

Implementation references: Linux man-pages for [PR_SET_CHILD_SUBREAPER](https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html) and [waitpid](https://man7.org/linux/man-pages/man2/waitpid.2.html), plus [kernel boot-ID documentation](https://docs.kernel.org/admin-guide/sysctl/kernel.html). No new application runtime dependency, service or background daemon was added. Python 3 is used for the new build-time integration test.

Validation: release build and all **three CTest suites pass**: 265 core assertions, UI lifecycle integration and the new runtime-recovery integration. The latter launches real fixture process trees, verifies that a detached child delays completion, sends SIGKILL to the test supervisor during both preparation and gameplay, verifies blocked retries while the child survives, and confirms the barrier persists after the child itself exits. It simulates a later boot by editing only a temporary fixture marker, then verifies in-place retry and save preservation. Core tests cover blocked edits, relink/removal preview and application, reopening, corrupt records and prior-boot handling. UI tests verify disabled actions, Recovery required and the restart explanation. Test descendants are collected by their fixture driver; no user process was signalled. Real Wine behavior and a physical reboot still need acceptance.

Warm/offscreen 500-entry measurement: first UI event 108 ms, RSS 48,124 KiB (about 47 MiB), zero CPU ticks over two idle seconds. Reopen Novelty to load this build. The next P1 increment is a durable removal journal and recovery of staged folders after interrupted deletion; engine recipes and GE provisioning remain later blocks.

## 2026-10-08 — Continued autonomous work authorized

User: “just feel free to proceed until we run out of tokens, make sure everything is logged for next sessions”. Continuing the priority checklist in bounded, tested increments without further permission requests. Created SESSION_HANDOFF.md as a concise resumption entry point. The next active block is durable removal recovery; game compatibility changes and actual user-data deletion are not required for its implementation/tests.

## 2026-10-08 — Durable removal recovery

Completed the next authorized P1 block. Extracted removal into `src/removal.cpp` and migrated the library database to schema 2 with a removal-intent table. Intent commits before renames; game deletion and committed-cleanup authorization commit together. Staging/restoration flush their containing directories. Startup and Rescan resolve jobs in the discovery worker: restore if the removal transaction did not commit, otherwise resume previously confirmed cleanup. No further deletion confirmation is needed for a job the user already confirmed.

Recovery validates directory and parent identities, expected path structure and library references before acting. It preserves conflicting/replaced folders, never guesses at corrupt records, retains unfinished jobs and blocks launch/edit/relink/removal/reimport for affected installations/paths. A library mutation lock serializes conflicting edits/import/removal work. Remaining cleanup failures include useful messages and can be retried with Rescan. Old unjournaled staging directories are not automatically touched. Older binaries reject the new schema on opening; close old windows before using this build.

Validation: all four CTest suites pass: 265 core assertions, UI integration, process recovery and new removal recovery integration. A test-only driver abruptly exits after durable intent, each folder move, transaction commit, partial staged cleanup and per-game state cleanup. Recovery checks correct restore/delete behavior, idempotence, unchanged fixture saves, replacements at old names, swapped staging directories, conflicts, damaged records and blocked launches. A UI test opens a crashed-removal fixture and verifies background restoration plus library refresh. Destructive tests used only temporary directories. No real game folder/prefix was modified.

Limits: this validates process crashes and staged conflict cases, not physical power loss or SD-card removal. Filesystems lacking stable directory identities or directory flush support refuse recoverable removal. Automatic same-boot process recovery remains a later improvement; lost supervisors still require restart. Next autonomous block: reduce UI work while a game runs, replacing whole-library polling refreshes with updates to the affected game.

## 2026-10-08 — Targeted launch monitoring

Replaced the launch monitor's full library reload/model reset every 500 ms with `GameModel::updateState` for the affected installation. Unchanged polls emit no model changes; state/action transitions update only that row. Selection and unrelated rows remain stable. A library disconnect still refreshes the unavailable state and stops monitoring. Also closed two removal-related stale-client cases: an old discovery result cannot reimport a missing folder, and a stale Manage save cannot recreate a deleted record.

Validation: all four CTest suites pass (267 core assertions plus UI/process/removal integration). The UI check verifies one-row updates, zero model resets, retained selection, no signal for unchanged state and action recovery when the game stops. Added `scripts/benchmark.py --running`, which supervises one disposable fake game in a 500-entry library. Before the change, frontend CPU over two seconds was 33 ticks; afterward it was zero. Updated run: first UI event 135 ms, RSS 46,464 KiB. These are warm/offscreen fake-game measurements, not real GE/Gaming Mode acceptance. No user game was launched or modified.

Next: a config-pinned GE installer foundation with upstream checksum verification, side-by-side versions and runner-picker discovery. This supplies the prerequisite for a clean GE baseline; actual per-game engine compatibility remains unverified. Physical Deck acceptance cannot be established solely by automated fixtures.

## 2026-10-09 — Pinned GE-Proton installation and native controls

Completed the standard-library Python installer, managed runner discovery, and Get GE-Proton dialogs in Settings and Manage → Runtime. Exact versions are configured in `.novelty/ge-proton.json`; the checked-in example disables automatic downloading. Explicit installation uses the official release-by-tag API, selects x86_64 assets (including legacy names), verifies published length/SHA-512 and an optional configured checksum pin, validates archive paths/links, and publishes side-by-side. Previous versions remain available. Interrupted owned work can be retried; an existing verified installation is reusable offline. Unknown staging directories are preserved. Native installation runs asynchronously and supports cancellation; selecting the result still requires saving Settings/Manage. No provisioning runs automatically on Play yet.

A full real GE-Proton11-7 x86_64 download/extraction test succeeded in a disposable project-local directory: 563,784,602 bytes, configured SHA-512 verified, executable proton present, version file `1789520217 GE-Proton11-7`, elapsed 28.93 seconds. The temporary installation was removed. This verifies installer mechanics, not game compatibility; no user runner/prefix/settings were changed and no downloaded runner executed.

Validation: all five CTest suites passed, including 279 core assertions, UI lifecycle, process recovery, removal recovery, and nine Python installer tests with archive/error subcases. UI fixtures cover responsive installation, success, cancellation and request cleanup. Preview: `artifacts/ge-installer.png`. Python with `tarfile.data_filter` is required (Deck Python 3.13 supports it). Power-loss publication durability and real GE gameplay acceptance remain untested. Next block: versioned setup-profile foundation with the existing generic fallback.

## 2026-10-09 — Versioned setup profiles

Added a small compiled profile catalog/resolver (`profiles.cpp`) and a persisted per-game `setup_profile` pin. New detected Ren'Py installations select `renpy-bundled-v1`; all other engine signals use `generic-proton-v1`. Older records without the field retain generic setup. Existing pins do not follow engine-label/default changes automatically. Manage → Runtime offers an explicit profile choice and Use detected default; Cancel discards changes and Save preserves unrelated locale/environment/arguments and prefix data. Overview and launch logs expose the selected profile. Runtime now scrolls on smaller screens.

Both profiles deliberately share the already-tested generic preparation strategy. The Ren'Py baseline uses bundled files and adds no dependencies or overrides; it is not a claim of GE compatibility. Matching generic completion receipts remain valid when switching between these equivalent profiles. New receipts also record the profile for audit. Unknown/malformed profile versions remain visible for repair and fail before any backend/preparation action; they are not silently replaced. Future changes to preparation behavior require a new immutable preparation version.

Primary Ren'Py documentation was rechecked independently: distribution engine files and current Windows 10 requirements support a bundled-runtime-first baseline, but do not diagnose the user's GE 7-18 error. See ENGINE_RESEARCH.md.

Validation: all five CTest suites passed (296 core assertions). Added tests cover legacy receipt compatibility, import pins, unknown/malformed profiles with no preparation side effects, unchanged saves, UI reset/cancel/save and preservation of custom settings. Reviewed `artifacts/manage-runtime.png`. Also improved the GE releases link contrast. No real game or prefix changes. Next block: native umu installation from Settings.

## 2026-10-09 — Native umu installation

Settings now offers Get umu alongside Get GE-Proton. The asynchronous dialog installs/uses the pinned upstream umu 1.4.4 zipapp, reports failures, supports retry/cancellation and places the result in the executable field. Saving Settings applies the library-wide executable choice. The helper ships with the installed app. Installer cancellation now first requests termination, with a two-second forced-stop fallback.

The helper verifies both the pinned archive SHA-256 and extracted zipapp SHA-256, enforces size/type limits, uses a per-version lock and installs side by side at `.novelty/tools/umu-1.4.4/umu-run`. It never overwrites an existing differing installation, follows no managed-path links, and rehashes the executable for offline reuse. The legacy `.novelty/tools/umu-run` remains intact and has priority in default lookup when system umu is absent. If neither system nor legacy umu exists, the new managed path supplies the default. Custom Settings paths still take precedence. SIGTERM cleans normal temporary work; an abrupt kill during final publication may leave a small hidden staging directory, which is not broadly deleted.

Validation: five CTest suites passed, 300 core assertions and 14 Python tests. Native fixtures cover Settings access, error/retry, event-loop responsiveness and cancellation preserving a prior version. Python checks cover offline reuse, concurrent installers, legacy retention, checksum/size failure, archive ambiguity, links and existing modified files. A real official download verified the existing 430,080-byte archive pin, installed the 419,388-byte executable and passed reuse with networking disabled. The disposable installation was removed; no runner or game executed. Reviewed `artifacts/umu-installer.png`. First-run Steam Linux Runtime downloading and clean-machine acceptance remain pending.

Next block: Wine configuration/registry tools under existing per-game supervision.

## 2026-10-09 — Supervised Wine prefix tools

Manage → Runtime now has Wine configuration and Registry editor actions. They use saved game settings, initialize/verify the prefix if needed, and invoke the prefix's actual utility through the selected umu/Proton configuration. Game-specific arguments are omitted. The command uses an absolute prefix path, avoiding a same-named executable in the game directory. Utility locations were checked against Wine sources and read-only inspection of the existing test prefix: `drive_c/windows/system32/winecfg.exe` and `drive_c/windows/regedit.exe`. No real utility was opened or user registry changed.

Tools run through the existing detached supervisor, per-game lock, descendant tracking and persistent recovery barrier. Games/preparation/other tools cannot share the prefix concurrently. The UI may close while a tool remains open. Status is Configuring; completion/failure is recorded in ordinary bounded launch logs/status. Steam tools are disabled because that adapter does not own/track its prefixes. Unknown tool names and missing utility files fail without executing a fallback program. Opening a tool preserves saved settings and setup receipts; edits made inside the tool intentionally persist in that prefix and are not rolled back by selecting a profile.

Added headless `--run ID --prefix-tool winecfg|regedit`, with explicit action validation and support for equals-form options. Tests cover exact utility/runner/prefix/locale selection, omitted game arguments, automatic preparation, unavailable/unsupported/Steam tools, nonzero exit reporting, existing saves and lock exclusion. UI integration clicks the actual action and verifies survival after closing the window. Process integration uses real detached descendants/SIGKILL during a prefix-tool session and proves blocked game/prepare/tool retries. All five suites passed: 322 core assertions, UI, runtime recovery, removal recovery and 14 Python installer tests. Real GE tool-window behavior remains an acceptance gate.

Reviewed the Runtime preview and constrained prefix-label/runner-picker size hints so long paths can fit within the dialog. Final validation/performance results follow below.

Final responsiveness checks after the new dialogs/profile/tool support: a 500-entry warm/offscreen idle library reached the first UI event in 146 ms, used 49,276 KiB RSS, and consumed zero frontend CPU ticks over two seconds. With a supervised fake game: 140 ms, 46,684 KiB, zero ticks. These measure mechanics on this Deck, not cold startup, a real Wine workload or Gaming Mode. Session handoff and ordered checklist are updated with the next patch/installer-executable increment and the remaining real GE/platform gates.

Final build after the sizing adjustments passed all five suites again (322 core assertions, 14 Python installer tests; total 14.11 seconds). No disposable GE/umu validation directories remain. No unfinished implementation or test process was left running.

## 2026-10-09 — One-time patch/installer execution

Implemented Manage → Runtime → Run another executable for an existing umu installation. The dialog selects a Windows `.exe`, optional working directory (default: selected executable's folder), and literal argument lines. Run uses saved runner/prefix/locale/environment settings, performs required prefix preparation and leaves the stored game executable/cwd/arguments unchanged. Cancel launches nothing. Steam and MSI support are not added. All automated execution uses disposable fake backends; no real game, patch or installer was run.

The same detached supervisor, prefix lock, descendant tracking, log rotation and persistent crash barrier cover these operations. `.novelty/installations/<ID>/extra-launch.json` records the selected paths/arguments, umu, runner/version, profile/prefix, timestamps, state and exit result; one previous record is retained. Manage → Logs displays the latest record. Normal Play still runs the saved game; after a safely resolved interruption, normal Play marks the old operation interrupted rather than retrying it. Installer changes are intentionally persistent, and retries remain explicit user actions.

Added CLI `--run ID --extra-exe=/absolute/patch.exe [--extra-cwd=/absolute/directory] [--extra-arg=value ...]`. Options are mutually exclusive with Prepare and built-in prefix tools. Equals syntax supports option-like/empty arguments without a shell. Paths, reserved Windows names and argument size/null characters are validated before preparation, then executable/cwd/umu paths are checked again before execution.

Temporary path references are published under the library mutation lock. Removal/recovery inspect active extra-operation references and refuse deleting another game's referenced folder, prefix or state directory, including when applying a previously confirmed removal plan. Extra launches reject all pending removal targets (including per-game state). Malformed active operation states fail closed for removal. An interrupted operation's references remain protected by the existing process barrier.

Extracted command construction/supervision into `src/launch.cpp` (192 lines at this checkpoint), reducing `core.cpp` to 487 lines without a plugin framework. Added core tests for preserved settings/saves, preparation, literals/default and custom cwd, invalid requests, failures/history, return to normal Play and stale removal plans. UI tests click the real dialog controls, cancel and run, validate inputs and destroy the launcher while the fake installer continues. Process tests exercise exact CLI arguments, detached children, SIGKILL, retained interruption evidence and safe recovery without patch replay. Removal integration checks pending game/prefix/state exclusion. Preview reviewed: `artifacts/run-executable.png`.

Next independent increment: one-launch debug logging and an inspectable diagnostic export. Real GE gameplay/tool/patch acceptance and the outstanding base-platform gates remain pending; launcher-managed saves remain Stage 2.

Final verification: all five CTest suites passed in 14.95 seconds, including 358 core assertions, UI lifecycle, process recovery, removal recovery and 14 Python installer tests. The built app and previews are current. Logs/checklist/handoff are updated, and no test/build process remains running.


## 2026-10-09 — Add game from folder or external installer; card revision

Replaced Add game’s executable picker with Select folder and Run installer tabs. Folder selection inspects an existing folder and imports it without moving files; a previously registered folder keeps its configuration. Installer selection accepts a Windows `.exe` anywhere accessible, a new direct-child folder name inside Games and a Proton runner. It validates the runner, umu, source `.exe`, destination name and existing-path conflicts before creating the game entry. The dialog displays the physical destination and tells the user to choose `N:\` in the installer. A new installation uses umu and a dedicated prefix even if the ordinary-import default is Steam. Installer-specific automatic destination flags and MSI are not supported.

The installer executes from its own source directory to find adjacent media. Its game entry is pending and cannot Play until files are detected. The existing detached supervisor, prefix preparation, descendant tracking, per-game/mutation locks, log, operation record and crash barrier cover the install. After preparation, the supervisor maps this game’s destination to Wine drive N:, refusing an existing conflicting mapping. Once the installer and descendants close successfully, detection scans two folder levels, selects a sole game executable, and checks for a nested engine beside it. Multiple executables need a Manage choice; no executable leaves the entry pending. The original ID, prefix and settings remain. A nonzero installer leaves the entry pending, files intact and a clear error in status/logs. Manage → Overview provides explicit Run / retry installer, editable source path and Detect installed files; a completed operation can be finalized on startup/Rescan without executing it again. Pending external installer sources are protected from another game’s full removal. Prefix tools can prepare a pending entry before retrying.

Updated the grid to follow the user’s `cards.png` reference, copied to `docs/design/cards-reference.png`: full card image/placeholder, bottom fade, title, engine, status dot, Play and ellipsis menu with Manage/Delete. Delete routes through the same typed permanent removal confirmation. Deck-width grid now fits four portrait cards; smaller windows fit two. List headings and cell text use aligned left padding. Metadata remains placeholders. Previews: `artifacts/layout-grid-fixture.png`, `layout-list-fixture.png`, `layout-small.png`, `add-installer.png`.

Validation used disposable fake Windows installers and backends only. Core checks cover invalid inputs and non-overwrite, external adjacent media, pending Play refusal, mapped destination, nested Ren’Py/profile selection, failed/empty/ambiguous installs, completed detection recovery without replay, drive conflict and installer-media removal protection. UI integration covers folder and installer tabs, closing the UI while installation continues, card menu removal cancel/confirm and aligned list. The process suite added a real supervisor SIGKILL with an installer descendant, demonstrating persistent retry barrier and retained prefix save bytes. **All five CTest suites passed: 396 core assertions**, UI lifecycle, runtime crash recovery, removal recovery and 14 Python provisioning tests (latest run 2026-10-09 15:38 IST). No supplied game, real installer, Proton runner, user prefix or save was changed by these tests. Physical GE/umu installer window and N: visibility still need Steam Deck acceptance.


## 2026-10-09 — One-launch Wine debug and diagnostic report

Added Manage → Runtime → Launch once with Wine debug. This runs the saved umu game with Wine exception and loader traces (`WINEDEBUG=+seh,+loaddll`) for one invocation, after any required normal prefix preparation. The user’s saved environment, executable and arguments remain unchanged. The current `launch.log` identifies the debug run and retains the existing 1 MiB output cap; the next ordinary launch rotates it to `launch.log.previous` and uses the saved environment. The headless equivalent is `--root ROOT --run ID --debug-wine`; prepare, installer, prefix tools, extra executables and Steam do not accept this debug option.

Manage → Logs → Export diagnostic report saves readable JSON atomically to a user-chosen location. It includes selected game paths/engine/profile/runner, visible status, setup/recipe/operation/runtime state and current and previous launch logs (each capped at 1 MiB). It deliberately omits custom environment values, game arguments and full setup receipt configuration; environment variable *names* remain to aid diagnosis. The Logs tooltip tells users that the report contains paths and log output so they can review it before sharing. Damaged JSON state is represented as a read error rather than blocking the report. No automatic upload or networking is involved.

Validation used temporary game folders and fake umu backends. Core tests prove the debug override is per launch, normal Play restores the saved Wine setting, a report includes the log while omitting known fixture secrets, and damaged status remains exportable. UI tests exercise the save dialog and Runtime action; CLI/runtime tests check option rejection and execution. **All five CTest suites pass: 410 core assertions**, UI lifecycle, runtime crash recovery, removal recovery and 14 Python provisioning tests (latest complete run 2026-10-09, 16.97 seconds). Real GE log usefulness remains a device acceptance check; no supplied game or user prefix was launched by these tests.

Final validation after adding the previous-log field passed all five suites again (16.97 seconds, 410 core assertions). No build/test process remains running.

## 2026-10-09 — Japanese installer text and move-on-add folders

The user's `installer.png` showed mojibake in an old Japanese InstallShield dialog; a copy is at `docs/design/installer-text-reference.png`. Verified the Deck has `ja_JP.UTF-8` and Japanese fonts. Add game → Run installer now offers System default / Japanese text (legacy installer). The choice is stored before prefix preparation and passed as `HOST_LC_ALL` and `LANG` to the actual umu/Proton installer process, then retained for game and one-time executable launches. Manage → Runtime labels its editable locale control clearly and explains how to Save and explicitly retry a completed installer through Run another executable. Official Proton documentation describes `HOST_LC_ALL`; a reported Proton Japanese-locale case needed `LANG` too. This is fixture-tested, not confirmed against the user's installer. The AISp@ce test-library entry and its prefix were inspected read-only and were not rerun or modified.

Per the subsequent user correction, Add game → Select folder now moves a chosen external game directory into the selected Games root and scans/configures it there. Direct children are scanned in place. Existing registered umu entries outside Games retain ID, prefix, executable mapping and settings. The transfer rejects collisions, protected directories, nested/overlapping game folders, other games' runtime/installer/active extra-operation paths and removal reservations before moving. Same-filesystem moves rename. Cross-filesystem moves copy to a hidden Games staging path, compare file contents and symlink targets, publish by rename, then retire the source; failure before publication keeps the source. The UI performs the transfer in a worker under a modal progress notice. A hard crash during copying can leave a hidden staged copy; it is not autoimported and needs manual review until recovery is added. No real user game folder was moved by automated tests.

Core fixtures cover move+detect, direct-child idempotence, collisions/source preservation, protected and nested folders, mounted-drive root rejection, preserving a registered external game's settings, and a cross-filesystem transfer through `/dev/shm` when available. UI integration clicks Select folder and verifies the folder moved into Games with the executable pointing there; installer UI chooses Japanese locale, and a fake backend records both locale environment variables. A pending Rescan result now waits until the folder transfer finishes before touching the library. Final release build and all five CTest suites pass in 19.01 seconds: **432 core assertions**, UI lifecycle, runtime recovery, removal recovery and 14 Python installer tests. Physical Japanese installer text and SD-card behavior remain Deck acceptance checks. No test/build process remains running.

## 2026-10-09 — Simpler Delete confirmation

Replaced the typed-name, large-path removal dialog with a compact Yes/No warning for both Manage → Remove game and card Delete. No is the default. The warning names the game and states that the game folder, prefix with local saves, settings and logs are permanently removed. An unchecked, disabled **Delete synced saves** checkbox shows the future save-sync choice without claiming to delete synced data today. The core removal plan, filesystem identity checks, running-game guards, durable recovery and worker cleanup are unchanged.

Updated the UI integration fixture to exercise No and Yes from both entry points, check the default choice and disabled checkbox, and verify that No preserves the game and prefix while Yes removes the disposable fixture. Reviewed `artifacts/removal-confirmation.png`. Final release build and all five CTest suites pass in 18.19 seconds: 432 core assertions, UI lifecycle, runtime recovery, removal recovery and 14 Python provisioning tests. No user game or save was deleted by these tests.

## 2026-10-09 — Manual VNDB metadata and offline cache

The user brought metadata forward ahead of saves and chose to show every distinct publisher listed across a VN's releases. Manage now has one Metadata tab: search by title, select/link a VNDB result, Refresh, or Unlink. It displays the original title, description, clickable VNDB ID, expected play time, developers, publishers and non-spoiler content tags. The sidebar shows the original title in small type below the local folder title, plus cached description, tags, play-time estimate and cover; grid cards also use the cover. Unlinked games keep their generated art/placeholders. Launch settings and the game folder name are independent of the VNDB match.

The official Kana API provides VN details and paginated release producers; only producer entries marked publisher are aggregated. Metadata text is cached in the existing SQLite game JSON record; the bounded, decoded PNG cover lives under the game's Novelty installation directory. Search/Refresh/Link are explicit network actions. Startup, scan, selection and Play do not fetch metadata. Unlink removes only the metadata and its cover. Descriptions render as plain text, and the UI does not interpret remote markup.

Qt Network was added to the native build; the Deck's local Qt header bootstrap and SDK link path were updated for the matching installed runtime. A local HTTP fixture tests title search, linking, two release pages, content-tag filtering, original title, cover download, offline reopening and unlinking. The Metadata preview at `artifacts/manage-metadata.png` was reviewed; search results now collapse after linking so details stay visible. The final build passes all five CTest suites (432 existing core assertions, UI lifecycle, runtime recovery, removal recovery and 14 Python installer tests; 20.57 seconds). A live read-only Kana search and VN detail request also succeeded. The 500-entry warm/offscreen idle check reports first event **154 ms**, **53,984 kB RSS**, **zero frontend CPU ticks / 2 sec**. No supplied game or prefix was changed. See [VNDB Kana API](https://api.vndb.org/kana) for the fields and release pagination.

## 2026-10-10 — Process monitor and interrupted folder import recovery

The user asked to continue with the two P1 recovery gaps. New umu operations now record `runtime.json` before forking a separate process monitor. That monitor is the backend parent and Linux child subreaper. If the headless caller is killed, the monitor still waits for detached descendants and removes the barrier only after they exit. A same-boot retry then works; running descendants still block it. If the monitor itself dies, or an old marker has no surviving monitor, the conservative reboot rule remains. No process scanning, forced termination or automatic installer replay was added. The user-facing recovery message now distinguishes waiting for a live game/tool from a marker that remains blocked.

Cross-filesystem Select folder now writes `.novelty/imports/<id>.json` before creating/copying its hidden stage. It records the source and stage directory identities. Startup/Rescan recovers an incomplete copy under the library mutation lock only if the original still has its recorded identity, the stage has its recorded identity and the destination is absent. Changed/conflicting copies and old unjournaled stages are kept for inspection. There is a narrow crash interval before the stage identity is recorded; its copy is also kept for inspection. Successful transfers clear their journal. No user game was moved in these checks.

Build succeeded. Core tests (**438 assertions**, including new import recovery and destination-conflict cases), real SIGKILL runtime recovery for setup/game/prefix tools/one-time executables/installers, removal recovery and Python installer suites passed. The runtime fixture also kills the caller's whole process group while the monitor runs in a separate session. The UI lifecycle suite could not start its local VNDB TCP fixture because this sandbox forbids socket creation (`Operation not permitted`); its prior full pass remains the 2026-10-09 baseline. Tests used temporary libraries and did not touch `/home/deck/TestGames`. Real Deck/GE behavior, monitor death and power-loss durability remain acceptance work.

## 2026-10-10 — Product scope: one game lifecycle manager

The user clarified the intended product and asked to retain it for future reference. Saved the full wording and actionable requirements in [PRODUCT_VISION.md](PRODUCT_VISION.md), which takes precedence over narrower scope statements in historical proposals. Novelty is VN first with other Proton games in scope. The default journey is executable selection, automatic metadata and game/engine/generic VN or generic game setup, Play, then continuation on another device using one-button peer-to-peer LAN game installation when needed and synced saves.

The interaction requirement is one fast, self-contained application with progressive disclosure. Metadata correction, manual save paths, Proton changes and advanced configuration remain available without becoming mandatory steps for ordinary play. Preserve editable configuration and user overrides; defaults should be opinionated while capabilities remain flexible.

Updated README, session handoff, staged plan, priority checklist and UI guidance to reference this direction and distinguish intended behavior from current implementation. Local lifecycle reliability remains the foundation; LAN transfer ordering is to be planned with cross-device saves. This was a documentation update only; no application code or runtime settings changed, and no tests were needed.

## 2026-10-10 — Reordered Lego priority list

The user requested an updated priority list before further implementation. Reworked STATUS_CHECKLIST.md into eight delivery blocks with an outcome and acceptance gate each: P1 executable-based Add, P2 automatic setup/runtime provisioning on Play, P3 automatic metadata, P4 packaged Deck/desktop local lifecycle, P5 local save mappings/snapshots/restore, P6 paired-device save sync for installed games, P7 LAN game installation, and P8 optional Steam integration and further controls.

Real GE/Deck acceptance, UI-suite rerun and remaining recovery limits now form a validation track alongside delivery. Relevant defects still block affected paths and unpassed acceptance gates block release claims. Full Steam shortcut work moves after the core cross-device journey; Gaming Mode acceptance remains an early platform requirement. Direct LAN peer sync is the planning default for P6, with protocol details to be resolved during implementation.

Updated PRODUCT_VISION.md, MVP_PLAN.md and SESSION_HANDOFF.md to use this order. The next implementation block is P1, selecting a game's executable and reaching Play with existing defaults. Documentation only; no application changes or tests in this update.

## 2026-10-10 — Corrected Add flow and narrowed Steam scope

The user clarified that executable selection happens after adding a game folder or installing a game. The earlier executable-first priority was an incorrect interpretation. Source review confirms folder import already auto-selects a sole candidate and derives its working directory; Manage → Launch provides manual correction. The remaining P1 UI gap is a direct executable-selection step after an ambiguous import or installation.

The user also questioned treating P1–P4 as entirely new features. Updated the checklist to state what already works in each: folder/installer import and executable selection; prefix preparation, generic/Ren'Py profiles and runtime installers; manual VNDB matching/download/cache; native application/local build. Remaining work is the narrow selection step, missing-runtime automation and fuller presets, automatic metadata matching, and distribution packaging/clean-device acceptance. These should extend existing behavior rather than rebuild those areas.

P5 local saves, P6 paired-device sync and P7 LAN game installation remain accepted. P8 is now only Add to Steam, targeting Novelty's managed launch. Removed the broader Steam adapter and integrations expansion from active planning while retaining existing editable controls. Corrected PRODUCT_VISION.md, README, MVP_PLAN.md, SESSION_HANDOFF.md and UI_LAYOUT.md accordingly. Documentation only; no application code changes or tests.

## 2026-10-10 — Executable selection after Add; self-contained release boundary

Implemented the remaining P1 executable selection step. Add folder still moves/scans the chosen folder and automatically uses a sole suitable executable. For several candidates or none, it now opens a chooser immediately after Add. The same chooser opens when a supervised installer finishes without a selected game executable; Manage → Detect installed files and Manage → Launch offer a later route. The user can choose Later without losing the game entry. Selecting an existing `.exe` inside that game folder saves the executable and its working directory against the same installation ID and keeps the existing prefix/settings. Core validation rejects missing, non-`.exe` and outside-folder selections. A nested executable can update a generic engine/profile when evidence exists. Manual editing of an already configured executable remains in Manage.

Added a focused offscreen UI suite that does not need a local network socket. It exercises a multiple-candidate folder Add, no-candidate Later, a simulated installer completing with two candidates, chooser selection, Play availability and persistence in a reopened library. The core suite passes **450 assertions**; focused UI, runtime recovery, removal recovery and Python installer suites pass. The larger UI lifecycle suite still cannot start its local VNDB fixture in this sandbox because socket creation is denied. No supplied game, user prefix or system installation was changed by the fixture tests.

The user strengthened the release requirement: Novelty-owned files should live in the application folder and chosen Games folder; ordinary setup/play must not require a separate Bottles, protontricks or ProtonUp-Qt installation; external prefix editors are optional. Removing a game or the app plus Games must leave no Novelty-owned OS clutter. Updated PRODUCT_VISION.md, STATUS_CHECKLIST.md, MVP_PLAN.md, README.md and SESSION_HANDOFF.md to make the storage and clean-uninstall audit an explicit P4 gate. This is a target, not a claim about the current development build; QSettings, helper/interpreter dependencies and runtime write paths need investigation during packaging. P2 first-Play provisioning should also meet the no-separate-manager requirement.

## 2026-10-10 — P2 first-Play runner recovery and generic game baseline

New unidentified game imports now pin `generic-game-v1`, while advisory VN-family detections use the conservative `generic-proton-v1` and Ren'Py retains `renpy-bundled-v1`. All three currently use the same prefix preparation; no new codecs, DLLs or engine fixes were inferred. Existing profile pins are unchanged. First Play with an empty runner now uses a valid library default, a sole installed Proton runner, an in-app choice among several, or the existing GE installer. If the library has an exact GE version pin and has not explicitly disabled automatic downloads, that installer starts automatically; otherwise it still asks for the version/one-download action. A missing default umu starts the existing in-app umu installer automatically. Explicit custom runner and umu paths remain untouched if unavailable, so the user can repair them in Manage/Settings.

Checked pinned umu 1.4.4's [runtime path source](https://github.com/Open-Wine-Components/umu-launcher/blob/1.4.4/umu/umu_consts.py): `UMU_FOLDERS_PATH` and `XDG_CACHE_HOME` select its runtime and cache locations. Newly created Games libraries set these under `<Games>/.novelty/` for all umu commands. Existing libraries keep the previous environment until an explicit migration, avoiding an unexpected runtime download or offline regression. This is one part of the app/Games folder boundary; QSettings, helper interpreter, temporary files and package behavior still need a write audit. Steam Linux Runtime download itself still needs visible progress/cancellation and clean-device acceptance.

Validation: build succeeded; **456 core assertions**, focused Add/first-Play UI (including missing-runner launch and automatic pinned installer starts), runtime recovery, removal recovery and Python installer suites pass. The larger UI suite reaches its VNDB fixture then fails because this sandbox denies its localhost socket. Its pre-fixture profile tests were updated for the new generic-game default and pass. No supplied game, user prefix or prior home-directory umu runtime was altered. A clean real GE/game compatibility baseline remains pending.

## 2026-10-10 — Generic GE version and automatic VNDB matching

The user requested that a generic profile select a Proton version and choose metadata without a manual VNDB result click. Both generic VN and generic game profiles now prefer `GE-Proton11-7`, the latest official GE release checked for this increment. First Play uses a game's explicit runner or valid library default first, then a library GE pin or the profile pin. An installed managed copy is reused; otherwise the in-app GE dialog opens with the version selected and starts unless `auto_download: false`. An unknown profile still requires repair before provisioning. The pin is an installation default, not evidence of compatibility with a particular game; the clean real-game gate remains open.

New folder Adds, completed installer detection and background imports now queue VNDB search/fetch without blocking Play. Matching compares normalized local name against title and alternate title, then falls back to the first VNDB searchrank result. Results, publishers and artwork use the existing offline cache. Existing metadata and manual relinks win over pending automatic results; Unlink cancels a pending result. Empty searches and network/storage failures leave the game playable. VNDB can be a poor fit for general games, so a general-game metadata source and ambiguous-result review remain in P3.

Validation: native build, core, focused Add/first-Play UI and socket-free matching assertions, runtime recovery, removal recovery and Python installer suites pass. The larger UI suite still cannot start its local TCP fixture in this sandbox; a new automatic Add/cover/offline test compiles but awaits a socket-capable run. No real game or user prefix was changed.

## 2026-10-10 — Card double-click and first-Play executable selection

The user removed the card Play button: double-clicking a card remains the Play action, while the ellipsis still opens Manage/Delete. A game with no executable now keeps Play available when its folder exists and it is not installation-pending or busy. On the first Play attempt, Novelty opens the same picker used after Add; Later does not start setup or launch, and saving an executable continues that Play attempt. The list's Play action uses the same path. Missing folders and unfinished installers remain blocked. The executable and working directory still persist under the existing game ID.

Validation: build and five of six suites pass, including a focused UI fixture for picker cancellation, persistence and launch continuation. The larger UI suite passes its card double-click/ellipsis checks before reaching the localhost VNDB fixture, which this sandbox cannot bind; its remaining metadata checks still need a socket-capable run. No real game or user prefix was changed.

## 2026-10-10 — Right-click cards and full sidebar covers

The user removed the remaining card ellipsis. Cards now show only the title and engine at bottom left and the status light at bottom right. Double-click launches; right-click on a card opens the existing Manage/Delete menu at the pointer. The fixed 340-pixel right sidebar now sizes a cached cover to its full aspect ratio and draws the whole image. Portrait covers grow vertically within the existing scroll area; generated placeholder art retains its prior height.

Validation: the native build and five of six suites pass. The larger UI suite passes the card right-click, double-click, status-light position and removal-menu checks before its localhost VNDB fixture fails to bind in this sandbox. The focused executable UI suite checks full portrait and landscape cover ratios and image edges. Offscreen 1280×800 and 900×620 previews were inspected under `/tmp/novelty-card-preview`; title/engine and status dot appear in the requested corners without changing the sidebar width. The cached-cover integration portion of the larger suite still needs a socket-capable run. No real game or user prefix was changed.

## 2026-10-10 — In-app Steam Linux Runtime preparation on first Play

Added a runtime preflight for new managed libraries when the selected Proton `toolmanifest.vdf` names a supported Steam Linux Runtime. Before handing Play to the detached launcher, Novelty checks umu's completed-runtime markers in `<Games>/.novelty/runtime/umu/`, then runs the selected umu with a disposable prefix under `<Games>/.novelty/`. umu remains responsible for downloading and extracting its own required runtime. Novelty displays phase text, elapsed activity and bounded umu output, with Cancel and Retry. The disposable prefix is cleaned after exit; later Plays reuse the complete runtime offline. Existing libraries and unknown custom runner manifests keep their previous behavior.

Validation: the build and five socket-free suites pass. A focused fake-umu UI fixture now checks the actual first-Play path plus failure/retry, Cancel, completed-runtime reuse and probe cleanup. The larger UI lifecycle suite still cannot bind its local metadata fixture under this sandbox. The test did not run a real GE game or download a real Steam Linux Runtime. The UI currently shows indeterminate activity rather than a byte percentage; GE/umu/runtime remain separate installer dialogs, and temporary umu download behavior still needs clean-device acceptance. Next P2 work is unifying that preparation presentation and validating real pinned GE behavior before release claims.

## 2026-10-10 — P3 accepted at current scope

The user decided to skip further P3 work for now. Automatic VNDB matching, cached metadata/artwork and the existing manual browse/relink/refresh/unlink controls are sufficient for MVP. Ambiguous-result prompts and a separate general-game metadata provider are deferred refinements, not active gates. Metadata still must not be treated as evidence that a game-specific compatibility preset is safe. Updated the priority checklist, product vision, staged plan, README and session handoff. This was a scope/documentation change only; no app code changed and no new test run was needed. The next active implementation remains P2 setup, then P4 packaging and self-contained storage.

## 2026-10-10 — Authorization through P8; portable storage and test package

The user authorized continued work through P8 without routine prompts, with intervention only where needed. Started a P4 increment while P2 real-game/setup gates remain open. The GUI now redirects its two QSettings preferences to `data/Novelty/Novelty.ini` beside the executable, copies missing values from an old settings file and removes that file only when it has no unrelated keys. New managed-library umu commands set `TMPDIR` inside `.novelty/cache/tmp` alongside their existing runtime/cache variables; per-game overrides cannot redirect it. The selected umu source's temporary-directory code was inspected to confirm it uses Python's temporary location or the managed cache. Legacy libraries retain their prior paths.

Added `scripts/package-portable.py` to stage an unpacked test package with the executable, installer helpers, selected Qt plugins and linked libraries, a wrapper and a hash manifest. The wrapper loads bundled libraries/plugins for Novelty; `externalToolEnvironment()` restores the original host library/plugin paths before Proton and Python installer commands. The package script refuses an existing output and cleans an unpublished staging directory on error. An isolated copied-binary test migrated old settings and rendered the UI, removing the old two-key config. A packaged offscreen screenshot then passed from a disposable folder with 94 copied libraries. Core tests cover child environment scrubbing and Games-local temp. The native build passes five socket-free suites; the UI lifecycle suite still cannot bind its local metadata fixture in this sandbox.

This is not P4 acceptance: the test package still uses host glibc, graphics drivers, certificates and Python 3.12+; clean Deck/Bazzite, online provisioning, uninstall and actual device controls remain unverified. P2's unified first-Play presentation, verified game/engine presets and real GE game acceptance also remain open.

An additional disposable package test moved the application directory, reopened it using the remembered Games path, rendered an offscreen screenshot and found no external config directory. Loader tracing confirmed Qt Core came from the copied package library. Manage → Runtime now previews the selected profile, Proton source and locale while keeping controls editable; the native build and five socket-free suites pass. This does not close the missing verified preset hierarchy or clean-device acceptance.

## 2026-10-10 — One automatic first-Play preparation dialog

The normal pinned GE first-Play path now uses one dialog to sequence missing GE-Proton, default umu and a recognized Steam Linux Runtime. Each step runs asynchronously with phase/activity text, elapsed time, bounded output, Cancel and Retry. A library `auto_download: false` presents Start before GE; a click enables that installation. Completed components remain available for retry, while a newly selected per-game runner is not saved until the full preparation succeeds. Direct Get GE-Proton/Get umu controls in Settings and Manage remain available; an unpinned custom profile may still need the existing manual runner chooser. Recognized runtime preparation still uses a disposable Games-local probe prefix.

The focused offscreen fixture now checks an explicit Start, a failed GE step followed by Retry, GE→umu→runtime completion within one dialog and cancellation during umu without prematurely saving the runner. The native build, core, focused UI, runtime recovery, removal recovery and Python installer suites pass. The larger UI lifecycle suite still cannot bind its local VNDB fixture under this sandbox. No real GE game or runtime download was run. Remaining P2 gates are verified game/engine presets, real pinned GE gameplay and clean-device first Play; cancellation of actual umu descendants also needs device acceptance.

## 2026-10-10 — Bundled Python in portable test package

The unpacked test package now includes the host Python 3.13 interpreter and standard library, excluding site packages and development/test modules. A small package-local wrapper sets `PYTHONHOME`, disables user site packages and bytecode writes, and loads the package's linked libraries. `src/provisioning.cpp` prefers this bundled interpreter for GE/umu installer helpers; development builds without it continue using the host Python. A disposable package successfully imported SSL, tarfile, urllib, hashlib, lzma and bz2 through bundled Python, rendered the Novelty UI, and ran the focused executable/setup UI fixture from inside the package with fake installer helpers. All five socket-free suites still pass; the larger VNDB fixture remains blocked by sandbox localhost binding.

This removes a separate Python installation from the packaged first-Play path, but P4 remains open: the package still depends on host glibc, graphics drivers and certificate store, needs license review for bundled dependencies, and has not been accepted on a clean Deck or Bazzite desktop with real online provisioning and gameplay.

## 2026-10-10 — Local save lifecycle from the cross-device RFC

Imported the two desktop save/sync notes into `docs/novelty_codex_sync_handoff.md` and `docs/novelty_sync_rfc_v0.4.md`, then mapped M0 to the existing Qt/SQLite game supervisor in `docs/SAVE_M0_IMPLEMENTATION_PLAN.md`. The RFC sets the MVP boundary at local saves, accountless LAN sync and optional LAN game transfer; server/accounts/WAN are later work. The setup `recipe.lock.json` remains separate from trusted declarative save recipes.

Implemented a library-local profile UUID, stable profile-game UUID, portable game/AppData/Documents roots, explicit discovery states and candidate evidence, exact-match declarative save recipes, a verified Ren'Py rule and bounded before/after observations across launches. Normal Play observes while searching, captures mapped saves after the tracked process exits, and leaves durable work for startup/Rescan recovery. Empty known roots do not create a snapshot or block Play. Manage → Saves exposes manual mappings, candidate confirmation, state controls, snapshot history and restore.

Snapshots have hashed manifests, ancestry/provenance fields and duplicate detection. Restore verifies before replacement, journals rollback and supports an empty destination at a different absolute game folder/prefix. Fixture tests cover three save-free launches followed by a save, ambiguous changes, log/config exclusion, recipe edition matching, Ren'Py literals, schema-1 migration, symlink/traversal rejection, interrupted staging, and portable restore. The native build and five socket-free suites pass with 534 core assertions. The larger UI lifecycle suite reaches the local metadata fixture and fails because this sandbox cannot bind its socket. M0 still needs real-game load after restore, target-device interruption/unavailable-drive checks and clean Deck acceptance before LAN transfer is claimed.

## 2026-10-10 — Steam shortcut convenience

Added Manage → Add to Steam for an existing umu game. It writes the selected local Steam account's binary `shortcuts.vdf` atomically and invokes Novelty's headless `--run` path through the portable package wrapper when present, preserving the saved Proton, prefix and save lifecycle. The parser refuses unsupported or damaged files and preserves unrelated entries; a profile/game marker prevents duplicates and repeat Add updates the app/Games paths after a move. Unit fixtures pass with 552 standard core assertions, and a temporary copy of this Deck's real Steam shortcuts file passes with 557 assertions. No live Steam file was edited. Desktop/Gaming Mode launch and Steam's behavior after restart still need a device check; removal of previously added Steam shortcuts is not yet handled automatically.

Prepared a disposable 3.9 GB Template!! copy under `.acceptance/Games` for the M0 real-game gate. The dedicated test library created an 8.2 MB verified snapshot of the existing save folder, removed only the copied save folder, restored into the empty location and verified slot `1-1-LT1.save`. Launching from the coding sandbox reached umu but failed before Proton because socket creation is denied (`Operation not permitted`); `./.acceptance/run-restored-game.sh` is ready for a normal Deck terminal and a visual slot-load check. The original `/home/deck/TestGames` installation and its saves were not changed. Remove `.acceptance` after the user-confirmed test.

The user launched that disposable restored copy outside the sandbox and saw the expected save slot. Novelty returned to Ready after exit. The slot's bytes match the source; choosing Load and resuming gameplay in the restored copy has not been explicitly confirmed.

## 2026-10-10 — LAN sync identity and trust foundation

Added a persistent P-256 self-signed device certificate and private key under each library profile's `.novelty/devices/<profile ID>/` directory. Generation uses the linked OpenSSL API inside Novelty. The key and directory are owner-only; reopening verifies key/certificate match, self-signature and validity, and damaged or missing files stop pairing instead of silently changing identity. Relocating a Games library retains its fingerprint. Qt parses the generated certificate and agrees on the fingerprint; private-key loading through Qt still awaits a full Qt SDK because this development fallback lacks `QSslKey` headers.

Added an exact-fingerprint peer-pin store scoped to the same profile, with owner-only records, duplicate prevention, label updates, revocation and fail-closed malformed-record behavior. This is storage for a future verified approval, not a pairing UI or network authorization path yet. A separate library has a separate profile/device identity and no inherited trust. The native build and socket-free tests pass with **594 core assertions**, including identity persistence/permissions, cross-profile isolation, corruption, pinning and revocation. TLS connections, discovery, verification phrases, authenticated transfer, before-Play reconciliation and two-device acceptance remain open P6 work. The coding sandbox cannot bind sockets, so no LAN behavior has been claimed.

## 2026-10-11 — LAN protocol primitives

Added a versioned, fixed-size discovery datagram containing only a random session ID and listener port; an 80-bit verification phrase derived from both certificate fingerprints and fresh 256-bit nonces; and versioned control frames with a 64 KiB payload limit and bounded receive buffer. Both pairing sides derive the same phrase regardless of which initiates. Invalid fingerprints/nonces, unknown versions, malformed sizes and oversized messages are rejected. Qt TLS setup now loads the existing certificate and P-256 private key through the path overload, requires peer verification and TLS 1.2 or later, and limits its read buffer. Certificate helpers reject invalid self-signatures, dates, unexpected TLS errors and wrong pins; application authorization also requires an encrypted socket and an exact profile-local trust record. The fixture exposed that Qt's `isSelfSigned()` does not recognize Novelty's leaf certificate, so OpenSSL verifies its signature directly.

These are still connection primitives: no UDP listener, live TLS session, human approval or save transfer is wired yet. The sandbox cannot create the localhost socket used by the metadata UI fixture. The build passes the five socket-free suites, with **617 core assertions**; the sixth UI lifecycle suite still fails to start that fixture. No real game, save or trusted-peer record was changed by these tests.

## 2026-10-11 — Manual paired-device saves path

Added `LanConnection` for Qt TLS client/server streams. Only the generated self-signed leaf error is allowed; after encryption a previously paired connection needs an exact certificate fingerprint pinned to the current Games profile. Pairing exchanges fresh nonces over TLS, shows the same certificate-bound phrase on both screens and writes trust only after both people approve. `Window::devices` displays the local identity and paired devices, lets the user revoke a pin, and offers a manual LAN-IP/port pairing dialog. While a library is open, Novelty listens for trusted save requests on port 38373 or an available fallback port. The app stays usable when listening is unavailable.

For saves, confirmed existing installations can explicitly link differing profile-game IDs to one trusted fingerprint without rewriting local history. Import and restore still require matching game, edition, save-format and mapping labels; wrong editions and unlinked IDs are rejected. `LanSaveExchange` can locate a matching game, stream a verified immutable snapshot in bounded chunks to Games-local staging, verify each file, then call the existing guarded import. Manage → Saves offers **Sync from peer…** with a manual address. An ahead/available import offers a guarded restore; a conflict keeps both snapshots for explicit selection. No automatic restore is attempted while the game is running.

The release build and six socket-free CTest suites pass, including a focused Devices screen check and **620 core assertions**. `lan_integration` now includes a real two-sided TLS pairing and snapshot import/restore fixture for a TCP-capable environment; it skips at bind in this sandbox, as does the older VNDB UI lifecycle fixture. A Unix socket pair did not advance Qt's TLS handshake here, so it was not used as a substitute for the TCP test. No real peer, game folder or user save was changed. This is not M1 acceptance: UDP discovery, automatic before-Play/after-exit reconciliation, a durable offline queue and two-device validation remain open. The Template!! Load action after the previously seen restored slot also remains unconfirmed.

## 2026-10-11 — Automatic LAN save lifecycle and staged peer game install

Added a profile-local, atomic pending-save queue keyed by trusted peer and game. Manual capture and tracked post-exit capture replace that peer's pending head; an exact acknowledgement clears it without discarding a newer capture. The window retries offers to remembered endpoints while open, and an incoming trusted offer fetches and verifies the snapshot before acknowledging durable receipt. An offline or sleeping peer leaves the queue intact. A receiving installation keeps its applied save unchanged until Play or explicit manual restore. A new `importedSaveCandidates` view prunes older snapshots on the same branch and keeps divergent branches visible.

Ordinary Play now checks remembered, linked peers for compatible save histories, fetches missing ancestors in order, and restores one safe newer branch before launch. The headless `--run` path used by Steam shortcuts runs the same check; conflicts stop headless launch with a direction to Manage → Saves. The GUI handles conflicts by preserving both copies and asking whether to play current local progress. Manual Save Sync uses the ancestry fetch after a game link is confirmed. These behaviors compile and the socket-free suites run; live TLS/data transport still awaits a host with TCP binding and a two-device check.

Added `LanGameExchange` on the same authenticated connection. Add game offers **Install from paired device**, lists a bounded catalogue, scans the source manifest off the UI thread, streams verified files to a hidden Games-local stage, resumes complete staged files on retry, and refuses to replace an existing game folder. A completed transfer registers a local umu game with relative executable/cwd, arguments, locale, bounded metadata and save identity/mappings; local runner and prefix remain device-specific. It then checks peer saves and enters the existing runtime preparation path. The TCP fixture now covers a new device installing and reconciling saved progress, but its live portion skips in this sandbox. Large-transfer performance, interrupted retry, real game launch and differing-build/device acceptance remain open.

## 2026-10-11 — LAN install recovery and manifest checks

Completed staged game files are now hashed by a worker before retry, keeping that scan off the UI thread. Newly received files retain their streaming SHA-256 verification; final publication checks the verified file set, sizes, links and unexpected paths. The stage marker pins peer, profile game, release and save format while allowing refreshed display metadata on a retry. The complete manifest is validated before file requests, including relative paths, declared parent directories, exact byte total, executable/working directory, save mappings and duplicate entries. A new socket-free core fixture rejects malformed manifests. The rebuilt native binary passes 645 core assertions and six socket-free CTest suites; `lan_integration` still skips its live TCP path here. The latest disposable package is `/tmp/novelty-mvp-package-20261011-b`, and its wrapper reports `Novelty 0.1.0`. Two-device install, interruption, gameplay and save-load acceptance remain open.

## 2026-10-11 — Remaining local implementation and P2P visibility

Added an explicit Settings action for libraries created before Games-local umu storage. It copies complete legacy Steam Linux Runtime directories and the shim into an owned Games-local stage, preserves symlinks and executable permissions, verifies file bytes and a final tree digest, publishes the copy, then updates the library's storage mode. The original home-directory runtime remains untouched. A cancellation or unknown destination leaves the library on its prior storage. The copy runs on a worker with an independent SQLite connection so Settings remains responsive. Fixture tests cover cancellation, collision, successful cutover and source retention; the user's real runtime was inspected read-only and was not migrated.

The pairing dialog now broadcasts and lists bounded UDP discovery hints while keeping manual IP and the two-sided phrase approval. The selected-game panel shows queued outgoing saves, an incoming newer save or a conflict. The preset resolver now has a tested exact game/release → engine → generic order, and Manage shows the recommendation source; no unverified game-specific recipe was added. The GUI directs its own cache and temporary directory beside its executable. The rebuilt binary passes 665 core assertions and six runnable suites, with the LAN suite's live TCP portion still skipped here. The latest disposable package is `/tmp/novelty-mvp-package-20261011-c`; its offscreen window opened from a disposable Games folder. Real-device migration, setup, Steam and P2P acceptance remain open, with P2P device testing intentionally last.

The final rebuild after improving cancellation of a large migration still passes the six runnable suites. A fresh `/tmp/novelty-mvp-package-20261011-d` package opened offscreen from the disposable Games folder. This package supersedes the `-c` test package above.

A final migration review aligned its published layout with the first-Play runtime probe: `<Games>/.novelty/runtime/umu/steamrt*`. The core migration fixture passes with that layout. The latest package is `/tmp/novelty-mvp-package-20261011-e`, superseding `-d`.

## 2026-10-11 — Wider save discovery after Amairo Chocolata stayed Searching

The user reported that Amairo Chocolata remained Searching after Play. The prior observer only compared named save folders and save-like filenames. Normal Play now records bounded file metadata across the game folder and the Wine prefix's C: drive before launch, then compares it after exit. New and changed files in unusual folders become reviewable folder candidates, even with nonstandard names. Candidate evidence includes example filenames. These broad findings require manual confirmation. A focused, save-like location still auto-identifies when it is the only distinct candidate. Nested suggestions are collapsed under a confirmed focused location. Observation is metadata only; real snapshot capture still copies and verifies files after a mapping is confirmed. The UI tells users to create a snapshot after confirming a candidate from an already completed Play.

The release binary and core fixture build pass. The focused core suite passes **676 assertions**, including unusual game-folder, AppData/Roaming and AppData/Local locations, a changed existing file with an unfamiliar name, and compatibility with an observation from the older build. The full CTest run before the final fix had the expected localhost-socket `ui_lifecycle` failure and a save-discovery fixture failure; the latter was fixed and the core suite rerun successfully. The new disposable package is `/tmp/novelty-mvp-package-20261011-f`; its binary hash matches the rebuilt binary and its wrapper reports version 0.1.0. No Amairo save or prefix was modified by these fixtures. The user's next Amairo Play must establish that its actual folder appears as a candidate; confirm that folder and create a snapshot, then test restore and in-game Load separately.

The user then asked for suggestion rows to display the path where activity was found instead of file details. Manage → Saves now shows the resolved absolute location (with the same path in its tooltip), retaining the relative mapping internally when selected. The window rebuilt successfully. `/tmp/novelty-mvp-package-20261011-g` contains this UI change and supersedes `-f`.

The user clarified the desired display: omit the Games root, so an Amairo save appears as `/Amairo Chocolata/UserData` rather than `/home/deck/TestGames/Amairo Chocolata/UserData`. Both candidate and confirmed mapping rows now use that game-relative display. AppData, Documents and Prefix paths retain a scope label because they live outside the game folder. The stored mapping remains scope plus relative path. The window rebuilt successfully; `/tmp/novelty-mvp-package-20261011-h` supersedes `-g`.

The user then asked to remove the Game folder/AppData/Documents/Prefix dropdown. The Saves tab now has one path field. A typed `UserData` or displayed `/Amairo Chocolata/UserData` resolves to a game-relative mapping; File… and Folder… browsing accept locations in the game or managed Wine prefix and infer the portable scope. Other saved mappings display as `AppData:/…`, `Documents:/…` or `Prefix:/…`, which the same field accepts. Paths outside the game and its prefix are rejected. Core checks pass **680 assertions**, including scope inference and traversal rejection. The rebuilt window has no compiler warning from the new callback. `/tmp/novelty-mvp-package-20261011-i` supersedes `-h`; actual Amairo discovery and save-load acceptance are still awaiting the user's test.
