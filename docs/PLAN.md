# Novelty engineering proposal

Superseded for current scope and stack recommendations by [the staged MVP plan](MVP_PLAN.md). Retained as research history; the user's later scope puts the base launcher first, saves second, and all other features beyond MVP.

Prepared 2026-10-08. This continues the handoff's planning task. Nothing below establishes compatibility with any actual VN.

## Upstream recheck

GitHub's commit API returned `82ab14c542ffd0b35c1ef131e337f2b203712863` for LVNM `main`, dated 2026-10-05—the same revision as the handoff. The importer, model, package metadata, launcher and configuration were fetched through the GitHub connector. Shell access to GitHub failed DNS resolution; the connector supplied the source successfully. This was source inspection, not execution.

Rechecked findings:

- The importer resolves a named runner and queues its download/extraction if missing. It skips prefix creation when a prefix with the selected name already exists. Its setup path delegates codecs, Winetricks and fonts to prefix management. [Importer](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/82ab14c542ffd0b35c1ef131e337f2b203712863/lvnm/ui/import_game_dialog.py)
- `GameCard` contains VNDB identity and save mappings but no dedicated release, build, installation or recipe identity. `from_dict` discards unknown fields: an external tool cannot simply add those fields to an import and expect them to survive. [Model](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/82ab14c542ffd0b35c1ef131e337f2b203712863/lvnm/model/game_card.py)
- Package metadata still says no license is published and declares `LicenseRef-proprietary`. Treat code reuse as unresolved; independent implementation can proceed without copying LVNM. No author contact has been made. [Package metadata](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/82ab14c542ffd0b35c1ef131e337f2b203712863/flatpak/io.github.uunniiblog.lvnm.metainfo.xml)
- The external hooker path retains the TODO about moving to umu. Sharing a prefix alone does not establish successful attachment across runtime/container boundaries. [Launcher](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/82ab14c542ffd0b35c1ef131e337f2b203712863/lvnm/launchers/launcher_wine_game.py)
- Default metadata is under `~/.local/share/lvnm`; the default Proton directory is Steam's compatibility-tools directory. A single-root installation needs more than changing the import dialog. [Configuration](https://github.com/uunniiblog/linux-vn-manager-lvnm/blob/82ab14c542ffd0b35c1ef131e337f2b203712863/lvnm/config.py)

The handoff's detailed savedata/timer review remains prior research at this same commit; those modules were not independently re-audited in this pass.

## Extending LVNM versus an independent frontend

| Area | Extend LVNM | Independent Novelty |
|---|---|---|
| Starting point | Existing library, import/setup UI and associated features | Build a small import/setup/launch interface |
| Release recipes | Change model, serialization and importer; add migrations and recipe lock | Make release and recipe identity part of the initial model |
| Prefix isolation | Replace name-based reuse with installation ownership and verified setup state | One prefix per installation from the outset |
| Managed storage | Audit settings, defaults and path persistence across existing modules | Central root/path resolver from the outset |
| Save/session reliability | Migrate display-name and executable-based identities and existing sync behavior | New identity and conflict rules, but all implementation remains to do |
| Deck controls | Existing upstream work still needs assessment and testing | Small controller-navigation prototype before committing to the UI stack |
| Reuse status | Published permission remains unresolved | No LVNM implementation copied |

Recommendation: build an independent, small frontend around umu. LVNM remains a useful behavioral reference. An importer-only extension is attractive for a quick experiment, but it would not meet the identity, storage and sync requirements by itself. Revisit reuse if suitable permission becomes available and the migration cost is acceptable.

A third option is an independent recipe tool that exports LVNM import files. This could test setup recipes cheaply, but would require a separate lock/identity store and would not solve LVNM's broader persistence behavior. It is an optional interoperability feature, not the proposed product foundation.

## Proposed technical boundaries

Python with PySide6 is a candidate, not a final selection. Keep a UI-independent core and prove the execution lifecycle through a small CLI first. Evaluate Qt Quick navigation on the Deck before choosing it over a simpler widget interface. No web frontend or server is needed for the first milestone.

