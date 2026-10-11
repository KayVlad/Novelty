# Novelty product direction

Established by the user on 2026-10-10. This is the current product scope and takes precedence over narrower scope statements in older proposals. [STATUS_CHECKLIST.md](STATUS_CHECKLIST.md) tracks implementation; this document describes the intended experience.

## Purpose

Novelty is a game lifecycle manager for Linux and Steam Deck, with visual novels as the first priority and support for other games that run through Proton. It owns the experience from adding and configuring a game through playing, installing it on another device and continuing with synced saves.

## Default experience

1. Add the game folder or install the game, then select its executable. Search the folder and automatically select a sole suitable candidate; offer selection when there are several or browsing when none is found. Keep required user configuration to a minimum. The executable choice follows adding the game.
2. Download metadata and select a suitable setup preset: a game preset, an engine preset, or a generic VN / generic game fallback. Prepare the required runtime and setup within Novelty's workflow.
3. Press Play. Novelty stays unobtrusive while the game runs and handles its lifecycle without requiring attention.
4. Close the game, later open Novelty and play again from the saved progress.
5. Move from the Deck to a desktop or another device. Open Novelty, install the game from a peer over the LAN with one button if needed, synchronize saves and continue from the saved progress.

“Continue where you left off” means carrying saved progress between sessions/devices; it does not imply migration of a running process.

## Interaction principles

- One self-contained manager with progressive disclosure inside Novelty. Installation, setup, metadata and saves belong to the same product experience.
- Fast startup, a responsive library and little overhead while playing are core requirements.
- Routine use should require choosing a game and pressing Play. Show extra controls when a user needs to correct something or deliberately chooses to configure it.
- Metadata matching and save-path detection should work automatically where possible, with clear manual correction when needed.
- Power users retain editable configuration, including metadata, save paths, Proton version, presets and advanced settings. Defaults should be opinionated; capabilities should remain flexible.
- Preserve explicit user overrides when resolving or updating presets. Explain the effective configuration when the user opens its details.
- Manage required components through the application so routine workflows do not require opening several other applications or navigating many configuration dialogs.
- Keep Novelty's owned files within its application folder and the chosen Games folder. Bundle or provision the required launch components from Novelty; installing Bottles, protontricks, ProtonUp-Qt or another manager must not be a prerequisite. External tools may still open a managed prefix when a user chooses to use them.
- A fresh install should reach a running game through Add and Play. Removing a game should remove its Novelty-owned prefix, setup data, cache and logs; removing Novelty and its Games folder should leave no Novelty configuration or runtime clutter in the operating system. Provide a clear way to retain game files or user saves when requested.
- Steam support is an Add to Steam shortcut invoking Novelty's managed game launch. Keep the product focused on this lifecycle and preserve editable settings within it.

## Delivery and current gaps

Build this experience in tested increments, following the [Lego priority list](STATUS_CHECKLIST.md). Stage 1 completes remaining setup gaps and delivers a packaged local product; current automatic VNDB metadata with manual browsing is sufficient for MVP. Stage 2 builds local save protection, paired-device save sync, then LAN game installation using those foundations. Device acceptance runs alongside delivery; Add to Steam is the final shortcut convenience in the current list.

The current build has local folder/installer import, direct executable choice after ambiguous Add, sole-candidate executable selection, manual executable editing, supervised umu launch with prefix preparation, runtime installers, generic VN/generic game/Ren'Py profiles and automatic VNDB metadata with an offline cache and manual correction. Generic VN/game profiles prefer a pinned GE build on first Play; missing GE, umu and known Steam Linux Runtime steps now run in one first-Play preparation dialog for new managed libraries. Remaining local work includes real-game validation of the Proton pin, the fuller preset hierarchy and release packaging. Broader metadata coverage and ambiguity prompts are deferred. Save management, LAN game installation and Add to Steam remain planned. The application/Games folder boundary is a release requirement, not a guarantee of the present development build: the test package, helper dependencies and remaining write paths still need clean-device audit. Broader integrations are outside the current delivery list.

## Subsequent clarification

The user clarified that “point to the exe” meant choosing the executable after adding a game, with automatic selection when only one is found. Folder import already exists and should be retained. The first four roadmap areas contain implemented features and must list only their remaining gaps. The user accepted the local saves → device sync → LAN installation order and limited the final integration block to Add to Steam.

The user also specified the installation boundary: Novelty should be self-contained in its application folder apart from the Games folder, require no separate prefix or Proton management applications, allow optional external prefix editing, and clean up its owned state when games or the application are uninstalled.

> The idea is for the app to be self contained in its folder (except for games folder), meaning you don't need a separate bottles install, protontricks, protonup-qt or any of that, though you can manage the prefixes with external apps if needed, one should be able to install the app and have a game running without much hasstle, and uninstall the games/app without leaving clutter in the os.

## User's original wording

> The scope is a game lifecycle manager (vn first but you can play anything proton). You add game, minimal user configuration (just point to the exe), metadata is downloaded, setup configured with presets, by game, engine or generic vn/generic game preset, u press play and forget novelty exists. You turn game off, go to your desktop, open novelty, open game, continue where you left off, install the game if needed (p2p lan sync) with one button, saves sync, play where you left.
>
> Ofc sometimes metadata needs manual intervention, saves need manual path and sometimes you want to change the proton version, or advanced settings, but thats the details. You dont go through tens of lutris submenus, you dont open five different apps that somehow botttles depends on, and you dont wait 30 seconds for heroic to open. its a self contained manager.
>
> Not three separate applications, not thirty configuration dialogs. Just progressive disclosure within Novelty.
>
> And for the power users, I'd absolutely keep everything editable. Novelty should be opinionated about its defaults, not restrictive about its capabilities.
