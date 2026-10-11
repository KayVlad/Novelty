# Visual novel launcher — Codex handoff

## Project status

This project is at the research and scope-definition stage. No product implementation has been started. The user wants to continue this project in Codex. Treat this document as the project context, not as a claim that a task has already been submitted to Codex.

## User's original request

> Target: Steam deck/linux
> Project idea: A general proton based setup/launcher for visual novels, not reliant on steam services, utilizing existing projects such as bottles, umu, etc or similar depending on your findings, with a way of stitching it together, and keep it in the same folder to avoid clutter.
>
> The main goal is for the user to add a vn directory, ideally the vn is detected through file names/hashes with the possibility of searching (eg. Rance01 Mangagamer version), the wizard then checks db for availability and sets the best configurations for this particular vn, such as proton version, init commands, lang.
>
> Let's discuss the feasability, research tech and implementation

The user's subsequent scope clarification:

> check lvnm, might be interesting, i'm specifically curious about save file sync (especially since there's no standard place to put them), text extration, timer, metadata fetch. A similar frontend with the ability of autoset configs for a game could be a good scope, adding unrelated features to make it an everything app would be too much

## Scope and constraints

- Target Linux desktop and Steam Deck. The user's desktop runs Bazzite.
- No required Steam account/client/services for DRM-free game execution. Using Proton and the Steam Linux Runtime is compatible with the proposed architecture, but this interpretation should remain explicit.
- Add an existing VN directory, identify likely game and exact release, allow manual search/confirmation, select a tested configuration, prepare it and launch.
- Keep configuration, prefixes, runners and caches organized under a clear root where practical. Strict relocation/portability is still an unresolved engineering requirement.
- Closely related features: metadata/artwork, savedata discovery and sync, external text hooking, playtime tracking, controller-friendly launcher UI.
- Keep scope focused on visual novels. Avoid unrelated storefronts, emulators, social integrations or a broad productivity/language-learning suite.
- No requirement to copy/move game files on import; importing in place should remain possible.

## Preliminary architecture recommendations — not final user decisions

- Use umu-launcher directly as the Proton execution backend; pin runner versions per tested recipe.
- Use VNDB for VN/release metadata, not as a compatibility or executable-hash database.
- Maintain a separate versioned recipe/fingerprint catalog. Start as files in Git and a downloadable snapshot; no dedicated backend required initially.
- Distinguish VN identity, release/build/patch identity, local installation identity and recipe version.
- Detection: filename/executable metadata and engine markers for candidates; executable plus selected data-file hashes for stronger release matching. An engine executable alone can be shared by multiple games.
- Prefer one dedicated prefix per installation. Share runner binaries, not mutable game prefixes.
- Recipe fields should cover executable/cwd/arguments, runner version/checksum, locale/env, ordered setup actions, save mappings, optional hook settings and test evidence.
- Store a resolved local lockfile so recipe updates do not silently change working games.
- Python + PySide6/Qt Quick was suggested, but no stack is final. Assess extending LVNM before choosing a new application.

## LVNM research

Repository: https://github.com/uunniiblog/linux-vn-manager-lvnm

Inspected commit: `82ab14c542ffd0b35c1ef131e337f2b203712863`, dated 2026-10-05. Findings below come from source inspection and docs, not runtime tests. Recheck against current upstream before relying on them.

### Config import

`lvnm/ui/import_game_dialog.py` already downloads a specified runner when missing, creates a prefix, applies codecs/Winetricks/fonts and registers the game. It can reuse an existing prefix by name and skip setup; verify its configuration or isolate prefixes for release recipes.

`game_import_jsons/` contains six engine/developer presets: BGI, Kirikiri, Liarsoft, Minori, Nitroplus and Siglus. These are manual import presets, not automatic release identification. The importer is the most promising integration point for an identification/setup wizard.

### Saves

`lvnm/savedata_manager.py`:

