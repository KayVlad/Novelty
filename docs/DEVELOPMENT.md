# Build and test the first launcher increment

## Current executable

`build/novelty` is a release build for this Steam Deck. `run.sh` opens it; `Novelty.desktop` is a local launch file pointing at this project and `/home/deck/TestGames`.

The build uses system Qt 6.9.1 libraries and plugins. Packaging with bundled dependencies and Gaming Mode/controller validation are still pending. No Steam shortcuts or Steam configuration files were modified.

## Build

On a normal development system, install a C++20 compiler, CMake, Ninja and Qt6 Core/Gui/Widgets/Sql/Network development packages, then run:

```sh
./scripts/build.sh
```

This session could not start Distrobox/Flatpak build namespaces. The working fallback uses the already-installed Freedesktop SDK compiler with project-local sysroot links and Qt 6.9.1 public headers. Headers were retrieved from the official `qt/qtbase` tag; generated feature/export headers match the host's qmake module configuration. These development files are under ignored `.tools/`; no system packages were changed. Prefer a full SDK for future releases.

To recreate the local header fallback from a normal shell with internet access:

```sh
python3 scripts/bootstrap-qt-headers.py
./scripts/build.sh
```

The bootstrap intentionally refuses another host Qt version. The fallback assumes the existing Freedesktop SDK 25.08, host binutils and this SteamOS layout. Normal CMake builds do not use this fallback.

## VNDB metadata

