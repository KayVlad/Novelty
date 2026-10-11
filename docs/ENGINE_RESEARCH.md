# Engine evidence and compatibility tests

## Generic Proton baseline

Implemented: one prefix per installation, explicit Proton directory, correct cwd, argument array, inherited locale unless overridden, `HOST_LC_ALL` and `LANG` for the selected locale and `umu-default` for unmanaged games. No blanket native codec/DLL or Winetricks installation.

Preparation uses the documented empty executable invocation. Selected settings and runner version are recorded locally. [umu CLI manual](https://github.com/Open-Wine-Components/umu-launcher/blob/main/docs/umu.1.scd) documents the standalone interface. [Valve's runtime-options documentation](https://github.com/ValveSoftware/Proton/blob/proton_11.0/README.md) identifies `HOST_LC_ALL` as its locale override.

The user's `installer.png` shows mojibake in an old Japanese InstallShield window. The Deck has `ja_JP.UTF-8` and Japanese CJK fonts. Add game now offers Japanese text for the installer and persists that locale for its game; Manage can change it for an existing entry. Proton documents `HOST_LC_ALL`; a [Proton Japanese-locale issue](https://github.com/ValveSoftware/Proton/issues/9156) reports a case requiring `LANG` too, so Novelty passes both for an explicit locale. This is a plausible fix, not a verified result on the user's installer: no real installer or prefix was rerun during implementation.

Status: tested with a fake backend for execution mechanics; actual clean-prefix compatibility pending. No universal VN compatibility claim.

## Template!! — first supplied installation

User supplied a copy under `/home/deck/TestGames/Template!!` on 2026-10-08 and authorised testing.

Inspection findings:

- Top-level Windows launcher: `Tenpure.exe`.
- Bundled `renpy/__init__.py`, `game/` and Windows Python runtime.
- Existing copied log reports Ren'Py `8.5.2.26010301`, game `1.0.0`, built 2026-06-26. This is historical information from the supplied copy, not a new run.
- Archives use names such as `code.xp3.rpa` and `movie.xp3.rpa`. The actual final extension is `.rpa`; matching a substring `xp3` would incorrectly suggest Kirikiri.
- Bundled Linux launcher also exists, but the selected test path is the Windows executable through Proton, as required for the launcher backend test.
- Existing save files are present. No game execution or save changes occurred in this pass.

Detection now checks for the Ren'Py runtime plus game directory and selects the sole plausible Windows launcher, excluding the runtime helper executables. The [official Ren'Py build documentation](https://www.renpy.org/doc/html/build.html) describes distribution layouts and platform builds. Detection identifies an engine family; it does not validate an exact patched build.

Initial setup hypothesis: generic Proton prefix first, using the game's bundled runtime and assets. Additional codec, font and graphics overrides require evidence from this build's actual launch. No Ren'Py-specific dependency installation is asserted as necessary or enabled.

Test runner selection: existing Proton 10.0, with its version recorded. Existing umu sniper runtime found under `~/.local/share/umu/steamrt3`. umu itself was not available on PATH or in the test library. The official pinned zipapp download failed sandbox DNS resolution, so the actual game launch remains pending.

| Check | Result |
|---|---|
| Discover correct executable/engine | Passed by file inspection and launcher preview |
| Prefix creation through real umu | Passed: dedicated prefix created by umu 1.4.4 / Proton 10.0; first session exited with code 0 |
| Title screen/text/fonts | Passed: user confirmed the requested gameplay checks work perfectly |
| Movie/audio/input | Passed by user confirmation of the requested checks; individual media samples not independently inspected |
| Save/load | Passed by user confirmation, using the test copy |
| Normal exit and offline relaunch | First normal exit passed; second launch running; offline behavior pending |
| Suspend/resume and Gaming Mode | Pending |

2026-10-08 follow-up: user installed umu and ran Template!!. The journal recorded Ready at 17:54:13 UTC after the first session. A second supervised launch started at 18:03:32 UTC and reached a rendered startup splash. The game log reports its Windows Ren'Py runtime using ANGLE on the Deck's AMD GPU. No additional dependencies or overrides were added. This establishes launch/prefix mechanics, not completed gameplay, movie/audio or save/load validation. Awaiting user observations; the second session was left open for those checks.

User then confirmed “works perfect” in response to the text/fonts/sound/movie, fresh save/load, closing Novelty while playing, and quit/reopen/relaunch checklist. Those requested checks are recorded as user-reported passes; the launcher journal also returned to Ready. Offline, suspend/resume, Japanese installation paths and Gaming Mode were not part of that confirmation and remain separate gates. This result supports the generic profile for this particular game/build and Proton version, without asserting compatibility for every Ren'Py title.

## GE-Proton follow-up

2026-10-08 GE-Proton follow-up: the user reports that GE 7-18 crashes/exits with a “Windows 10 required” error and suspects an incompatible runner. This is a **user-reported failure**, not a diagnosed cause. The Windows version advertised by the existing prefix, the runner, and this game build have not been isolated. No Windows-version override, prefix reset or runner change was made in response; the user requested continuing launcher development. The earlier Valve Proton 10 pass remains separate evidence. A supported, pinned GE baseline still needs its own clean acceptance run.

## Subsequent engines

Kirikiri, BGI/Ethornell and Siglus remain research candidates. Only advisory Kirikiri/Siglus signals are present; neither has an enabled engine-specific setup. Kirikiri's [own XP3 archive source](https://github.com/krkrz/krkrz/blob/master/base/XP3Archive.cpp) supports using the actual `.xp3` extension as one candidate signal. Require engine-version evidence and representative game tests before adding setup actions. LVNM implementation/preset code was not copied.

## 2026-10-09 — First versioned profile baseline

Rechecked the [official distribution documentation](https://www.renpy.org/doc/html/build.html): distributions include engine files, and current generic Windows requirements include Windows 10 or later. These observations support starting with the supplied runtime and avoiding blanket dependency changes. They do not isolate the cause of Template!!'s GE 7-18 failure or establish compatibility across patched engine builds.

`renpy-bundled-v1` now names this conservative baseline for new detected Ren'Py installations. Its actual preparation is identical to `generic-proton-v1`; no fonts, codecs, renderer overrides or Windows-version registry edits are installed. Existing installations stay pinned to their previous generic behavior until explicitly changed. Mechanical/profile tests pass; clean GE gameplay, media and offline acceptance remain pending.
