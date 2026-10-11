# Library layout revision

The [2026-10-10 product direction](PRODUCT_VISION.md) governs future UI work: one fast game lifecycle manager with progressive disclosure. Add the game folder or install it, select the executable when needed (a sole candidate is selected automatically), then use metadata/setup defaults and Play. Later continue with installation and synced saves on another device. Manual metadata/save paths, runner selection and advanced settings remain editable. Steam scope is Add to Steam targeting Novelty's managed launch. The controls below describe the current implementation, not a requirement to retain its number of tabs or dialogs.

Local visual references informed this revision of the launcher presentation. The reference images are excluded from the source repository; the staged engine/setup and saves work stays on its existing plan.

The header shows the Novelty title, Add game and Settings. Games folder selection is inside Settings. Search, the grid/list toggle and Rescan form the next row. The local card reference informed the artwork layout; the user subsequently removed its buttons. Cards use full-height artwork with a dark bottom fade, title and engine at bottom left, and the status light at bottom right. Double-click or Enter launches; right-click opens Manage/Delete. List headings and row text share left alignment and horizontal padding. Grid and list share their selection, and the chosen view is remembered. In list view, right-click offers Play/Manage; Ctrl+M opens Manage. When a game has no executable, the first Play attempt opens the same executable picker used after Add, then continues into Play if a choice is saved.

The right sidebar stays 340 pixels wide and contains the full uncropped cover at its native aspect ratio, title, status, description, tags and playtime. Taller artwork uses the existing sidebar scroll area. A missing cover keeps generated artwork at its previous height. Configuration paths, engine/runtime settings, last-played/language fields and sidebar Play/Manage buttons have been removed. Status-related errors remain visible when needed. The layout supports 1280×800 and a 900×620 minimum window.

Recovery required blocks Play/Manage for the affected game while other available games remain usable. A lost umu supervisor requires a system restart in the current implementation. Interrupted removal instead asks for Rescan; startup/Rescan recovery runs in the discovery worker and preserves conflicting files. The sidebar explains which recovery is needed.

The Games path at the bottom right is a link that passes its local-file URL to the default file explorer. The full path is available on hover. Manage opens on a read-only Overview tab containing the saved configuration; Metadata, Launch, Runtime, Advanced and Logs remain available.

## Full removal

Manage → Remove game and card Delete use a compact Yes/No warning, with No selected by default. An unchecked, disabled **Delete synced saves** checkbox reserves the future save-sync choice; Novelty does not manage synced saves yet. No makes no deletion. Yes removes the game directory, Novelty's dedicated prefix (including local saves stored there), per-game settings/logs and the library record. Files/saves outside those directories remain untouched. Steam games and games with an untracked Steam launch request have removal disabled, with a visible explanation: Novelty cannot confirm that they have stopped. The core enforces the same guard even after changing backend. This is a limitation of the existing legacy Steam backend; planned Add to Steam shortcuts will invoke Novelty's supervised launch.

The core checks the game identity/configuration again under its launch lock and a database write transaction. Running/preparing games, unresolved process recovery records, changed folders, linked target directories, broad/personal/library directories, overlapping game folders and folders used by another game's runner are refused. Internal symlinks are unlinked without recursively deleting their external targets. Missing game folders can remove their leftover Novelty data.

Game and prefix directories are staged by renaming beside their originals on the same filesystem. A durable database intent precedes these moves; deletion authorization commits with the game-record deletion. Startup/Rescan restores interrupted uncommitted moves, or resumes committed cleanup, outside the UI thread. Directory/parent identities are rechecked; replacements, links and conflicts are preserved and reported. Pending jobs survive failed cleanup for retry. Actual power-loss/SD-card acceptance remains pending, and old unjournaled staging folders require manual inspection. Cancellation is offered before confirmation; permanent cleanup itself cannot be cancelled.

## Metadata

Manage opens on Overview; its Metadata tab lets the user search VNDB, select a result, refresh the linked entry or unlink it. The tab shows the original title, description, clickable VNDB ID, expected play time, developer, all distinct publishers found across releases, and non-spoiler content tags. The game keeps its local folder title. The sidebar adds the original title in small text below it and displays the cached description, tags, play-time estimate and cover. Grid cards use the cached cover too. Empty values retain clear placeholders and generated covers.

Search and refresh are also available manually. New games try VNDB matching automatically; cached details and a bounded local cover work offline. Opening the launcher makes no metadata requests. VNDB metadata does not change launch settings or measured playtime. Tag editing and save management are not included. The mock's example games/artwork are not imported.

The reviewed Metadata tab preview is `artifacts/manage-metadata.png`. Search results collapse after linking so the saved details remain visible.

Cards use a native Qt item delegate so the library does not allocate a separate collection of widgets for every installation. The original table remains available as the list view. This follows Qt's [QListView](https://doc.qt.io/qt-6/qlistview.html) and [QStyledItemDelegate](https://doc.qt.io/qt-6/qstyleditemdelegate.html) interfaces; no web runtime or extra UI dependency was added.

