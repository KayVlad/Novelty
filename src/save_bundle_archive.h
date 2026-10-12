#pragma once

#include <QtCore/qstring.h>

namespace novelty {

// Versioned, bounded stream of one verified schema-2 portable save bundle.
// The destination directory for extraction must not already exist.
void writePortableSaveArchive(const QString &bundleDirectory, const QString &archivePath);
void extractPortableSaveArchive(const QString &archivePath, const QString &destinationDirectory);

} // namespace novelty
