#pragma once
#include "core.h"

namespace novelty {

struct SteamShortcutResult { bool added = false; quint32 appId = 0; };
// Edits a selected Steam account's shortcuts.vdf. The caller supplies the account path.
SteamShortcutResult addSteamShortcut(const Library &, const Game &, const QString &shortcutFile,
                                     const QString &noveltyExecutable);

} // namespace novelty