## Validation

The UI integration test exercises view switching and shared selection, card double-click launch and right-click Manage/Delete, search and empty results, metadata placeholders, bottom-right status light and the small window. A focused UI fixture checks first-Play executable selection, cancellation and launch continuation, plus full portrait/landscape sidebar covers at the unchanged width. A nine-entry fixture verifies three visible columns, card context menus and scrolling to the last entry. Existing detached-launch, discovery, rename, relink and unavailable-library checks remain active. Removal tests additionally cover running locks, stale configuration/folders, overlapping folders, linked prefixes, internal symlink targets, missing-game cleanup and database-failure rollback. UI checks No and Yes using temporary fixtures, verifies the disabled synced-saves checkbox, and checks the default Overview and Games folder in Settings.

Development previews are saved under ignored `artifacts/`. The smaller fixture preview can be regenerated with:

```sh
NOVELTY_UI_PREVIEW_DIR="$PWD/artifacts" ./scripts/build.sh
```

Preview the real test library at Deck resolution:

```sh
QT_QPA_PLATFORM=offscreen ./build/novelty --root /home/deck/TestGames --screenshot artifacts/layout-revision.png
```

Warm/offscreen 500-entry measurement for this revision: first queued UI event 112 ms, RSS 48,412 KiB, zero CPU ticks over two idle seconds. This is a development measurement, not a cold boot or Gaming Mode test.

## 2026-10-09 — GE-Proton installation

Settings and Manage → Runtime include Get GE-Proton. The version dialog keeps the UI responsive during download/extraction, supports Cancel/window-close, and returns a validated installed path to the runner selector. Save the enclosing dialog to apply that selection. Manage disables this action for a saved Steam backend. The preview is `artifacts/ge-installer.png`; fixture tests cover cancellation and asynchronous completion.

Manage → Runtime also offers pinned setup profiles and Use detected default. Save applies the profile; Cancel leaves the saved configuration intact. Other tinkering fields and prefix data are preserved. Overview shows the saved profile. Runtime uses a scroll area to keep bottom Save/Cancel actions available; `artifacts/manage-runtime.png` shows this revision.

Settings also includes Get umu. It installs the pinned launcher asynchronously, allows retry/cancellation and fills the executable field on success; Save selects it for the library. Preview: `artifacts/umu-installer.png`. Default backend/Proton affect new games; the umu executable applies library-wide.

Wine configuration and Registry editor actions appear below Prepare in Manage → Runtime. They use saved settings, close Manage, and show Configuring while the detached supervisor owns the prefix. Play/Manage remain blocked for that installation until tools and descendants exit. Steam games show these actions disabled. Tool UI interactions and launcher-close survival are fixture-tested; the updated preview is `artifacts/manage-runtime.png`.

## 2026-10-09 — Run another executable

Manage → Runtime includes Run another executable for umu games. A separate dialog collects the Windows `.exe`, optional cwd and one argument per line; it validates the paths before enabling Run. Cancel leaves Manage open. Run closes Manage and uses saved settings through the detached supervisor. Game Play/Manage remain blocked while its prefix is in use. Manage → Logs includes the last operation record. Preview: `artifacts/run-executable.png`. Fixture coverage includes cancelled/valid requests, bad cwd, Steam disabling and survival after destroying the launcher window.

## 2026-10-09 — Add folder or run installer

Add game opens Select folder / Run installer tabs. Select folder moves an external game folder to a direct child of Games, then detects its executable/engine and creates the entry there. A direct child is scanned in place; an existing entry keeps its configuration. A transfer from another filesystem copies into a hidden staging folder and verifies the copy before retiring the source. The modal transfer notice keeps the UI responsive. Run installer selects an external Windows `.exe`, a fresh folder name inside Games, a Proton runner and optional Japanese text locale. It shows the destination before starting: select `N:\` in the installer. Installer dialogs still control their own destination; Novelty does not guess vendor-specific silent flags. New installer entries use umu even when the default for ordinary imports is Steam.

The installer uses its source folder as cwd for adjacent media files. The UI may close while the detached supervisor continues. After normal completion, detection updates the same game entry and retains the prefix. Pending installs cannot Play. Manage → Overview offers Run / retry installer and Detect installed files, with an editable installer path when the source moves. No files are deleted on installer failure/cancellation. The same Yes/No deletion warning guards Delete from the card menu.

Previews: `artifacts/add-installer.png`, `layout-grid-fixture.png`, `layout-list-fixture.png`, `layout-small.png`. Unlinked games still use generated cover art; no example artwork was imported from the reference.


## 2026-10-09 — Diagnostics controls

Manage → Runtime includes a one-launch Wine debug button beside the other supervised tools. It is disabled for Steam or pending installs. Manage → Logs can export a JSON report from the saved installation and current log. The current log remains viewable in the tab, and the export tooltip explains that paths/logs may need review before sharing.