Use umu as a subprocess backend with explicit `WINEPREFIX`, an absolute pinned `PROTONPATH`, cwd and argument array. Its manual documents standalone execution without Steam, prefix creation and Winetricks invocation. `UMU_RUNTIME_UPDATE=0` disables runtime updates, but does not provision an absent runtime. Record the installed runtime and umu versions as well as the Proton version; a runner pin alone does not reproduce the whole environment. [umu CLI manual](https://raw.githubusercontent.com/Open-Wine-Components/umu-launcher/main/docs/umu.1.scd)

VNDB supplies VN/release metadata. Novelty's own recipe catalog associates tested builds with configuration and fingerprints. Metadata selection is not proof of an installed build match. Cache confirmed metadata for offline use; identification can remain manual initially. [VNDB API](https://api.vndb.org/kana)

Proposed records:

- VN and release IDs; separate build/patch description and fingerprint set.
- Local installation UUID with game-directory reference, independent of display name.
- Versioned recipe with runner URL/checksum, cwd/executable/arguments, environment, ordered setup actions, save mappings and evidence.
- Resolved installation lock holding the selected recipe content/digest, actual runner/runtime versions, completed setup actions and local overrides.
- Later: a save-compatibility key shared across compatible installations/devices, plus unique session/device IDs. A local installation UUID alone cannot identify corresponding saves on another machine.

Draft storage layout:

```text
<library-root>/
  library.json
  installations/<uuid>/installation.json
  installations/<uuid>/recipe.lock.json
  prefixes/<uuid>/
  runners/<version>/
  recipes/
  snapshots/<save-key>/
  sessions/
  cache/
  logs/
```

Store managed paths relative to the library root. Preserve external game locations with a relink action when unavailable. Investigate supported umu runtime/cache placement before promising that everything relocates. Do not redirect the user's HOME globally. The development project directory and the eventual game-library root are separate concepts.

## First implementation milestone: one reproducible game

Deliver a small runnable setup/launch prototype for one user-provided DRM-free VN release. Rance 01 MangaGamer remains an example only; no game files or verified configuration have been supplied.

1. Define and validate the recipe, installation and lock formats. Reject invalid paths and incomplete runner pins. Use synthetic fixtures for these checks; label them untested for game compatibility.
2. Import a directory in place and manually select the exact release/build and executable. Present the resulting setup plan. No automatic identification claim yet.
3. Download and verify one fixed runner artifact, prepare one dedicated prefix and journal setup progress. Failed setup must stay visibly incomplete and retry safely. Serialize operations on the same prefix.
4. Launch through umu, retain logs and record start/exit with a durable session ID. Handle child-process handoff and interrupted sessions; prototype process-open time with explicit labeling. Suspend time should not inflate reading time.
5. Confirm recipe save mappings after creating a real save. Make and restore a local snapshot while the game is closed. Treat registry-based progress as an explicit mapping/test requirement when applicable.
6. Run the acceptance checklist below and record evidence with the exact build, runner, runtime, device and date. Only then label the recipe tested.

Acceptance criteria:

- Cold setup completes; repeating it does not redo successful destructive operations.
- Text/fonts, opening movie where present, audio, input, save/load and relaunch work on the chosen release.
- Relaunch works with networking unavailable after initial provisioning.
- A failed download or setup step is recoverable; normal exit and forced termination leave understandable state.
- Suspend/resume preserves the game and produces reasonable session duration.
- Move the managed root to a path containing spaces; verify launch and save/load. Test an external game-directory relink separately. Record any remaining absolute-path dependencies instead of calling it fully portable.
- Run on Steam Deck and Bazzite desktop before claiming both are supported. Include Deck Gaming Mode exit behavior, while keeping Steam optional for DRM-free execution.

Build-time tests can cover path resolution, checksum failure, setup retries, lock persistence and lifecycle handling with a fake backend. Those tests cannot establish Wine/Proton compatibility; the game checks require the actual files and devices.

## Following milestones and decisions

After the first recipe passes, add fingerprints for those tested files and cached metadata/artwork. Then prototype one external text hooker in the same execution environment, verifying attachment, extracted text and Gaming Mode window behavior.

Save sync should follow working local backup/restore. Start by evaluating a user-selected sync folder as one transport. Use content manifests and a shared base to detect two-device divergence, preserve both versions on conflict, snapshot before replacement and persist pending work across termination. Do not live-sync mutable prefixes. Registry saves and incompatible release formats need explicit treatment. Merge playtime by session ID rather than replacing an entire history file.

Decisions still open: first game/release and its directory; independent implementation versus a permitted upstream extension; UI stack; packaging; strict relocation requirements; sync transport. The most useful next input is the first game available for testing. No dependency installation, runner download, game execution or application implementation occurred during this planning pass.
