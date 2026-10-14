#pragma once
#include "core.h"
#include <functional>

namespace novelty {
bool setupComplete(const Library &, const Game &);
void validateWinetricks(const QStringList &verbs);
QStringList pendingWinetricks(const Library &, const Game &);
QString winetricksApproval(const Library &, const Game &);
// Caller holds the installation lock; each successful verb is recorded before the next starts.
void prepareWinetricks(const Library &, const Game &, const std::function<int(const QString &)> &run);
// Caller holds the installation lock. Retries initialise the existing prefix in place.
void preparePrefix(const Library &, const Game &, const std::function<int()> &run);
}