Add folder, completed installer detection and background discovery search the [VNDB Kana API](https://api.vndb.org/kana) for the local game title without delaying Play. An exact normalized title or alternate-title hit wins; otherwise Novelty takes the first valid result in VNDB searchrank order. Fetch then paginates releases to collect distinct publisher names. Manage → Metadata retains manual Search, Link, Refresh and Unlink for correction. The VN record supplies the main original title, description, expected length, developers, thumbnail and `cont` content tags. Tags marked as spoilers are omitted. The matched VNDB ID, normalized text and lists are stored in the existing game's JSON record; a decoded, size-bounded PNG cover is stored under `.novelty/installations/<ID>/cover.png`. The folder title and launch configuration remain local. Unlink removes only this record and cover and cancels a pending automatic link.

The library, sidebar and cards read the cache. Startup, selection and Play do not fetch metadata; a scan fetches only newly imported games. Offline reopening works from the cache. Metadata does not measure actual playtime. The UI test uses a local HTTP fixture for search, a two-page publisher response, automatic Add, cover, offline reopening and unlinking, so routine tests do not depend on VNDB availability. The API is public for these reads; no user credentials are stored.

## umu provisioning and real game test

```sh
python3 scripts/install-umu.py /home/deck/TestGames
./run.sh --root /home/deck/TestGames
```

The installer pins umu 1.4.4's upstream zipapp archive and SHA-256 from its [GitHub release](https://github.com/Open-Wine-Components/umu-launcher/releases/tag/1.4.4). It installs `.novelty/tools/umu-1.4.4/umu-run` side by side and verifies the zipapp itself as well. Settings → Get umu runs this helper asynchronously and returns the path for selection. Novelty finds it automatically when system umu and the legacy `.novelty/tools/umu-run` are absent. Existing different files and linked managed paths are refused; verified versions are reusable offline. Per-version locks prevent concurrent writes. A hard kill during final publication may leave a small hidden staging directory for later inspection; ordinary cancellation cleans temporary work. System `umu-run`, or a custom executable set in Settings, also works. Proton/runtime provisioning may still require networking. The first sandbox download failed DNS resolution; the user subsequently installed it, and actual Template!! launches now work. See ENGINE_RESEARCH.md for observed results and remaining checks.

Template!! is imported using `Tenpure.exe`, with the existing Proton 10.0 directory selected because the matching sniper runtime is already present on this Deck. That is a test selection, not compatibility evidence. Manage lets you change it. The accepted runner's version-file identity is stored; if Steam updates that identity, review/save it in Manage before launching. This is update detection, not immutable preservation of the entire runner artifact.

First Play prepares the selected game's dedicated prefix; Prepare is also available in Manage. The UI launches a headless instance of the same executable. Closing the UI leaves that supervisor alive; it now waits for umu and its descendant processes before releasing the installation lock. Prefix locks prevent duplicate preparation/launch and editing a running installation. Normal launch logs are capped at 1 MiB, with one previous log retained. Pending/failed/interrupted state survives reopening.

Generic setup now lives in `src/setup.cpp`. `setup.json` records an attempt ID, input configuration, prefix-init/validation step and running/failed/complete state. Success requires umu to exit normally with code 0, a prefix registry file and matching completion records. `recipe.lock.json` schema 2 records the successful attempt, prefix identity (where the filesystem supports it), runner/version, locale/environment and configuration snapshot. Ordinary launches keep this snapshot unchanged. Accepted runner, locale or environment changes require another preparation; title/argument changes and game-folder relinking do not.

An existing prefix with only the older snapshot/registry shows Needs setup and is initialised **in place** on next Play or Prepare. Novelty never deletes or rebuilds it during this operation. Failed, interrupted, damaged or mismatched completion records cannot produce Ready. Explicit Prepare reruns setup even when previously complete, using saved settings. This is a generic completion protocol, not yet an engine recipe executor or backup/restore system.

`src/runtime.cpp` handles new umu sessions through a separate monitor process. The headless caller writes `runtime.json` before forking the monitor, which starts a separate session. The monitor uses Linux's [child-subreaper facility](https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html) to adopt orphaned descendants, including detached sessions, and [waitpid](https://man7.org/linux/man-pages/man2/waitpid.2.html) to wait for them before clearing the marker. If the headless caller or its process group is killed, the monitor continues and permits a same-boot retry after every descendant stops. The direct backend's exit code remains the launch result. No persistent daemon or process-killing action was added.

While the monitor is alive, the marker blocks Play, Prepare, game settings, relinking and full removal; it clears only after monitored descendants exit. Reopen or Rescan to refresh an older UI view. If the monitor itself is killed, or a legacy marker has no surviving monitor, the barrier remains until a different kernel boot ID proves old processes cannot exist. Corrupt markers remain blocked for manual inspection. A dead PID alone is never treated as proof of a stopped game. This protection applies to umu sessions; Steam retains its separate untracked-request guard. Real umu/Wine acceptance remains pending.

For the packaged GUI, QSettings, Qt cache and GUI temporary files are directed under `bin/data` beside the executable. Managed umu commands override cache and temporary paths under `<Games>/.novelty/cache` and store Steam Linux Runtime under `<Games>/.novelty/runtime`; prefixes, logs, runner downloads, installers and save snapshots are also under `<Games>/.novelty`. GE and umu installer helpers stage inside Games. The bundled Python wrapper disables bytecode and user site packages. Existing libraries remain on their prior home-directory runtime until Settings → Migrate legacy umu runtime explicitly copies and verifies complete runtimes, then switches the library; it retains the source for other libraries. Novelty reads host graphics drivers, glibc, certificates and optional Steam account files; Add to Steam is the explicit external write. A custom system umu/runner may retain its own storage behavior. Clean Deck/Bazzite write-path and uninstall checks remain release acceptance tasks.

Test text rendering, sound, any opening movie, input, saving/loading, ordinary exit and offline relaunch. Then test suspend/resume and Gaming Mode. Record each result in ENGINE_RESEARCH.md; do not infer them from a successful process start.

## Automated checks

```sh
./scripts/build.sh
python3 -m unittest discover -s tests -p 'test_*.py'
python3 scripts/benchmark.py
```

Core checks use a fake umu executable to verify discovery, duplicate handling, ambiguous executables, prefix isolation, setup/launch/retry, arguments passed without shell interpretation, persisted state, runner-update detection, lock contention and missing-game retention. A UI integration test switches grid/list views, checks shared selection, searches the library, verifies empty metadata placeholders, double-clicks a card to Play, opens the card's Manage menu and checks controls in a smaller window. A focused UI fixture checks that first Play opens the Add executable picker, Cancel leaves the game untouched, and saving a choice continues launch. The larger suite also checks Overview, Games folder selection in Settings, Yes/No removal on temporary fixtures, and VNDB flows against a local HTTP fixture. It destroys the window during a fake game session and checks that the detached supervisor survives and records the exit. Installer checks verify archive hashes and preserve an existing executable on a failed update.

Setup recovery checks additionally exercise legacy-prefix verification, nonzero/crashed preparation after registry creation, zero exit without a registry, persisted interrupted steps across library reopening, mismatched/corrupt completion receipts, runner changes during preparation, explicit repair, stable snapshots and byte-for-byte preservation of fixture saves. Steam removal checks enforce the guard during preview and apply, including backend switches/reopening, and verify the disabled Manage action.

CTest also runs `tests/runtime_recovery.py` (Python 3 is a test/build dependency, not an application runtime dependency). It starts fake backend/descendant processes, kills the headless Novelty caller during setup, gameplay, prefix tools, one-time executables and installers, verifies blocked retries while a child survives, then checks same-boot marker clearance and save preservation. Core tests cover legacy/unowned markers blocking edits/relink/removal, including previously captured plans. UI tests verify status and disabled actions. Fixture descendants are collected by the test driver; user games are never signalled or removed.

Discovery checks additionally verify delayed copy import, nested/growing asset changes, vanished copies, continued access to existing games, idempotent rescans and reconnecting a missing library. The missing-volume cases simulate a renamed root and an empty remembered mount point; they do not physically unmount an SD card.

Relink tests cover native directory identity across renames, learning identities from older records, copies with distinct identities, reused folder names, pinned runner preservation, prefix/save contents, locks on both entries, configured/prepared duplicate rejection, transactional rollback and stale plans. The UI integration test opens Manage and Relink, applies a move outside Games, then launches through the fake backend and checks its executable/cwd and unchanged prefix. These are temporary filesystem fixtures, not an additional real VN compatibility run.

These tests do not establish Windows-game compatibility or GPU/display/controller behavior.

Observed preliminary release-build measurements: 500-entry library, first queued UI event at 89 ms, RSS 45,172 KiB (~44 MiB), zero CPU ticks over two idle seconds. This used Qt's offscreen platform after filesystem caches were warm. It is not a cold boot, packaged release or Gaming Mode benchmark.

Generate a UI preview without a display:

```sh
QT_QPA_PLATFORM=offscreen ./build/novelty --root /home/deck/TestGames --screenshot artifacts/library.png
```

## Current boundaries

The prototype discovers one installation per direct child folder and searches at most two levels below it, skipping common helper/runtime/save directories and symlinks. Multiple plausible executables require manual selection. New games get the configured default backend/runner; existing games keep their settings. Library and runtime state live in `<Games>/.novelty/`; a small QSettings file remembers the chosen root. Game files remain in place. Missing folders retain their records. umu folder relinking is implemented; whole-library relocation, Steam shortcut relinking and proof of external copy completion are not yet supported/validated.

New automatic imports now wait for two or more observations and at least four seconds without changes to names, sizes or modification times. Metadata is checked throughout the new game's tree in a worker, with no asset-content hashing; cached installations skip that work. Empty folders and eligible nested directories are watched so later executable arrival triggers discovery. Timed rescans run only while new candidates are settling and pause during the monitored game session. This is a conservative quiet-period heuristic: a copy paused for longer than that interval can still appear settled, and subsequent modifications to already-imported games are not gated. Manual Add remains an explicit import.

When the open library disappears, the current window retains its rows, marks them unavailable and disables Play. Reconnect and press Rescan. Starting with a missing remembered database shows its folder location and waits for reconnection instead of creating a replacement database at an empty mount point. Because the database remains inside the Games folder, cached rows cannot be loaded on a fresh start while that volume is absent. Watch-limit failures produce a Rescan hint.

## Moving or renaming an umu game

Reopen the updated launcher and let its initial scan finish. Existing records learn directory identity while their folders are still present. On supported Linux filesystems, discovery matches device/inode/creation-time identity to follow the same directory under a new direct-child name. Copies, cross-filesystem transfers, ambiguous matches and unavailable creation-time data require manual relinking. This uses metadata exposed by [Linux statx](https://man7.org/linux/man-pages/man2/statx.2.html), not content hashing or marker files inside games.

Select the original library row, open Manage → Relink folder, choose the new folder and review the executable/working directory. Paths inside the old folder are remapped; paths outside it, arguments and environment values stay as configured. If the executable layout changed, select the new executable. Relink commits immediately when its own Relink button is pressed; other unsaved Manage edits remain in that dialog. Cancel does not undo an already-applied relink.

The original installation ID, prefix, name, settings and accepted runner revision are retained. An automatically discovered destination entry can be consolidated only if it has no explicit saved configuration and no preparation/launch artifacts. The preview names that entry before applying. Configured, prepared or running entries are kept and reported as conflicts. Reusing an old folder name for a different directory produces Folder changed and cannot implicitly launch with the old prefix.

Linux can accept folder/file names that conventional Windows paths reject. Novelty now reports Path needs rename for Windows-reserved characters/device names or trailing periods/spaces in the umu executable/cwd, with an actionable explanation before launch. In the supplied test, renaming Template!! to Template!!? prevented Proton 10.0 from creating the game process; restoring its original folder name resumed startup with the same prefix. See [Microsoft's naming rules](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file). Changing the display name in Manage does not change the filesystem name.

The relink operation locks both involved entries, rechecks the previewed records and destination, and commits its database changes in a [SQLite transaction](https://www.sqlite.org/lang_transaction.html). It never moves or deletes game/prefix contents. Auto-detection does not accept Proton updates. Steam relinking is disabled because updating Novelty's paths would not update the external Steam shortcut.

After these changes, the same 500-entry offscreen measurement was 79 ms to the first queued UI event, 44,300 KiB RSS and zero CPU ticks over two idle seconds. Treat both measurements as preliminary warm-cache observations, not a performance guarantee.

Engine detection now chooses a pinned generic or Ren'Py bundled baseline on import. Both use the conservative generic preparation strategy. No codec/font/Winetricks installation or engine-specific dependency recipe is enabled yet.

The Steam adapter launches an existing numeric `rungameid` via the configured Steam executable. Use the launch ID from a Steam-created desktop shortcut. Steam owns its runner, prefix and lifecycle; receiving the request is not reported as a confirmed running game. This adapter does not create shortcuts, apply Novelty's umu prefix setup to them, or migrate saves between backends. The user's intended Steam semantics still need confirmation before increment 1D.


## Removing a game

Manage opens with a saved-settings Overview. Its Remove game action requires the saved name and lists every deletion target. It removes the game directory, Novelty prefix and per-game state/library record; saves inside those directories are included. Saves/files elsewhere are kept. Full removal is disabled for Steam games and games with an untracked Steam request, because Novelty cannot establish that their processes have stopped. The request marker persists across backend changes; resolving it awaits Steam lifecycle tracking. Older Requested in Steam journals gain the marker before settings or launch status changes. See [layout and removal behavior](UI_LAYOUT.md) for safeguards, background cleanup and recovery limits. Automated deletion checks never target `/home/deck/TestGames`.

## Interrupted removal recovery (library schema 2)

`src/removal.cpp` owns removal planning, staging and recovery. A SQLite `removals` row records the original game/configuration, exact staged paths, directory identities and parent identities **before any rename**. The game-record deletion and the transition to committed cleanup share one database transaction. SQLite uses synchronous FULL, and staging/restoration flushes directory changes with `fsync`. References: [SQLite atomic commit](https://www.sqlite.org/atomiccommit.html), [Linux fsync](https://man7.org/linux/man-pages/man2/fsync.2.html).

On startup and Rescan, the discovery worker resolves pending jobs before discovery: uncommitted jobs restore staged folders; committed jobs finish the already-confirmed deletion. Repeated recovery is safe after partial restoration/cleanup. A replacement folder at the original name is never overwritten during restoration or deleted during committed cleanup. Changed identities, links, missing parents, corrupt records and conflicting library entries keep their files and journal for inspection/retry. The UI reports recovery outcomes. Game operations remain blocked for a pending removal, and affected original/staged paths cannot be reimported or assigned to another game while unresolved. A short-lived library mutation lock coordinates edits, discovery import and removal/recovery; read-only browsing and unrelated game launches remain available.

The schema migrates from 1 to 2 without changing game configuration records. Older binaries reject schema 2 when reopened: close old Novelty windows before using this build. Recovery requires directory identities and directory flush support; unsupported filesystems refuse removal rather than guessing. This is crash recovery with filesystem durability ordering, not an independently tested guarantee against every power/storage failure. Physical SD-card loss/reconnection and power-loss testing remain pending. Hidden folders left by older, unjournaled removals still require manual inspection; no broad search/deletion of such directories occurs.

`tests/removal_driver.cpp` is a test-only executable. Its progress callback exits abruptly at intent, each staged folder, database commit, partial folder cleanup and state cleanup. `tests/removal_recovery.py` checks restoration versus cleanup, repeated recovery, retained saves, conflicting/replaced directories, corrupt records and blocked launches. The UI integration test verifies automatic restoration on opening a library. No crash switches are exposed in the production executable.

## Monitoring performance

Launch polling updates only the monitored game row and emits no model signal when its state/actions are unchanged. Full refresh remains appropriate for discovery/configuration/removal changes. `python3 scripts/benchmark.py --running` measures the frontend while a disposable fake game is supervised; the benchmark owns and terminates only its own fixture process group. In the 500-entry offscreen check, this change reduced two-second frontend CPU use from 33 ticks to zero. Real game/controller/Gaming Mode behavior is a separate acceptance gate.

## Managed GE-Proton versions

Settings and Manage → Runtime offer **Get GE-Proton**. Enter an exact official version such as `GE-Proton11-7`, install/use it, then save the enclosing dialog to select it. Installation is asynchronous and cancellable; completed versions remain side by side under `<Games>/.novelty/runners/`. Steam compatibility-tool registration is separate and not implemented. If a game has no runner, first Play uses the valid library default, then the exact GE build pinned in `ge-proton.json` or the generic VN/game profile. It reuses an installed copy or prepares a missing pinned build within the combined first-Play dialog. That same dialog continues to missing umu and Steam Linux Runtime steps. An explicit `auto_download: false` requires a Start click to permit the GE download. Profiles without a version pin retain the installed-runner chooser / manual GE dialog fallback. Explicit custom runner/umu paths are never silently replaced. GE-Proton11-7 is the current official release pin; successful archive validation does not establish game compatibility.

The helper requires Python supporting `tarfile.data_filter` (Python 3.12+ recommended; this Deck has 3.13). `configs/ge-proton.example.json` demonstrates the schema, architecture, exact tag, optional checksum pin and `auto_download` permission. With `auto_download: false`, missing versions fail without network access; existing verified managed versions remain usable offline. For an explicit CLI install, copy the example, review its version/checksum, enable `auto_download`, then run:

```sh
python3 scripts/install-ge.py /path/to/Games --config /path/to/ge-proton.json
python3 scripts/install-ge.py --inspect-release GE-Proton11-7
```

The native dialog stores defaults in `.novelty/ge-proton.json` and passes a unique request file to each helper. Release metadata/sidecar checksums come from the official repository; a configured SHA-512 pin adds an independent expected value. Existing managed builds are identified by a validated receipt, not rehashed file by file on every use. Per-version locks and owned staging permit retry by restarting download/extraction; byte-range resume is not implemented. Failed/cancelled work cannot replace a prior version. The extracted tree is not individually fsynced, so power-loss installation durability remains unverified.

References: [GE-Proton releases](https://github.com/GloriousEggroll/proton-ge-custom/releases), [GitHub release-by-tag API](https://docs.github.com/en/rest/releases/releases#get-a-release-by-tag-name), [Python extraction filters](https://docs.python.org/3/library/tarfile.html#extraction-filters). Archive validation for GE-Proton11-7 passed; this is not a tested game compatibility recommendation.

## umu runtime storage

New Games libraries mark their umu storage as managed. Every umu game, installer and prefix-tool command then sets `UMU_FOLDERS_PATH=<Games>/.novelty/runtime` and `XDG_CACHE_HOME=<Games>/.novelty/cache`; pinned umu 1.4.4 uses these for the Steam Linux Runtime and download cache. Its [1.4.4 path definitions](https://github.com/Open-Wine-Components/umu-launcher/blob/1.4.4/umu/umu_consts.py) show both variables. Existing libraries have no marker and keep their prior environment so an offline launch does not suddenly require a new runtime download. They need an explicit migration and device acceptance before this storage gate can be called complete. Temporary runtime downloads may still use system tmpfs while active; inspect cleanup on interruption. Novelty's own QSettings and packaging/helper dependencies remain outside this completed part.

For a selected Proton runner with a known `require_tool_appid` in `toolmanifest.vdf`, first Play checks the matching runtime's `.installed.ok` and `_v2-entry-point` under the managed path. If it is missing, Novelty runs the selected umu launcher with an empty argument and a disposable prefix under `.novelty/`, displays umu output and elapsed time, and offers Cancel/Retry. umu performs its own runtime download and extraction. The disposable prefix is removed after setup; the completed runtime is reused on later Plays. A failed or cancelled attempt does not mark the runtime ready. An unknown/missing manifest is still handled by umu during normal launch. Legacy libraries continue using their existing umu storage without this preflight until explicit migration. The current display is indeterminate because pinned umu does not expose a reliable total download size through this interface; clean-device behavior and temporary download cleanup still require acceptance.

Automatic first Play now uses `provisionFirstPlay()` for missing pinned GE, default umu and recognized Steam Linux Runtime steps in one dialog. Each successful component remains installed if a later step is cancelled; Retry can continue and the next Play reuses it. A newly chosen per-game runner is saved only after all preparation succeeds. GE's explicit automatic-download ban presents Start instead of silently starting; accepting it writes the chosen pin as in the manual installer. Settings and Manage retain their separate direct installers. The no-version custom profile fallback can still open the manual GE chooser before the combined path. Fixture tests cover one-dialog sequencing, explicit Start, retry, cancellation and no premature runner save. Real game and clean-device acceptance remain pending.

For managed libraries, launch commands also set `TMPDIR=<Games>/.novelty/cache/tmp` after creating and checking that directory. The pinned umu's `get_tempdir()` then uses that location when it chooses temporary storage. Game overrides cannot redirect the managed cache or temporary path. Legacy libraries retain their prior environment.

## Portable test package

Build with `./scripts/build.sh`, then run `python3 scripts/package-portable.py --output /path/to/Novelty-test` and launch `/path/to/Novelty-test/Novelty`. The package script refuses an existing output and stages before publishing. It copies the built executable, installer helpers, selected Qt platform/SQLite/image/TLS plugins, their linked libraries, and a Python 3.12+ interpreter with the standard library. Novelty uses that Python for its installer helpers when present. The wrapper supplies the package's libraries/plugins to Novelty; umu and Proton commands restore the host's original library/plugin search paths, while bundled Python loads its own library again. The GUI stores `gamesRoot` and `libraryView` in `bin/data/Novelty/Novelty.ini`. On first start, it copies missing values from a legacy Novelty settings file and removes that old file only when it contains no other keys. Isolated-folder offscreen screenshot, relocated reopen, bundled Python imports and a packaged focused UI/helper fixture passed.

This is a development package, not the P4 release gate. It still depends on host glibc, graphics drivers and certificates. Clean Deck and Bazzite tests, actual online downloads, controller/focus behavior, bundled-library license review and a complete write-path audit remain open. Delete the package folder and its Games folder to remove package-owned state; this has not yet been validated against a real clean-device install.

## Versioned setup profiles

`src/profiles.cpp` is the small built-in catalog/resolver. The per-game `setup_profile` is an immutable version pin. Import selects `renpy-bundled-v1` for detected Ren'Py, `generic-proton-v1` for advisory VN-family detections, and `generic-game-v1` for unidentified games. Generic VN/game profiles prefer GE-Proton11-7 when an unconfigured game first reaches Play; a valid library default and explicit per-game runner take precedence. Missing managed GE can be provisioned through the existing dialog, subject to the library's `auto_download` policy. All profiles currently share conservative prefix preparation, with no speculative dependency changes. Legacy records missing the field retain generic setup; unsupported fields are retained for visible repair and cannot launch/prepare through umu. Steam owns its own setup.

Manage → Runtime can select a profile or restore the detected default without clearing individual settings. Save applies the choice, Cancel discards it. Both current profiles share the `generic-proton-v1` preparation signature, so changing between them does not rerun equivalent work. Existing successful generic receipts remain valid. Preparation receipts record profile/configuration for audit, while completion comparison uses effective preparation inputs. If a later profile introduces different actions/defaults, give it a new preparation signature and implement/test that executor; never change the meaning of an existing version. No dynamic plugins, arbitrary scripts or speculative dependencies are introduced.

The Runtime tab previews the currently selected profile and whether it matches the detected default, plus the Proton source that first Play would use: a per-game path, valid library default, library GE pin or profile preference. The locale is shown too. This preview does not claim an unverified game-specific fix; explicit runner/profile/locale choices remain editable before Save.

## Wine configuration and registry tools

Manage → Runtime opens **Wine configuration** or **Registry editor** with the game's saved umu/Proton settings. Save edits before opening a tool. A valid game launch configuration is required; missing/changed runners are handled by the ordinary preflight, and incomplete prefix preparation runs first. The tool shares the game's supervision/lock/recovery behavior. Closing Novelty leaves the tool running; other operations remain blocked until all descendants stop. Steam prefix tools are unavailable.

```sh
./build/novelty --root /path/to/Games --run INSTALLATION_ID --prefix-tool winecfg
./build/novelty --root /path/to/Games --run INSTALLATION_ID --prefix-tool regedit
```

The tool receives only its absolute utility path within the dedicated prefix, without saved game arguments. Logs/status identify the operation and report errors. Wine configuration can edit settings such as the advertised Windows version; the launcher does not make such edits automatically. Tool changes persist in the prefix; profile reset does not undo them. Prefix backups/rebuild remain separate work. One-time patch/install executables are described below. This has fixture/process/UI coverage, with real GE utility behavior still pending.

Primary references: [umu's winecfg example](https://github.com/Open-Wine-Components/umu-launcher/blob/main/packaging/rpm/README.md), [umu 1.4.4 executable resolution](https://github.com/Open-Wine-Components/umu-launcher/blob/1.4.4/umu/umu_run.py), [Wine prefix file layout](https://github.com/wine-mirror/wine/blob/master/loader/wine.inf.in).

## One-time patch/installer executables

Manage → Runtime → **Run another executable…** selects a Windows `.exe`, an optional working directory (blank means the executable's folder), and one literal argument per line. Run uses the game's saved runner, prefix, locale and environment; Save any Manage edits first. Cancel has no launch side effects. This requires an existing valid umu game configuration; Steam and standalone MSI installation are not supported by this action.

The selected executable is passed to the detached supervisor without changing the stored game executable/cwd/arguments. Preparation runs first when required. The same per-game lock, descendant tracking and persistent crash barrier protect the prefix. Closing the UI leaves the operation running. Changes made by the selected program persist; Novelty does not attempt to undo an install/patch.

```sh
./build/novelty --root /path/to/Games --run INSTALLATION_ID \
  --extra-exe=/path/to/patch.exe \
  --extra-cwd=/path/to/game \
  --extra-arg='argument with spaces' --extra-arg='--option=value'
```

`--extra-cwd` is optional and `--extra-arg` may repeat. Equals syntax preserves leading dashes; CLI arguments may also be empty. The GUI skips blank argument lines. Neither path invokes a shell. Paths must be absolute and existing; Windows-reserved path components, null argument characters, more than 64 KiB of argument bytes and paths reserved by interrupted removal are refused.

The supervisor records `.novelty/installations/<ID>/extra-launch.json` with the request, umu/runner/prefix, state, timestamps and exit code or failure message. One previous record is retained. Manage → Logs includes the current record; normal launches preserve it. A crash retains preparing/running evidence and triggers the existing recovery barrier. A later safe normal launch marks an unfinished old operation interrupted; it never retries the patch. Retrying an installer remains an explicit user action.

Temporary executable/cwd references are published under the library mutation lock before preparation. Removal checks them again under the same lock, including when applying an older removal plan, so another game's referenced folder/prefix/state cannot be deleted while the operation or its unresolved crashed session owns them. Recovery also checks these references. This coordinates Novelty's operations; it does not lock files against external applications.

Launch command construction and supervision now live in `src/launch.cpp`; storage/discovery remain in core.cpp, setup receipts in setup.cpp and process lifetime tracking in runtime.cpp. Fake-backend, UI and actual SIGKILL tests cover this feature. A real patch/install execution through GE is still an acceptance test; no supplied game has been patched by the automated checks.


## New-game installer workflow

Select folder uses `Library::addFolder` in `src/folder_import.cpp`. It accepts a dedicated game folder, moves it to a direct child of Games and inspects/imports there under the library mutation lock. Direct children are scanned in place, and a previously registered umu game outside Games retains its ID, prefix and settings when moved. Name conflicts, protected/system/personal folders, mounted-drive roots, overlapping registered game/runtime paths and pending removal paths fail before the move. Same-filesystem moves use a directory rename; cross-filesystem moves write an import record under `.novelty/imports`, copy to a hidden Games staging folder, compare file contents and symlink targets, publish the folder, and only then retire the original. A failed transfer keeps the source; a failed source cleanup reports the retained copy. Startup/Rescan removes a recorded incomplete stage only when its directory identity and the original's identity still match and no destination exists. Changed/conflicting copies and older unrecorded hidden stages remain for inspection. A crash before the stage identity is recorded also requires inspection. The Add UI runs the transfer in a worker and shows a modal transfer notice; any pending Rescan result is held until it finishes.

Installer text language in Add game offers system default or `ja_JP.UTF-8`. The choice is saved before prefix preparation and passed to both `HOST_LC_ALL` and `LANG` for the installer, later game launches and one-time executables. Manage → Runtime can change the locale for an already added game; Save it before retrying the installer through Run another executable. This is a targeted locale control for legacy non-Unicode text, not a guarantee that every installer font/encoding issue is fixed.

`src/installation.cpp` owns allocation and detection. `Game` adds backward-compatible `installer` and `install_pending` fields. Allocation validates the external `.exe`, runner/umu and safe folder name before creating a fresh direct child of Games, under the library mutation lock. Existing directories, symlinks, registered missing folders and removal reservations are refused. The game executable starts empty; ordinary Play refuses pending entries. Installer source paths are protected against other games' full removal while pending.

`--root ROOT --install ID` supervises the explicitly selected pending installer. It prepares the dedicated prefix, maps `dosdevices/n:` to the allocated game folder, runs the external installer from its source directory and waits for descendants. A conflicting N: mapping fails without replacement. The path mapping follows [Wine's documented DOS-device symlinks](https://github.com/wine-mirror/wine/blob/wine-10.0/loader/wine.man.in). The user must choose `N:\` in the installer; this is not automatic installer-specific destination selection, a sandbox, or MSI support. Setup may still need a first-run runtime download.

The operation reuses `extra-launch.json` with `kind: installer`, the same locks, operation history/logs, path reservations and runtime crash barrier. Success is recorded before detection. Startup/Rescan may finish detection for a completed installer, but never executes an installer automatically. A killed headless caller leaves the monitor tracking installer descendants; a killed monitor still requires a restart. A failed/nonzero installer stays pending; retry and Detect installed files are explicit Manage actions. Files and prefix changes survive cancellation/failure. Automatic detection occurs after a zero exit; manual detection can recover files from a cancelled/failed install.

Detection runs under the launch lock plus library mutation lock. It checks the existing discovery depth (two subfolder levels), excludes setup/uninstall helpers, selects a sole executable and detects a nested engine beside it. Multiple executables require choosing one in Manage; no executable leaves an actionable pending status. Detection retains installation ID, prefix and runtime settings, and applies the detected setup profile. A subsequent normal Play still checks preparation receipts. Prefix tools can prepare/configure a pending installation before retrying it.

Fixture coverage includes invalid destinations/dependencies, non-overwrite, no accidental installer Play, lock exclusion, external adjacent media, nested Ren'Py detection/profile, ambiguous/empty installs, failed retry, completed-operation recovery without replay, conflicting drive mappings, pending source removal protection, actual Add UI controls, UI-close survival and installer-specific SIGKILL barriers. Real GE installer-window/drive visibility and game acceptance remain manual platform gates. No installer has been run on the user's supplied game by these tests.


## One-launch diagnostics

Manage → Runtime → **Launch once with Wine debug** (or `--root ROOT --run ID --debug-wine`) adds `WINEDEBUG=+seh,+loaddll` to that launch command. Prefix preparation, if needed, uses the saved environment. The game executable/arguments and persisted environment are unchanged. This requires an ordinary umu game launch; it is not accepted with a Steam shortcut, Prepare, installer, prefix tool or extra executable. The launch log marks debug capture and retains the 1 MiB cap. Normal Play subsequently rotates that log to `.previous` and uses the saved environment again.

Manage → Logs → **Export diagnostic report…** writes an atomic, indented JSON file selected by the user. `src/diagnostics.cpp` reads the current game status, setup summary, receipt summary, operation summary, runtime marker and the current/previous launch logs (each capped at 1 MiB). It includes paths, engine/profile/runner and the names of custom environment variables, but omits their values and game arguments. The report may still contain personal paths or arbitrary game log output, so inspect it before sharing. Broken state JSON appears as a `read_error` in that section. No archive, automatic upload or provider is part of this feature.