- Heuristic discovery searches beside the executable for `savedata`, `UserData`, `save`, then common prefix locations: Saved Games, Documents and AppData Roaming/LocalLow/Local.
- Matches directory names against displayed game name, executable stem and original Japanese title; prefers exact and deeper matches.
- Does not trace writes; nonstandard paths, scattered saves and registry progress can be missed.
- Current code supports multiple directories with exclusions or groups of selected files.
- Portable save-location metadata uses paths relative to the prefix or game installation and remaps the prefix username on another device.
- Google Drive sync uses modification times and a previous-sync manifest, first-sync conflict prompts and deletion safeguards. It is not a general concurrent/offline conflict-merging system.
- Registry copying exists elsewhere in LVNM, but do not assume registry saved progress is part of cloud file sync.

Docs report Google Drive requires the user to create their own Google Cloud OAuth client. Force-exit from Steam Gaming Mode can terminate the launcher before post-exit sync.

Recommended improvements: recipe-defined save paths before heuristics; manual confirmation fallback; local snapshots before replacing files; persistent pending-sync state; stable game/release IDs instead of display-name identity. A save-location recipe is separate from a sync transport, so a local sync-folder workflow is worth evaluating without expanding backend scope.

### Text extraction

`lvnm/launchers/launcher_wine_game.py::run_texthooker` launches an external hooker executable using the game's prefix. Textractor gets an automatic target-executable argument. Wiki reports Luna and Agent also tested, with attachment through their interfaces.

LVNM is not an integrated text extraction/reader/dictionary/OCR implementation. Its Proton hooker branch currently bypasses umu and has a TODO to revisit that integration. Test attachment, runner/container consistency and Gaming Mode window behavior explicitly.

Initial scope: launch an external hooker and remember per-game settings. An extracted-text panel/clipboard bridge can be evaluated later; a complete learning suite is outside scope.

### Timer

`lvnm/timetracker/` provides desktop focus-aware tracking, session CSVs, periodic persistence and statistics. Gaming Mode uses game-process-open time rather than desktop focus. Optional swayidle AFK detection is documented as unavailable in Deck Gaming Mode.

`log_manager.py::sync_tracking_to_gdrive` replaces the whole CSV according to modification time; independent offline sessions are not merged. Its log identity uses executable name plus file size, so patches/shared executables can affect identity.

Recommendation: stable installation identity and unique session IDs; merge session records across devices and update existing sessions without double counting. Reading without input should not automatically count as AFK. Validate suspend/resume behavior.

### Metadata

`lvnm/vndb_manager.py` provides VNDB name autocomplete, ID lookup, cached artwork and release-image fetching. SteamGridDB is an additional image source.

The VN request fetches several descriptive fields, but `model/game_card.py` and the inspected sidebar update handler chiefly persist VNDB ID, original Japanese title and artwork references. Do not confuse fetched fields with a complete persistent metadata catalog.

The game model needs release/build/patch identity and recipe version for exact configuration matching.

### Packaging and reuse

LVNM provides AppImage and Flatpak builds. The AppImage relies on additional host libraries; Flatpak bundles more dependencies/locale support. Directory settings exist, but config defaults retain a central data directory. A fully relocatable single-root package has not been demonstrated.

No LICENSE was present in the inspected tree. `flatpak/io.github.uunniiblog.lvnm.metainfo.xml` explicitly states that upstream does not publish a license and uses `LicenseRef-proprietary`. Clarify permitted reuse with the author before adopting a fork or copying implementation. No author contact has been made or authorized; independent implementation remains an option.

## First continuation task

Continue engineering planning from this context. Recheck current LVNM and compare extending its importer/model with an independently implemented frontend. Preserve the narrow scope. Identify the minimum changes and propose a concrete first implementation milestone. Do not treat prior architecture suggestions as settled requirements or claim VNs work without testing them.

Suggested first milestone: one existing DRM-free VN directory, manual exact-release selection, one curated recipe, pinned runner, dedicated prefix, verified text/movies/audio/save-load, offline relaunch and a library-move test. Then add fingerprints for the tested releases. Rance 01 MangaGamer is an example candidate, not an already verified recipe; actual game files are not provided.

## Primary references

