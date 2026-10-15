#pragma once
#include "core.h"
#include <QtCore/qurl.h>

namespace novelty {
struct ManagedRunner { QString version, path; bool verified = false; qint64 archiveSize = 0; };
struct GeRelease { QString version, published; qint64 archiveSize = 0; };
struct GeCatalog { QList<GeRelease> releases; QString error; bool fromCache = false; };
QStringList externalProtonLocations();
void addExternalProtonLocation(const QString &path);
void removeExternalProtonLocation(const QString &path);
QStringList protonInventory(const QString &libraryRoot);
bool runnerInstallationReady(const QString &libraryRoot, const QString &path);
bool geInstallInProgress(const QString &libraryRoot, const QString &version = {});
bool newerGeVersion(const QString &a, const QString &b);
bool isRunnerAlias(const QString &value);
QList<GeRelease> cachedGeCatalog(const QString &libraryRoot);
void cacheGeCatalog(const QString &libraryRoot, const QList<GeRelease> &releases);
QList<GeRelease> parseGeCatalog(const QByteArray &);
GeCatalog fetchGeCatalog(const QString &libraryRoot, const QUrl &endpoint =
    QUrl("https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases?per_page=100"), int timeoutMs = 10000);
QList<ManagedRunner> managedGeRunners(const Library &);
void syncGeProvisioningPolicy(const Library &, const QString &selectedRunner);
QString managedRunnerRemovalIssue(const Library &, const QString &version);
void removeManagedRunner(Library &, const QString &version);
}
