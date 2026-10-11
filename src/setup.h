#pragma once
#include "core.h"
#include <functional>

namespace novelty {
bool setupComplete(const Library &, const Game &);
// Caller holds the installation lock. Retries initialise the existing prefix in place.
void preparePrefix(const Library &, const Game &, const std::function<int()> &run);
}
