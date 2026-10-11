#pragma once
#include "core.h"
#include <QtCore/qfile.h>

namespace novelty {
QString runtimeRecoveryIssue(const Library &, const QString &id);
// Headless supervisor only: adopts/waits for descendants, never runs on the UI thread.
int executeTracked(const Library &, const QString &id, const Command &, QFile &log);
}
