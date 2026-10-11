#pragma once

#include "core.h"
#include <QtCore/qjsonarray.h>

namespace novelty {

struct SaveMapping {
    QString scope; // "game", "appdata", "documents" or advanced "prefix"
    QString path;  // Relative to that scope; never the entire prefix or game folder.
};
struct PeerSaveGameLink {
    QString fingerprint;
    QString profileGameId;
};

struct SaveConfig {
    QString profileId;            // Local Games-library profile identity.
    QString profileGameId;        // Stable across this profile's installations and devices.
    QString gameIdentity;          // Shared title identity, for example a confirmed VNDB ID.
    QString releaseIdentity;       // Exact release/build identity, entered when known.
    QString saveCompatibility;     // Explicit compatibility group; equality is required for sync.
    QString discoveryState = "searching"; // searching, identified or explicitly unidentified.
    QString discoverySource;       // manual, recipe, engine or observed.
    QJsonArray candidates;         // Unconfirmed portable locations and evidence.
    QStringList folderAliases;     // Previous local folder names for first-time peer matching after a rename.
    QList<SaveMapping> mappings;
    QList<PeerSaveGameLink> peerLinks; // Explicitly confirmed existing-install matches, scoped to a trusted peer.
};

struct SaveSnapshot {
    QString id;
    QString created;
    QString reason;
    int files = 0;
    qint64 bytes = 0;
};
struct SaveImportResult { bool added = false; QString relation; QString snapshotId; };
struct ImportedSaveCandidate { QString snapshotId; QString created; QString relation; };

SaveConfig loadSaveConfig(const Library &, const QString &gameId);
// Plausible folders for an unmapped game; a choice is needed when more than one exists.
QList<SaveMapping> saveDiscoveryCandidates(const Library &, const QString &gameId);
QList<SaveMapping> suggestedSaveMappings(const Library &, const QString &gameId);
void saveSaveConfig(Library &, const QString &gameId, const SaveConfig &);
// Validate and normalize without writing. The caller holds the library and game locks when committing.
QJsonObject validatedSaveConfig(const Library &, const Game &, const SaveConfig &);
void setSaveDiscoveryState(Library &, const QString &gameId, const QString &state);
SaveSnapshot createSaveSnapshot(Library &, const QString &gameId, const QString &reason = "manual");
// Capture populated mapped roots before a peer game transfer. Empty roots are valid.
bool captureAvailableSaveSnapshot(Library &, const QString &gameId, const QString &reason);
// Game-local candidates that still need a choice before transferring game files.
QStringList unresolvedGameSaveLocations(const Library &, const QString &gameId);
QList<SaveSnapshot> listSaveSnapshots(const Library &, const QString &gameId);
QString appliedSaveSnapshotId(const Library &, const QString &gameId);
QString verifiedSaveSnapshotBundle(const Library &, const QString &gameId, const QString &snapshotId);
QStringList saveSnapshotLineage(const Library &, const QString &gameId, int maxSnapshots = 64);
QList<ImportedSaveCandidate> importedSaveCandidates(const Library &, const QString &gameId);
void restoreSaveSnapshot(Library &, const QString &gameId, const QString &snapshotId);
SaveImportResult importSaveSnapshot(Library &, const QString &gameId, const QString &bundleDirectory,
                                    const QString &authenticatedPeer = {});
void linkPeerSaveGame(Library &, const QString &gameId, const QString &peerFingerprint,
                      const QString &peerProfileGameId);
QString normalizedSaveGameName(const QString &name);
QStringList recoverSaveRestores(Library &);
QString saveRestoreIssue(const Library &, const QString &gameId);
// Called by the detached supervisor while it holds launch.lock.
void queuePostExitSaveSnapshot(const Library &, const Game &);
void finishPostExitSaveSnapshot(Library &, const Game &);
QStringList recoverPendingSaveSnapshots(Library &);
QString pendingSaveSnapshotIssue(const Library &, const QString &gameId);
QString saveRecipePath(const Library &, const SaveConfig &);
QString saveMappingRoot(const Library &, const Game &, const QString &scope);
SaveMapping saveMappingFromPath(const Library &, const Game &, const QString &path);
void beginSaveObservation(Library &, const Game &); // Called with launch.lock held.
void finishSaveObservation(Library &, const Game &); // Called after the real game process exits.
QStringList recoverSaveObservations(Library &);

} // namespace novelty
