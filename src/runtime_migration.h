#pragma once
#include "core.h"
#include <stop_token>

namespace novelty {

QString legacyUmuRuntimePath();
// Copies a complete legacy runtime into Games and verifies its bytes before
// switching this library. The old runtime is retained for other libraries.
void migrateLegacyUmuRuntime(Library &, const QString &source, std::stop_token stop = {});

} // namespace novelty