- LVNM source at reviewed commit: https://github.com/uunniiblog/linux-vn-manager-lvnm/tree/82ab14c542ffd0b35c1ef131e337f2b203712863
- LVNM wiki: https://github.com/uunniiblog/linux-vn-manager-lvnm/wiki
- LVNM Google Drive setup: https://github.com/uunniiblog/linux-vn-manager-lvnm/wiki/Google-Drive-Cloud-Project-Setup
- umu: https://github.com/Open-Wine-Components/umu-launcher
- umu CLI docs: https://github.com/Open-Wine-Components/umu-launcher/blob/main/docs/umu.1.scd
- GE-Proton: https://github.com/GloriousEggroll/proton-ge-custom
- umu database: https://github.com/Open-Wine-Components/umu-database
- protonfixes: https://github.com/Open-Wine-Components/umu-protonfixes
- VNDB API: https://api.vndb.org/kana
- Bottles CLI: https://docs.usebottles.com/advanced/cli
- Lutris recipes: https://github.com/lutris/lutris/blob/master/docs/installers.rst

Handoff prepared 2026-10-08. No repository was created, no application code changed, and no external Codex task was launched by preparing this brief.

## Full transcript of the last LVNM and scope response

The following is the complete assistant response, preserved verbatim. Citation markers refer to retrieval results in the originating ChatGPT conversation; the source links elsewhere in this handoff can be used independently.

---

**LVNM is quite close to the scope you’re describing, Onii-chan.** Save sync, text-hooking integration, playtime and metadata all support the same activity: reading VNs across your devices. They fit comfortably alongside automatic configuration.

