#pragma once

#include "saves.h"
#include "devices.h"
#include "runtime.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdir.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qlockfile.h>
#include <QtCore/quuid.h>
#include <QtCore/qsysinfo.h>
#include <QtCore/qset.h>
#include <sys/statvfs.h>
#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace novelty::save_detail {
[[noreturn]] void fail(const QString &message);
struct NoSaveData : std::runtime_error { NoSaveData() : std::runtime_error("No saved data yet in the confirmed locations.") {} };
bool exists(const QString &path);
QString token();
bool validToken(const QString &value);
bool validFingerprint(const QString &value);
bool profileGameMatches(const SaveConfig &config, const QString &profileGameId,
                        const QString &authenticatedPeer = {});
bool labelCompatible(const QString &expected, const QString &actual);
bool labelsCompatible(const SaveConfig &config, const QJsonObject &manifest);
bool mappingsMatch(const SaveConfig &config, const QJsonObject &manifest);
QString configPath(const Library &lib, const QString &id);
QString journalPath(const Library &lib, const QString &id);
QString pendingPath(const Library &lib, const QString &id);
QString headPath(const Library &lib, const QString &id);
QString observationPath(const Library &lib, const QString &id);
QString transferIntentPath(const Library &lib, const QString &id);
QString transferUndoPath(const Library &lib, const QString &id);
QString snapshotsRoot(const Library &lib, const QString &id);
QString currentHead(const Library &lib, const QString &id);
void setHead(const Library &lib, const QString &id, const QString &snapshotId);

void noLinks(const QString &path);
QString cleanMappingPath(const QString &path);
QString mappingRoot(const Library &lib, const Game &game, const QString &scope);
QString targetPath(const Library &lib, const Game &game, const SaveMapping &mapping);
void validate(const Library &lib, const Game &game, const SaveConfig &config);
QJsonObject configJson(const SaveConfig &config);
SaveConfig parseConfig(const QJsonObject &data, const QString &gameId);
QString recipePath(const Library &lib, const SaveConfig &config);
QList<SaveMapping> recipeMappings(const Library &lib, const SaveConfig &config);
void resolveDeclaredSavePaths(Library &lib, const Game &game, SaveConfig &config);
QString saveNameKey(const QString &name);
QList<SaveMapping> heuristicSaveMappings(const Library &lib, const Game &game);
void discoverSavePaths(Library &lib, const Game &game);

struct SnapshotBudget { qint64 bytes = 0; int entries = 0; };
QJsonArray inventory(const QString &path, SnapshotBudget *budget = nullptr, bool hashes = true);
void copyNode(const QString &source, const QString &destination);
void removeNode(const QString &path);
void verifySnapshot(const QString &directory, const QJsonObject &manifest);
void verifyAvailableMappingRoots(const Library &lib, const Game &game, const SaveConfig &config);
SaveSnapshot snapshotLocked(Library &lib, const Game &game, const SaveConfig &config, const QString &reason,
                            bool allowEmpty = false);
void finishPendingLocked(Library &lib, const Game &game);
struct SaveLocks {
    QLockFile mutation, launch;
    SaveLocks(const Library &lib, const QString &id, int mutationWaitMs = 0);
};
QJsonObject snapshotManifest(const Library &lib, const QString &id, const QString &snapshotId, bool verifyContents = true);
bool liveMatchesSnapshot(const Library &lib, const Game &game, const SaveConfig &config,
                         const QJsonObject &manifest);
bool hasAncestor(const Library &lib, const QString &gameId, const QString &descendant, const QString &ancestor);
QString stageSuffix(const QString &nonce, const QString &type);
void rollback(const Library &lib, const Game &game, const QJsonObject &journal);
} // namespace novelty::save_detail
