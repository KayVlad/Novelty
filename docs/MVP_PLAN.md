# Staged MVP plan

Initial proposal dated 2026-10-08, with scope and delivery order updated on 2026-10-10. [PRODUCT_VISION.md](PRODUCT_VISION.md) owns product direction and [STATUS_CHECKLIST.md](STATUS_CHECKLIST.md) owns the current Lego delivery order. The sections below retain supporting design and acceptance detail; their older 1A–1D labels do not replace that order. See [PROGRESS.md](PROGRESS.md) for completed increments and observed test results.

## Scope fixed by the user

Build one game lifecycle manager in small, independently testable increments. Visual novels are the first priority; other Proton games are in scope. Steam Deck is the main target. Novelty must start quickly, remain responsive and stay unobtrusive while a game runs. Use progressive disclosure in one application and keep configuration editable for power users. Its release should keep owned files in the application folder and chosen Games folder, require no separate Bottles/protontricks/ProtonUp-Qt installation, and clean up owned state on removal.

Stage 1: complete the existing local installation/setup/play flow and deliver a self-contained package. Add a game folder or install the game, then choose its executable when needed; a sole suitable candidate is already selected automatically. VNDB metadata is fetched automatically for new games with manual correction available; this is sufficient for MVP, while broader metadata coverage is deferred. Finish the fuller game/engine preset hierarchy, preserving runtime selection and advanced overrides. Use VNDB's colour scheme.

Stage 2: save discovery, backup/restore, paired-device save sync, then one-button peer-to-peer LAN game installation when the other device lacks the game. Metadata is part of the default product experience and manual VNDB matching is already implemented. The final convenience block is Add to Steam. Broader integrations are outside the current delivery list.

## Proposed stack

- C++20 and Qt 6 Widgets for the application. Use Qt Core, Gui, Widgets, Network and Sql only as needed. Qt Test plus CTest for automated verification.
- SQLite for installation identity, library state and setup progress; versioned JSON files for engine presets and resolved per-game configuration. No service or database server.
- Qt model/view for the library, filesystem notifications plus background reconciliation for discovery, asynchronous subprocesses for setup and launch.
- umu-launcher as the execution backend. Add to Steam creates a shortcut to Novelty's managed launch command, retaining the same setup and lifecycle behavior.
- CMake and Ninja for builds. Build in an isolated user-owned environment on the Deck; distribute an unpacked application bundle first, then evaluate AppImage packaging. The packaged release must include its required Qt plugins/dependencies. Do not assume the host's Qt installation is a stable deployment target.

C++ adds development effort relative to Python/PySide6, but better fits the stated priority on startup and frontend footprint. It does not itself guarantee performance. Qt Widgets fits the list, buttons and detailed settings workflow. Use explicit focus navigation and sufficiently large controls at 1280x800. Steam Input mappings can drive navigation in Gaming Mode; desktop touch/mouse/keyboard must work independently. Direct gamepad input outside Steam is an input-adapter decision to test, not a reason to promise Qt Widgets provides it automatically.

Observed locally: SteamOS 3.8.28 x86_64, Steam, Qt's qmake6, Distrobox/Podman and several Proton/runtime installations are present. umu-run, CMake and a C++ compiler were not found on the current PATH. Nothing has been installed. Build-environment setup is part of the first increment.

## Module boundaries

Library storage owns stable installation IDs and configured paths. Discovery proposes installations/executables. Game/engine identification returns evidence and confidence. The preset resolver selects game-specific, engine-specific or generic VN / generic game defaults and preserves explicit user overrides. Setup executes a persistent plan. Novelty launches through umu and supervises the game; the planned Steam shortcut invokes that same path. The UI presents those states through one integrated workflow. Existing legacy Steam dispatch behavior remains guarded until explicitly revised.

These are modules in one program, not a plugin framework or independent services. Add boundaries for tests and later saves without implementing future features now.