I inspected the source at commit [`82ab14c`, dated October 5](https://github.com/uunniiblog/linux-vn-manager-lvnm/tree/82ab14c542ffd0b35c1ef131e337f2b203712863). This is a code review, not a runtime test on the Deck.

The most promising discovery: **its configuration importer already does much of the setup work we need.** Given a JSON preset, it can download the specified runner, create a prefix, install codecs/Winetricks dependencies, configure fonts and register the game. Automatic identification and recipe selection could sit in front of that. [Importer source](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/main/lvnm/ui/import_game_dialog.py)

**Save discovery is heuristic, but the sync implementation has some useful foundations.**

Its detector:

- Checks beside the executable for `savedata`, `UserData` or `save`.
- Searches the prefix’s Documents, Saved Games and AppData directories.
- Matches folder names against the game name, executable name and Japanese title.
- Prefers exact matches, then more deeply nested matches.

It doesn’t monitor which files the game writes. Unusually named saves, scattered files or registry-based progress can therefore escape detection.

The current code supports **multiple save folders, folder exclusions, or individually selected files**. It also uploads location metadata relative to the game directory or prefix, remapping the Windows username on another machine. Once configured on your desktop, the Deck can reconstruct those locations despite different installation paths. [Save-management source](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/main/lvnm/savedata_manager.py)

Sync uses Google Drive, modification times and a previous-sync manifest. There are first-sync conflict prompts and mass-deletion safeguards, but ordinary competing edits generally use the newer timestamp. That is not a full solution for playing independently on two offline devices.

Two practical catches are documented:

- Google Drive setup requires creating your own Google Cloud client.
- Force-exiting through Steam can terminate the launcher before its post-game upload runs. Normal in-game exit is the documented workaround. genui{"citation":{"refs":["turn12view1","turn11view0"]}}

**Our configuration database could improve save discovery substantially.** The same release recipe that supplies the Proton version can supply exact save paths, file patterns and exclusions. I’d use this order:

1. Known save mapping from the recipe.
2. Previously confirmed mapping downloaded from sync.
3. Heuristic suggestions.
4. Manual selection.

For unknown games, a later “make a save, then show changed files” helper could assist discovery without pretending every modified file is saved progress.

I’d also add local snapshots before overwriting saves and a persistent “upload pending” state that survives launcher termination. Those directly address the desktop-to-Deck workflow.

**Text extraction currently means launching an external text hooker.**

You configure its executable, then select **Open texthooker** on a running game. LVNM uses the game’s prefix; for Textractor it supplies the target executable name automatically. The wiki also reports testing Luna and Agent, with attachment handled through their interfaces. It warns that some hooks behave differently under Wine/Proton. genui{"citation":{"ref":"turn12view2"}}

LVNM itself doesn’t provide an integrated extracted-text panel, dictionary or OCR pipeline. In the Proton branch, the hooker launch currently bypasses umu and includes a TODO to revisit that integration—something I’d explicitly test before adopting it. [Hooker-launch source](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/main/lvnm/launchers/launcher_wine_game.py)

For our scope, **launching the hooker with remembered per-game settings is enough initially**. An optional text panel or clipboard bridge could follow. Building a complete language-learning suite would be a separate project.

**The timer is useful, with an important difference between desktop and Gaming Mode.**

| Environment | What LVNM measures |
|---|---|
| Supported desktop sessions | Time while the game window is focused |
| Steam Deck Gaming Mode | Time while the game process remains open |
| Optional AFK detection | Uses `swayidle`; documented as unavailable in Deck Gaming Mode |

It records sessions in CSV files, supports periodic persistence and shows playtime statistics. genui{"citation":{"refs":["turn11view0","turn12view2"]}}

However, **playtime sync uploads or downloads the entire CSV according to modification time**. It doesn’t merge independent sessions. From the code, that means playing offline on both devices can cause one device’s history to replace the other’s. Its tracking identity also uses executable filename plus file size, which can change after a patch. [Tracking source](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/main/lvnm/timetracker/log_manager.py)

I’d retain session tracking but assign each game a stable ID and each session a unique ID. Then syncing can merge sessions and update an existing session without double-counting it. AFK detection should remain optional: spending several minutes reading without touching a controller is normal VN usage.

**Metadata fetching already covers the launcher basics.**

There’s VNDB name autocomplete, explicit VNDB-ID selection, cover downloads, Japanese-title retrieval and an image picker using VNDB releases. SteamGridDB provides another artwork source.

The VNDB request fetches fields including description, release date, languages and rating, but the inspected game model and update handler chiefly retain the VNDB ID, Japanese title and artwork references. Fetching those fields does not yet amount to a rich persistent metadata catalog. [VNDB client](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/main/lvnm/vndb_manager.py) · [Game model](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/main/lvnm/model/game_card.py)

More importantly for us, the model lacks a dedicated **release ID, patch identity and recipe version**. Those would need adding to distinguish “this VN” from “this particular MangaGamer build.”

**Automatic configuration is a natural extension of what LVNM already does.**

The repository includes six engine/developer presets: BGI, Kirikiri, Liarsoft, Minori, Nitroplus and Siglus. They contain concrete runner and prefix settings; they aren’t an automatic release-matching catalog. [Existing presets](https://github.com/uunniiblog/linux-vn-manager-lvnm/tree/main/game_import_jsons)

One importer behavior I’d change: it can reuse an existing prefix by name and skip creation. A reliable automatic setup should verify that prefix’s configuration or create a dedicated one.

The scope I’d settle on is:

| Feature | Intended scope |
|---|---|
| Library | Add directory, identify/search VN and release, fetch artwork |
| Setup | Select tested recipe, install runner/dependencies, retain manual overrides |
| Saves | Recipe-defined locations, backups, one sync backend, conflict handling |
| Text | External hooker integration and remembered settings |
| Playtime | Sessions, totals and reliable cross-device merging |
| Deck interface | Controller-friendly browsing, launch and visible sync status |

The recipe becomes the shared source for **launch settings, save locations and optional hook settings**. That makes the features reinforce each other without expanding into unrelated functionality.

There is one concrete reuse issue: LVNM’s own [package metadata](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/main/flatpak/io.github.uunniiblog.lvnm.metainfo.xml) explicitly says the repository does not publish a license and labels it `LicenseRef-proprietary`. **I’d clarify that with the author before choosing a code-reuse route.**

My recommendation is to evaluate extending LVNM’s existing importer and game model first. The new work would be the identification wizard, release recipes, stronger sync identity/conflict handling and Deck-friendly presentation. Its existing implementation gives us a much more concrete starting point than another generic launcher from scratch.
