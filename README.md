# Novelty

A game lifecycle manager for Linux and Steam Deck, with visual novels first and support for other Proton games. Add a game folder or install the game, select its executable when needed, then press Play with suitable metadata and setup defaults. A sole suitable executable is selected automatically. Without a save server, a fresh LAN installation adopts compatible initial saves; later P2P replacement is an explicit **Transfer saves** action with Undo. An optional self-hosted server supplies automatic save sync.

Status: folder/installer import, executable choice, editable settings, prefix preparation, generic VN/game and Ren'Py profiles, cached VNDB metadata, local save discovery and verified snapshots are implemented. New managed libraries can prepare missing GE-Proton, umu and a known Steam Linux Runtime on first Play. LAN pairing, save exchange and staged peer game installation are implemented; live two-device validation remains open. A native portable test package can be built, while clean-device packaging and platform acceptance remain open. Detailed planning and progress notes are kept locally under `docs/`, which is excluded from Git.

Steam Deck is the primary target; Bazzite desktop follows. Novelty uses progressive disclosure: simple defaults for ordinary play, with editable metadata, save paths, Proton versions and advanced configuration when needed. Stage 1 completes local installation/setup/play; Stage 2 adds saves and continuation across devices. Current folder import moves an external game folder into Games and prompts for an executable if detection is ambiguous or finds none. The umu path supports DRM-free execution without a Steam client/account. Planned Steam support is a shortcut that invokes Novelty's managed launch.

The distribution target keeps Novelty-owned files in its application folder and the chosen Games folder. A user should be able to install Novelty, add a game and play without separately installing Bottles, protontricks or ProtonUp-Qt. External prefix tools remain optional. Removing a game or the app and its Games folder should cleanly remove Novelty-owned data. This is a packaging and storage acceptance goal; the current development build has not passed that gate.

Implementation uses C++20, Qt6 Widgets and SQLite. The current build includes library discovery, executable/runner selection, per-game environment/arguments, prefix preparation and supervised umu launch. Add to Steam can create a shortcut that calls Novelty's managed launch. Settings can install pinned GE-Proton and umu versions with checksum verification; first Play can provision missing tools in app. Manage offers versioned generic/Ren'Py baseline profiles plus supervised Wine configuration/registry tools and one-time patch/installer execution in the same prefix. Verified local save snapshots, guarded restore and LAN transfer use the same mapped save locations.

Automatic discovery waits briefly for new folders to stop changing before import. Existing games remain available while this check runs. If a library disappears, reconnect it and use Rescan; the open window retains its entries. For umu games, renaming/moving the same folder within the Games directory follows its filesystem identity. Other moves use **Manage → Relink folder…**, preserving the installation ID, settings and prefix. Reopen the updated launcher and let the first scan finish before testing a rename.

Paired devices connect automatically while Novelty is open. Games available on a trusted device appear in the library with desaturated covers and **Not installed** status. Right-click → **Install**, or double-click the cover, to transfer a game; known saves travel through verified save snapshots separately. Install selects a local runner, prepares the receiving device's runtime and prefix, and applies one compatible peer save when there is no local progress, without starting the game. If setup is interrupted, Play retries it and continues with the peer save. Transfer addresses are remembered after authentication and rediscovered on the LAN when they change.

After installation, Manage → Saves offers **Transfer saves…** from a selected trusted device and **Undo last transfer…** when automatic server sync is off. Ordinary Play does not poll or apply P2P saves, including through a Steam shortcut. Verified empty saves can be transferred as intentional deletions. Settings can enable a self-hosted save server; the shared window/headless Play path checks accepted revisions and actual local files, uploads changed saves conditionally, allows offline Play, and preserves conflicts for an explicit choice in Manage → Saves. This server path compiles and has local archive/store/decision tests; real server and two-device acceptance remain open. Storage usage display, rolling retention and independent backup are follow-up work described in [the amended save RFC](docs/novelty_sync_rfc_v0.4.md).

For a local server trial, build `novelty-save-server` and run it with `--data /private/path`. It listens only on `127.0.0.1:42428` by default and writes an owner-only `access.token` inside that private data directory. Use a TLS reverse proxy for another device, then enter its HTTPS address and token in Settings on each Novelty installation. P2P installations share a server game key automatically; independently installed copies can set the same advanced **Server game link** in Manage → Saves. The server holds one current verified state per game key and retains earlier revisions for recovery; it is not an independent backup. Do not expose the plain HTTP listener directly to a network.

Pairing uses the machine names and a six-digit code: connect from one device, enter the code shown on the other, then confirm on the displaying device after the code matches. Update both devices for the new code-pairing screen. Existing trusted devices, library identities and saved history remain valid; they do not need pairing again. The previous phrase protocol remains supported by the transport for older clients using that mode. Cover previews are optional catalogue fields, so older peers can still exchange games without previews.

Build and run locally:

```sh
./scripts/build.sh
./run.sh --root /path/to/Games
```

Template!! previously passed user gameplay/save-load checks with Proton 10. The user subsequently reported a GE 7-18 failure; a clean GE compatibility baseline remains pending. Use Settings → Get umu / Get GE-Proton for runtime installation. The terminal alternative for umu is:

```sh
python3 scripts/install-umu.py /path/to/Games
```

This local build uses SteamOS's Qt 6.9.1 libraries; it is not yet a self-contained distribution. The script verifies a pinned upstream umu archive checksum and installs it inside the test library. A configured system umu installation also works.