Default managed storage: `<Games>/.novelty/` for the library database, presets, configuration, prefixes, shared runners, cache and logs. Exclude it from scanning. Current Select folder moves external game folders into Games; direct children are scanned in place. New libraries set pinned umu's runtime and cache paths under `.novelty`; preexisting libraries retain their previous umu location until an explicit migration is ready. Store the chosen Games location and app preferences in the application folder for the portable release. Audit and redirect remaining external settings/cache/runtime writes, including QSettings. Temporary work must be removed after use or during recovery. A read-only application directory and read-only game location need explicit behavior, but should not silently create scattered OS state. The optional Add to Steam action is an explicit write to Steam's shortcut configuration; an external prefix editor may write inside a managed prefix at the user's request.

## Stage 1 increments and acceptance gates

### 1A — One reliable launch and a measured UI shell

Create the small library/Manage window and one manually added game using an existing compatible runner through umu. Manual selection at this step is a diagnostic baseline for later automation. Store configuration, show actionable failure state and capture bounded logs. Retain game ownership outside the UI so closing the window does not accidentally kill the game. A small headless invocation of the same executable may supervise a launch and exit when the game ends; no permanent daemon.

Gate: actual game launches and exits, relaunches offline after provisioning, handles spaces/Japanese paths, and survives the UI being closed. Test process handoff and failed executable separately. Measure packaged startup, idle memory/CPU and hidden-window behavior here.

### 1B — Main folder and incremental discovery

Choose the Games folder in first-run setup. Load the cached library immediately, then reconcile new/changed directories in a worker. Start with one game per direct child directory and a bounded search inside it for executables; support manual correction for unusual nesting. Use directory events while open, debounced for copying/extracting games, plus a Rescan action and startup reconciliation. Watcher events alone do not prove a directory is complete.

Rank likely launch executables and exclude obvious uninstallers/installers from automatic selection. Ask for selection when ambiguous. Do not execute discovered files to identify them. Avoid full-library hashing. Mark missing games/drives unavailable while retaining entries and setup data. Prevent duplicate imports and symlink loops.

Gate: adding, copying, removing and renaming folders behaves predictably; a disconnected SD card preserves the library; scanning never blocks launching an already configured game.

### 1C — Generic setup, then independently researched engines

First make the generic setup reliable: a dedicated prefix, a fixed compatible runner, correct working directory and per-game locale/font choices. Generic means a conservative baseline that can be adjusted; it cannot guarantee all VN engines work.

Then add engine profiles individually. Initial research candidates: Kirikiri/Kirikiri Z, BGI/Ethornell and Siglus. Choose the first based on available test games. Developer names such as Minori or Nitroplus are not automatically engine/version identities. Each enabled profile needs its own evidence.

For each profile:

1. Establish engine signals from engine documentation/source, executable metadata and actual installations. For example, Kirikiri's own source implements XP3 archives; the extension alone is a candidate signal, not proof of a particular build.
2. Research requirements against current Proton/GE-Proton, Wine/Winetricks and the specific media/rendering behavior. LVNM presets can suggest questions, but do not become Novelty presets by copying them.
3. Start with a clean generic prefix; add only demonstrated dependencies or overrides. Some codec scripts disable or replace existing components, so blanket codec installation is not a neutral fallback.
4. Record test game/build, engine variant, runner/runtime, changes made and text/movie/audio/save-load results. Mark untested engine variations accordingly.

Setup runs automatically on first Play, with visible progress and retry, plus an optional Prepare action. Merely discovering a large library should not allocate prefixes or download dependencies for every game. Pin successful configuration; preset updates must preserve local overrides and working setups. Use checksummed downloads, resumable/restartable setup steps and a per-installation operation lock. Failure remains visibly incomplete rather than falsely Ready.

Gate per profile: real-game text, movies where present, audio, input, save/load and offline relaunch; interrupted setup recovers; two installations never share mutable prefixes accidentally. Normal save/load is tested now even though save-management UI arrives in Stage 2.

### 1D — Deck/desktop acceptance and complete Stage 1

Package the local lifecycle with its required libraries/plugins and a complete first-Play runtime path. Verify a clean Deck/user and Bazzite desktop, cold/warm startup, offline relaunch, UI-close survival, force-exit, suspend/resume, controller/focus/touch and supported library locations. These are the P4 release gates. Gaming Mode acceptance remains part of this work.

P8 is limited to Add to Steam: register a non-Steam shortcut invoking Novelty's managed game launch, preserve unrelated entries and avoid duplicate shortcuts. Validate that Desktop/Gaming Mode shortcut launch uses the same Novelty runner, prefix and saves. No separate Steam-owned setup or execution adapter is planned. The current legacy Steam backend is existing behavior, not a roadmap requirement to expand it.

