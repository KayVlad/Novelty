#include "lan_saves.h"
#include "devices.h"
#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qpointer.h>
#include <QtCore/qset.h>
#include <QtCore/quuid.h>
#include <stdexcept>

namespace novelty {
namespace {
constexpr qint64 maxTransferBytes = 512LL * 1024 * 1024;
constexpr int maxEntries = 20000;
constexpr qsizetype chunkBytes = 24 * 1024;
bool uuid(const QString &value) { return !QUuid(value).isNull() && !value.contains('/'); }
bool sha256(const QString &value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
bool safeBundlePath(const QString &path) {
    if (path.isEmpty() || path.size() > 512 || path.contains('\\') || path.contains(QChar::Null) ||
        path.startsWith('/') || QDir::cleanPath(path) != path ||
        (path != "manifest.json" && path != "items" && !path.startsWith("items/"))) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}
QByteArray fileDigest(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read verified snapshot file.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto bytes = file.read(1024 * 1024);
        if (bytes.isEmpty() && file.error() != QFile::NoError)
            throw std::runtime_error("Cannot hash verified snapshot file.");
        hash.addData(bytes);
    }
    return hash.result().toHex();
}
void collectEntries(const QString &root, const QString &relative, QList<LanSaveExchange::Entry> &entries,
                    qint64 &total) {
    const auto info = QFileInfo(root + '/' + relative);
    if (info.isSymbolicLink()) throw std::runtime_error("Save snapshot contains a symbolic link.");
    if (info.isDir()) {
        if (!relative.isEmpty()) entries.append({relative, true, 0, {}});
        for (const auto &child : QDir(info.absoluteFilePath()).entryInfoList(
                 QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
            const auto path = relative.isEmpty() ? child.fileName() : relative + '/' + child.fileName();
            if (!safeBundlePath(path)) throw std::runtime_error("Unsafe snapshot transfer path.");
            collectEntries(root, path, entries, total);
        }
    } else if (info.isFile()) {
        if (info.size() < 0 || info.size() > maxTransferBytes - total)
            throw std::runtime_error("Save snapshot exceeds the LAN transfer limit.");
        total += info.size();
        entries.append({relative, false, info.size(), QString::fromLatin1(fileDigest(info.absoluteFilePath()))});
    } else throw std::runtime_error("Save snapshot contains an unsupported node.");
    if (entries.size() > maxEntries) throw std::runtime_error("Save snapshot has too many transfer entries.");
}
}

LanSaveExchange::LanSaveExchange(Library &library, LanConnection &connection, QObject *parent)
    : QObject(parent), library_(library), connection_(connection), transferTimeout_(this) {
    transferTimeout_.setSingleShot(true);
    QObject::connect(&transferTimeout_, &QTimer::timeout, this, [this] {
        clearIncoming();
        if (onError) onError("Peer save transfer stalled. Retry when the device is available.");
        connection_.close();
    });
    QPointer<LanSaveExchange> guard(this);
    connection_.onMessage = [guard](const QJsonObject &message) { if (guard) guard->handle(message); };
    connection_.onDrain = [guard] { if (guard) { guard->pump(); if (guard->onTransportDrain) guard->onTransportDrain(); } };
}
LanSaveExchange::~LanSaveExchange() {
    clearIncoming();
}

void LanSaveExchange::locate(const SaveConfig &config) {
    if (!connection_.ready()) throw std::runtime_error("Connect to a trusted peer before checking saves.");
    QString folderName;
    for (const auto &game : library_.games())
        if (loadSaveConfig(library_, game.id).profileGameId == config.profileGameId) {
            folderName = QFileInfo(game.folder).fileName(); break;
        }
    if (folderName.isEmpty()) throw std::runtime_error("Local game folder is unavailable for peer matching.");
    locateRequest_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connection_.send({{"type", "save-locate"}, {"request_id", locateRequest_},
        {"profile_game_id", config.profileGameId}, {"folder_name", folderName.left(100)},
        {"folder_aliases", QJsonArray::fromStringList(config.folderAliases)},
        {"game_identity", config.gameIdentity}, {"release_identity", config.releaseIdentity},
        {"save_compatibility", config.saveCompatibility}});
}

void LanSaveExchange::fetch(const QString &localGameId, const QString &remoteProfileGameId,
                            const QString &snapshotId) {
    if (!connection_.ready() || incomingActive_ || !uuid(remoteProfileGameId) || !uuid(snapshotId))
        throw std::runtime_error("A save transfer is unavailable or already running.");
    const auto config = loadSaveConfig(library_, localGameId);
    bool linked = config.profileGameId == remoteProfileGameId;
    for (const auto &item : config.peerLinks)
        if (item.fingerprint == connection_.peerFingerprint() && item.profileGameId == remoteProfileGameId)
            linked = true;
    if (!linked) throw std::runtime_error("Confirm the matching peer game before fetching its saves.");
    localGameId_ = localGameId; remoteGameId_ = remoteProfileGameId; requestedSnapshot_ = snapshotId;
    fetchRequest_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connection_.send({{"type", "save-get"}, {"request_id", fetchRequest_},
        {"profile_game_id", remoteGameId_}, {"snapshot_id", snapshotId}});
    transferTimeout_.start(90000);
}

void LanSaveExchange::offer(const QString &gameId, const QString &snapshotId) {
    if (!connection_.ready() || !outgoingOfferId_.isEmpty() || !uuid(snapshotId) ||
        appliedSaveSnapshotId(library_, gameId) != snapshotId)
        throw std::runtime_error("Only the current verified save head can be offered.");
    const auto config = loadSaveConfig(library_, gameId);
    if (config.mappings.isEmpty()) throw std::runtime_error("Choose a save location before offering saves.");
    verifiedSaveSnapshotBundle(library_, gameId, snapshotId);
    outgoingOfferId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    offeredGameId_ = gameId; offeredSnapshot_ = snapshotId;
    connection_.send({{"type", "save-offer"}, {"request_id", outgoingOfferId_},
        {"profile_game_id", config.profileGameId}, {"snapshot_id", snapshotId},
        {"game_identity", config.gameIdentity}, {"release_identity", config.releaseIdentity},
        {"save_compatibility", config.saveCompatibility}});
}

void LanSaveExchange::acknowledgeOffer(const QString &offerId, const QString &snapshotId) {
    if (!connection_.ready() || !uuid(offerId) || !uuid(snapshotId))
        throw std::runtime_error("Invalid save offer acknowledgement.");
    connection_.send({{"type", "save-ack"}, {"request_id", offerId}, {"snapshot_id", snapshotId}});
}

void LanSaveExchange::deferOffer(const QString &offerId, const QString &reason) {
    if (!connection_.ready() || !uuid(offerId)) throw std::runtime_error("Invalid save offer response.");
    connection_.send({{"type", "save-defer"}, {"request_id", offerId}, {"reason", reason.left(160)}});
}

void LanSaveExchange::sendError(const QString &requestId, const QString &message) {
    if (uuid(requestId)) connection_.send({{"type", "save-error"}, {"request_id", requestId},
                                            {"message", message.left(160)}});
}

void LanSaveExchange::answerLocate(const QJsonObject &request) {
    const auto id = request["request_id"].toString();
    const auto remoteGameId = request["profile_game_id"].toString();
    const auto folderName = request["folder_name"].toString();
    const auto gameIdentity = request["game_identity"].toString();
    const auto release = request["release_identity"].toString();
    const auto compatibility = request["save_compatibility"].toString();
    const auto aliases = request["folder_aliases"].toArray();
    if (!uuid(id) || !uuid(remoteGameId) || folderName.isEmpty() || folderName.size() > 100 ||
        folderName.contains('/') || !request["folder_aliases"].isArray() || aliases.size() > 8 ||
        gameIdentity.size() > 256 || release.size() > 256 || compatibility.size() > 256)
        throw std::runtime_error("Invalid save lookup request.");
    QSet<QString> remoteNames{normalizedSaveGameName(folderName)};
    for (const auto &alias : aliases) {
        if (!alias.isString() || alias.toString().size() > 100 || alias.toString().contains('/'))
            throw std::runtime_error("Invalid previous folder name in save lookup.");
        remoteNames.insert(normalizedSaveGameName(alias.toString()));
    }
    QJsonArray matches;
    for (const auto &game : library_.games()) {
        const auto config = loadSaveConfig(library_, game.id);
        bool linked = config.profileGameId == remoteGameId;
        for (const auto &link : config.peerLinks)
            if (link.fingerprint == connection_.peerFingerprint() && link.profileGameId == remoteGameId) linked = true;
        const bool labelsMatch =
            (config.gameIdentity.isEmpty() || gameIdentity.isEmpty() || config.gameIdentity == gameIdentity) &&
            (config.releaseIdentity.isEmpty() || release.isEmpty() || config.releaseIdentity == release) &&
            (config.saveCompatibility.isEmpty() || compatibility.isEmpty() ||
             config.saveCompatibility == compatibility);
        if (config.mappings.isEmpty() || (!linked && !labelsMatch)) continue;
        bool named = remoteNames.contains(normalizedSaveGameName(QFileInfo(game.folder).fileName()));
        for (const auto &alias : config.folderAliases)
            if (remoteNames.contains(normalizedSaveGameName(alias))) named = true;
        if (!linked && !named) continue;
        QJsonArray history;
        for (const auto &snapshotId : saveSnapshotLineage(library_, game.id)) history.append(snapshotId);
        matches.append(QJsonObject{{"profile_game_id", config.profileGameId},
                                   {"snapshot_id", appliedSaveSnapshotId(library_, game.id)},
                                   {"snapshot_ids", history}, {"name", game.name.left(100)},
                                   {"compatibility_issue", !labelsMatch}});
        if (matches.size() >= 8) break;
    }
    connection_.send({{"type", "save-found"}, {"request_id", id}, {"matches", matches}});
}

void LanSaveExchange::answerFetch(const QJsonObject &request) {
    const auto id = request["request_id"].toString();
    const auto profileGameId = request["profile_game_id"].toString();
    const auto snapshotId = request["snapshot_id"].toString();
    if (!uuid(id) || !uuid(profileGameId) || !uuid(snapshotId) || outgoingActive_)
        throw std::runtime_error("Invalid or busy save transfer request.");
    QString gameId;
    for (const auto &game : library_.games()) {
        if (loadSaveConfig(library_, game.id).profileGameId == profileGameId) { gameId = game.id; break; }
    }
    if (gameId.isEmpty()) { sendError(id, "That game is not available here."); return; }
    try {
        outgoingFolder_ = verifiedSaveSnapshotBundle(library_, gameId, snapshotId);
        outgoingEntries_.clear(); outgoingTotal_ = 0; outgoingIndex_ = 0;
        collectEntries(outgoingFolder_, {}, outgoingEntries_, outgoingTotal_);
        outgoingRequest_ = id; outgoingActive_ = true;
        connection_.send({{"type", "save-begin"}, {"request_id", id}, {"snapshot_id", snapshotId},
            {"entries", outgoingEntries_.size()}, {"bytes", static_cast<double>(outgoingTotal_)}});
        pump();
    } catch (const std::exception &e) {
        outgoingActive_ = false; outgoingEntries_.clear(); outgoingFolder_.clear();
        sendError(id, QString::fromUtf8(e.what()));
    }
}

void LanSaveExchange::pump() {
    if (!outgoingActive_ || !connection_.ready()) return;
    try {
        while (connection_.pendingBytes() < 256 * 1024) {
            if (outgoingIndex_ >= outgoingEntries_.size()) {
                connection_.send({{"type", "save-end"}, {"request_id", outgoingRequest_}});
                outgoingActive_ = false; outgoingEntries_.clear(); outgoingFolder_.clear(); return;
            }
            const auto &entry = outgoingEntries_[outgoingIndex_];
            if (entry.directory) {
                connection_.send({{"type", "save-dir"}, {"request_id", outgoingRequest_}, {"path", entry.path}});
                ++outgoingIndex_; continue;
            }
            if (!outgoingFile_.isOpen()) {
                outgoingFile_.setFileName(outgoingFolder_ + '/' + entry.path);
                if (!outgoingFile_.open(QIODevice::ReadOnly)) throw std::runtime_error("Snapshot file disappeared during transfer.");
                connection_.send({{"type", "save-file"}, {"request_id", outgoingRequest_},
                    {"path", entry.path}, {"size", static_cast<double>(entry.size)}, {"sha256", entry.sha256}});
            }
            const auto bytes = outgoingFile_.read(chunkBytes);
            if (bytes.isEmpty() && outgoingFile_.error() != QFile::NoError)
                throw std::runtime_error("Cannot read snapshot during transfer.");
            if (!bytes.isEmpty()) {
                connection_.send({{"type", "save-chunk"}, {"request_id", outgoingRequest_},
                    {"data", QString::fromLatin1(bytes.toBase64())}});
            } else {
                outgoingFile_.close();
                connection_.send({{"type", "save-file-end"}, {"request_id", outgoingRequest_}});
                ++outgoingIndex_;
            }
        }
    } catch (const std::exception &e) {
        outgoingFile_.close(); outgoingActive_ = false; outgoingEntries_.clear();
        sendError(outgoingRequest_, QString::fromUtf8(e.what()));
    }
}

void LanSaveExchange::clearIncoming() {
    transferTimeout_.stop();
    incomingFile_.close(); incomingActive_ = false;
    if (!stage_.isEmpty() && QFileInfo(stage_).isDir()) QDir(stage_).removeRecursively();
    stage_.clear(); incomingPath_.clear();
}

void LanSaveExchange::receiveBegin(const QJsonObject &message) {
    if (incomingActive_ || !uuid(fetchRequest_) || !uuid(requestedSnapshot_) ||
        message["request_id"].toString() != fetchRequest_ ||
        message["snapshot_id"].toString() != requestedSnapshot_) throw std::runtime_error("Unexpected save transfer.");
    const auto entries = message["entries"].toInt(-1);
    const auto bytes = message["bytes"].toDouble(-1);
    if (entries < 2 || entries > maxEntries || bytes < 0 || bytes > maxTransferBytes || bytes != qint64(bytes))
        throw std::runtime_error("Save transfer exceeds its limits.");
    if (QFileInfo(library_.dataRoot() + "/transfers").isSymbolicLink())
        throw std::runtime_error("LAN transfer storage is unsafe.");
    stage_ = library_.dataRoot() + "/transfers/in-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(stage_)) throw std::runtime_error("Cannot stage incoming saves.");
    expectedEntries_ = entries; receivedEntries_ = 0; incomingTotal_ = qint64(bytes); incomingReceived_ = 0;
    incomingActive_ = true;
    transferTimeout_.start(90000);
}

void LanSaveExchange::receiveEntry(const QJsonObject &message) {
    if (!incomingActive_ || incomingFile_.isOpen() || receivedEntries_ >= expectedEntries_)
        throw std::runtime_error("Unexpected save transfer entry.");
    const auto path = message["path"].toString();
    if (!safeBundlePath(path)) throw std::runtime_error("Unsafe save transfer path.");
    const auto target = stage_ + '/' + path;
    if (QFileInfo(target).exists() || QFileInfo(target).isSymbolicLink())
        throw std::runtime_error("Duplicate save transfer path.");
    if (message["type"] == "save-dir") {
        if (path == "manifest.json" || !QDir().mkpath(target)) throw std::runtime_error("Cannot create save transfer directory.");
        ++receivedEntries_; return;
    }
    const auto size = message["size"].toDouble(-1);
    const auto hash = message["sha256"].toString();
    if (size < 0 || size > maxTransferBytes - incomingReceived_ || size != qint64(size) || !sha256(hash))
        throw std::runtime_error("Invalid save transfer file.");
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) throw std::runtime_error("Cannot stage save file.");
    incomingFile_.setFileName(target);
    if (!incomingFile_.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create staged save file.");
    incomingPath_ = hash; incomingFileSize_ = qint64(size); incomingFileReceived_ = 0; incomingHash_.reset();
}

void LanSaveExchange::receiveChunk(const QJsonObject &message) {
    if (!incomingActive_ || !incomingFile_.isOpen()) throw std::runtime_error("Unexpected save data.");
    const auto encoded = message["data"].toString().toLatin1();
    const auto bytes = QByteArray::fromBase64(encoded);
    if (bytes.isEmpty() || bytes.size() > chunkBytes || bytes.toBase64() != encoded ||
        bytes.size() > incomingFileSize_ - incomingFileReceived_ ||
        bytes.size() > incomingTotal_ - incomingReceived_)
        throw std::runtime_error("Invalid save data chunk.");
    if (incomingFile_.write(bytes) != bytes.size()) throw std::runtime_error("Cannot write staged save data.");
    incomingHash_.addData(bytes); incomingFileReceived_ += bytes.size(); incomingReceived_ += bytes.size();
    if (onProgress) onProgress(incomingReceived_, incomingTotal_);
}

void LanSaveExchange::receiveFileEnd(const QJsonObject &) {
    if (!incomingActive_ || !incomingFile_.isOpen() || incomingFileReceived_ != incomingFileSize_ ||
        QString::fromLatin1(incomingHash_.result().toHex()) != incomingPath_)
        throw std::runtime_error("Transferred save file failed verification.");
    incomingFile_.close(); incomingPath_.clear(); ++receivedEntries_;
}

void LanSaveExchange::receiveEnd(const QJsonObject &) {
    if (!incomingActive_ || incomingFile_.isOpen() || receivedEntries_ != expectedEntries_ ||
        incomingReceived_ != incomingTotal_) throw std::runtime_error("Incomplete save transfer.");
    const auto result = importSaveSnapshot(library_, localGameId_, stage_, connection_.peerFingerprint());
    clearIncoming(); fetchRequest_.clear();
    if (onImported) onImported(result);
}

void LanSaveExchange::handle(const QJsonObject &message) {
    const auto type = message["type"].toString();
    const auto id = message["request_id"].toString();
    try {
        if (type == "save-offer") {
            if (!uuid(id) || !uuid(message["profile_game_id"].toString()) ||
                !uuid(message["snapshot_id"].toString()) ||
                message["game_identity"].toString().size() > 256 ||
                message["release_identity"].toString().size() > 256 ||
                message["save_compatibility"].toString().size() > 256)
                throw std::runtime_error("Malformed peer save offer.");
            if (onOffer) onOffer(message);
            else deferOffer(id, "This device cannot accept saves right now.");
            return;
        }
        if (type == "save-ack" || type == "save-defer") {
            if (id != outgoingOfferId_ || !uuid(id)) throw std::runtime_error("Unexpected save offer response.");
            if (type == "save-ack") {
                if (message["snapshot_id"].toString() != offeredSnapshot_)
                    throw std::runtime_error("Peer acknowledged a different save snapshot.");
                const auto gameId = offeredGameId_, snapshotId = offeredSnapshot_;
                outgoingOfferId_.clear(); offeredGameId_.clear(); offeredSnapshot_.clear();
                if (onAcknowledged) onAcknowledged(gameId, snapshotId);
            } else {
                outgoingOfferId_.clear(); offeredGameId_.clear(); offeredSnapshot_.clear();
                if (onDeferred) onDeferred(message["reason"].toString().left(160));
            }
            return;
        }
        if (type == "save-locate") { answerLocate(message); return; }
        if (type == "save-get") { answerFetch(message); return; }
        if (type == "save-found") {
            if (id != locateRequest_ || !message["matches"].isArray() || message["matches"].toArray().size() > 8)
                throw std::runtime_error("Unexpected peer save catalogue.");
            locateRequest_.clear(); if (onMatches) onMatches(message["matches"].toArray()); return;
        }
        if (type == "save-error") {
            if (id != locateRequest_ && id != fetchRequest_) throw std::runtime_error("Unexpected peer save error.");
            if (id == fetchRequest_) clearIncoming();
            locateRequest_.clear(); fetchRequest_.clear();
            if (onError) onError(message["message"].toString().left(160));
            return;
        }
        if (type == "save-begin") { receiveBegin(message); return; }
        if (type.startsWith("save-")) {
            if (!incomingActive_ || id != fetchRequest_) throw std::runtime_error("Unrequested save transfer data.");
            transferTimeout_.start(90000);
            if (type == "save-dir" || type == "save-file") receiveEntry(message);
            else if (type == "save-chunk") receiveChunk(message);
            else if (type == "save-file-end") receiveFileEnd(message);
            else if (type == "save-end") receiveEnd(message);
            else throw std::runtime_error("Unknown save transfer message.");
            return;
        }
        if (onOtherMessage) onOtherMessage(message);
        else throw std::runtime_error("Unknown peer message.");
    } catch (const std::exception &e) {
        clearIncoming();
        if (onError) onError(QString::fromUtf8(e.what()));
        connection_.close();
    }
}

} // namespace novelty