## UI proposal

One main library window with Search, Rescan and Settings. Following the user-supplied [layout revision](UI_LAYOUT.md), the default view is an adaptive cover-card grid with an optional row/list view. Cards show mostly artwork, title, engine and status light; double-click launches and right-click opens Manage/Delete. The fixed-width sidebar shows cached metadata and full cover artwork; game configuration is in Manage, starting with Overview. Games folder selection lives in Settings, and the footer path opens the file explorer. Full removal uses a compact Yes/No confirmation. VNDB matching runs automatically for new games, with manual correction available. Expected play time is metadata; measured playtime is outside the current delivery list. Status names should describe the next action: New, Preparing, Ready, Running, Needs attention or Unavailable. Show identification/setup details when needed and keep ordinary play free of unnecessary configuration steps.

Manage groups: launch executable/cwd/arguments; backend and runner; engine profile and locale; environment/DLL overrides and targeted dependency tools; logs, repair and reset-to-preset. Distinguish preset values from overrides. Destructive prefix recreation must preserve the old prefix or explicitly handle its saves. No unrelated panels or services.

Colour reference: VNDB's Angelic Serenade dark-blue skin. The fetched source mirror specifies black background, translucent `#071c30bc` panels, `#0d2741` secondary surfaces, `#dddddd` text, `#77bbdd` links and `#225588` borders. Adapt panels to opaque colours and increase secondary-text/focus contrast for the Deck. This is a palette reference; no VNDB artwork is needed. The live site's stylesheet could not be fetched in this session, so exact current-theme matching remains to verify.

## Performance gates

Proposed targets, not measured claims: cached library interactive within 1 second on a warm start and 2 seconds on a cold start; idle frontend under 100 MiB RSS and near-zero CPU. Measure release builds on this Deck with a 500-entry synthetic library, separately reporting initial scans and runner/game startup. Keep disk/network work off the UI thread, cap logs/caches and avoid background scanning while playing. If a target fails, profile it before adding the next layer.

## Stage 2 — Saves and cross-device continuation

After Stage 1 is stable: confirmed save mappings, local snapshots and restore first; one sync transport and explicit conflict handling second. Cross-device identity must be separate from display name and local installation ID. Registry progress requires explicit support. Never sync entire live prefixes. Engine detection alone is not enough to infer a game's save directory.

The target journey is to close the game on one device, open Novelty on another, install the game from a peer over the LAN with one button if needed, sync saves and continue. Build P5 local mappings/snapshots/restore first, P6 paired-device sync with the game already installed second, and P7 LAN game installation third. Plan direct LAN peer sync as the first transport; peer discovery and protocol details remain implementation decisions. Game transfer reuses pairing, identity and verification work. Show manual save-path correction or sync conflicts when necessary; advanced control must remain available.

## References for implementation research

- [Qt model/view](https://doc.qt.io/qt-6/model-view-programming.html), [filesystem watcher](https://doc.qt.io/qt-6/qfilesystemwatcher.html) and [process lifecycle](https://doc.qt.io/qt-6/qprocess.html).
- [umu launcher](https://github.com/Open-Wine-Components/umu-launcher), [umu FAQ including Gaming Mode](https://github.com/Open-Wine-Components/umu-launcher/wiki/Frequently-asked-questions-%28FAQ%29).
- [Valve Proton runtime options](https://github.com/ValveSoftware/Proton/blob/proton_11.0/README.md).
- [Steam non-Steam shortcut ID issue](https://github.com/ValveSoftware/steam-for-linux/issues/9463): historical evidence of API fragility, not proof of current client behavior.
- [Kirikiri XP3 implementation](https://github.com/krkrz/krkrz/blob/master/base/XP3Archive.cpp), [vn_winestuff codec implementation](https://github.com/b-fission/vn_winestuff/blob/main/codec.sh). These support research questions, not a verified preset.
- [VNDB skin source mirror](https://github.com/vgtuemgav/yorhel-vndb/blob/main/css/skins/angel.sass); upstream at code.blicky.net was inaccessible during this pass.
