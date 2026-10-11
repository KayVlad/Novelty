#include "lan_games.h"
#include "saves.h"
#include <QtCore/qdir.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qpointer.h>
#include <QtCore/qset.h>
#include <QtCore/quuid.h>
#include <algorithm>
#include <stdexcept>

namespace novelty {
namespace {
constexpr qint64 maxGameBytes = 512LL * 1024 * 1024 * 1024;
constexpr int maxGameEntries = 100000;
constexpr qsizetype chunkBytes = 24 * 1024;
bool uuid(const QString &id) { return !QUuid(id).isNull() && !id.contains('/'); }
bool safePath(const QString &path) {
    if (path.isEmpty() || path.size() > 1024 || path.startsWith('/') ||
        path.contains('\\') || path.contains(QChar::Null) || QDir::cleanPath(path) != path ||
        path == ".novelty-transfer.json" || path.endsWith(".novelty-part")) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}
bool safeName(const QString &name) {
    if (name.isEmpty() || name.size() > 100 || name.startsWith('.')) return false;
    for (const auto c : name) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return name != ".novelty" && !name.contains('/') && !name.contains('\\') &&
        !name.contains(QChar::Null) && name.trimmed() == name;
}
bool digest(const QString &hash) {
    if (hash.size() != 64) return false;
    for (const auto c : hash)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
QString relativePath(const QString &root, const QString &path) {
    if (path.isEmpty()) return {};
    const auto relative = QDir(root).relativeFilePath(path);
    return safePath(relative) && !relative.startsWith("../") ? relative : QString();
}
QByteArray fileHash(const QString &path, std::stop_token stop = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read game file for transfer.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (stop.stop_requested()) throw std::runtime_error("Game manifest scan was cancelled.");
        const auto bytes = file.read(1024 * 1024);
        if (bytes.isEmpty() && file.error() != QFile::NoError)
            throw std::runtime_error("Cannot hash game file for transfer.");
        hash.addData(bytes);
    }
    return hash.result().toHex();
}
void noLinks(const QString &path, const QString &root) {
    auto current = root;
    if (QFileInfo(current).isSymbolicLink()) throw std::runtime_error("LAN staging root is a symbolic link.");
    const auto relative = QDir(root).relativeFilePath(path);
    if (!safePath(relative)) throw std::runtime_error("Unsafe LAN game path.");
    for (const auto &part : relative.split('/')) {
        current += '/' + part;
        if (QFileInfo(current).isSymbolicLink())
            throw std::runtime_error("LAN game path contains a symbolic link.");
    }
}
void collect(const QString &folder, const QSet<QString> &excluded,
             QList<LanGameExchange::Entry> &entries, qint64 &bytes, std::stop_token stop) {
    if (QFileInfo(folder).isSymbolicLink() || !QFileInfo(folder).isDir())
        throw std::runtime_error("Only a real game folder can be transferred.");
    const auto walk = [&](auto &&self, const QString &directory) -> void {
        for (const auto &info : QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden |
                                                            QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
            if (stop.stop_requested()) throw std::runtime_error("Game manifest scan was cancelled.");
            const auto relative = QDir(folder).relativeFilePath(info.absoluteFilePath());
            if (!safePath(relative) || info.isSymbolicLink() || (!info.isDir() && !info.isFile()))
                throw std::runtime_error("Game folder contains an unsupported or unsafe path.");
            if (excluded.contains(relative)) continue;
            if (info.isDir()) {
                entries.append({relative, true, 0, {}});
                self(self, info.absoluteFilePath());
            } else {
                if (info.size() < 0 || info.size() > maxGameBytes - bytes)
                    throw std::runtime_error("Game exceeds the LAN transfer limit.");
                bytes += info.size();
                entries.append({relative, false, info.size(), QString::fromLatin1(fileHash(info.absoluteFilePath(), stop))});
            }
            if (entries.size() > maxGameEntries)
                throw std::runtime_error("Game contains too many files for LAN transfer.");
        }
    };
    walk(walk, folder);
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
}
QSet<QString> transferExclusions(const SaveConfig &config) {
    QSet<QString> excluded;
    if (config.discoveryState != "identified") return excluded;
    for (const auto &mapping : config.mappings)
        if (mapping.scope == "game") excluded.insert(mapping.path);
    return excluded;
}
QJsonObject descriptor(const Library &lib, const Game &game) {
    const auto config = loadSaveConfig(lib, game.id);
    QJsonArray mappings;
    for (const auto &item : config.mappings)
        mappings.append(QJsonObject{{"scope", item.scope}, {"path", item.path}});
    const auto metadata = QJsonDocument(game.metadata).toJson(QJsonDocument::Compact).size() <= 16 * 1024
        ? game.metadata : QJsonObject{};
    return {{"profile_game_id", config.profileGameId}, {"name", game.name.left(100)},
            {"engine", game.engine.left(60)}, {"executable", relativePath(game.folder, game.executable)},
            {"cwd", relativePath(game.folder, game.workingDirectory)},
            {"locale", game.locale.left(80)}, {"game_identity", config.gameIdentity},
            {"release_identity", config.releaseIdentity},
            {"save_compatibility", config.saveCompatibility}, {"mappings", mappings},
            {"arguments", QJsonArray::fromStringList(game.arguments)}, {"metadata", metadata}};
}
QJsonObject summary(const QJsonObject &full) {
    return {{"profile_game_id", full["profile_game_id"]}, {"name", full["name"]},
            {"engine", full["engine"]}, {"game_identity", full["game_identity"]},
            {"release_identity", full["release_identity"]},
            {"save_compatibility", full["save_compatibility"]}};
}
bool validSummary(const QJsonObject &item) {
    if (!uuid(item["profile_game_id"].toString()) || !safeName(item["name"].toString()) ||
        item["engine"].toString().size() > 60) return false;
    for (const auto &key : {"game_identity", "release_identity", "save_compatibility"})
        if (item[key].toString().size() > 256) return false;
    return true;
}
bool validDescriptor(const QJsonObject &item) {
    if (!validSummary(item) ||
        item["engine"].toString().size() > 60 || item["locale"].toString().size() > 80 ||
        !item["mappings"].isArray() || item["mappings"].toArray().size() > 32 ||
        !item["arguments"].isArray() || item["arguments"].toArray().size() > 64 ||
        !item["metadata"].isObject() ||
        QJsonDocument(item["metadata"].toObject()).toJson(QJsonDocument::Compact).size() > 16 * 1024 ||
        QJsonDocument(item).toJson(QJsonDocument::Compact).size() > 48 * 1024) return false;
    int argumentBytes = 0;
    for (const auto &value : item["arguments"].toArray()) {
        if (!value.isString() || value.toString().contains(QChar::Null)) return false;
        argumentBytes += value.toString().toUtf8().size() + 1;
        if (argumentBytes > 64 * 1024) return false;
    }
    if (item["executable"].toString().isEmpty()) return false;
    for (const auto &key : {"executable", "cwd"})
        if (!item[key].toString().isEmpty() && !safePath(item[key].toString())) return false;
    for (const auto &key : {"game_identity", "release_identity", "save_compatibility"})
        if (item[key].toString().size() > 256) return false;
    QList<QPair<QString, QString>> mappings;
    for (const auto &value : item["mappings"].toArray()) {
        const auto mapping = value.toObject();
        const auto scope = mapping["scope"].toString();
        const auto path = mapping["path"].toString();
        if (!QStringList{"game", "prefix", "appdata", "documents"}.contains(scope) ||
            path.size() > 512 || !safePath(path))
            return false;
        for (const auto &[otherScope, otherPath] : mappings)
            if (scope == otherScope && (path == otherPath || path.startsWith(otherPath + '/') ||
                                        otherPath.startsWith(path + '/'))) return false;
        mappings.append({scope, path});
    }
    return true;
}
}

QPair<QList<LanGameExchange::Entry>, qint64> previewLanGameManifest(const Library &lib, const QString &gameId) {
    const auto game = lib.game(gameId);
    const auto excluded = transferExclusions(loadSaveConfig(lib, gameId));
    QList<LanGameExchange::Entry> entries;
    qint64 bytes = 0;
    collect(game.folder, excluded, entries, bytes, {});
    return {entries, bytes};
}

void validateLanGameManifest(const QJsonObject &item,
                             const QList<LanGameExchange::Entry> &entries, qint64 totalBytes) {
    if (!validDescriptor(item) || entries.isEmpty() || entries.size() > maxGameEntries ||
        totalBytes < 0 || totalBytes > maxGameBytes)
        throw std::runtime_error("Invalid paired-device game manifest.");
    QHash<QString, bool> paths;
    qint64 bytes = 0;
    for (const auto &entry : entries) {
        if (!safePath(entry.path) || paths.contains(entry.path) || entry.size < 0 ||
            (entry.directory && (entry.size != 0 || !entry.sha256.isEmpty())) ||
            (!entry.directory && (!digest(entry.sha256) || entry.size > maxGameBytes - bytes)))
            throw std::runtime_error("Invalid game manifest entry.");
        paths.insert(entry.path, entry.directory);
        if (!entry.directory) bytes += entry.size;
    }
    for (const auto &entry : entries)
        for (auto parent = QFileInfo(entry.path).path(); parent != "."; parent = QFileInfo(parent).path())
            if (!paths.contains(parent) || !paths.value(parent))
                throw std::runtime_error("Game manifest omits a parent directory.");
    if (bytes != totalBytes) throw std::runtime_error("Game manifest byte count differs.");
    const auto executable = item["executable"].toString();
    const auto cwd = item["cwd"].toString();
    if (!paths.contains(executable) || paths.value(executable) ||
        (!cwd.isEmpty() && (!paths.contains(cwd) || !paths.value(cwd))))
        throw std::runtime_error("Game manifest does not contain its executable or working directory.");
}

LanGameExchange::LanGameExchange(Library &library, LanConnection &connection,
                                 LanSaveExchange &router, QObject *parent)
    : QObject(parent), library_(library), connection_(connection), router_(router), timeout_(this), manifestPoll_(this), resumePoll_(this) {
    timeout_.setSingleShot(true);
    QObject::connect(&timeout_, &QTimer::timeout, this, [this] {
        fail("LAN game transfer stalled. Reconnect to retry completed files.");
    });
    QPointer<LanGameExchange> guard(this);
    router_.onOtherMessage = [guard](const QJsonObject &message) {
        if (guard) guard->handle(message);
    };
    router_.onTransportDrain = [guard] { if (guard) guard->pump(); };
    manifestPoll_.setInterval(50);
    QObject::connect(&manifestPoll_, &QTimer::timeout, this, [this] {
        if (!manifestFuture_.valid()) return;
        if (manifestFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastManifestHeartbeat_ >= 10000) {
                lastManifestHeartbeat_ = now;
                try { connection_.send({{"type", "game-manifest-progress"}, {"request_id", sendingRequest_}}); }
                catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
            }
            return;
        }
        manifestPoll_.stop();
        try {
            auto result = manifestFuture_.get();
            if (manifestWorker_.joinable()) manifestWorker_.join();
            sendingEntries_ = std::move(result.first); sendingBytes_ = result.second;
            if (sendingEntries_.isEmpty()) throw std::runtime_error("Game folder has no transferable files.");
            sendingMap_.clear();
            for (const auto &entry : sendingEntries_) sendingMap_.insert(entry.path, entry);
            sendingIndex_ = 0;
            connection_.send({{"type", "game-manifest-begin"}, {"request_id", sendingRequest_},
                {"profile_game_id", sendingDescriptor_["profile_game_id"].toString()},
                {"descriptor", sendingDescriptor_}, {"entries", sendingEntries_.size()},
                {"bytes", static_cast<double>(sendingBytes_)}});
            sendingManifest_ = true; pump();
        } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
    });
    resumePoll_.setInterval(50);
    QObject::connect(&resumePoll_, &QTimer::timeout, this, [this] {
        if (!resumeFuture_.valid() || resumeFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        resumePoll_.stop();
        try {
            verifiedStagedFiles_ = resumeFuture_.get();
            if (resumeWorker_.joinable()) resumeWorker_.join();
            requestNext();
        } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
    });
}

LanGameExchange::~LanGameExchange() {
    manifestPoll_.stop(); resumePoll_.stop();
    if (manifestWorker_.joinable()) { manifestWorker_.request_stop(); manifestWorker_.join(); }
    if (resumeWorker_.joinable()) { resumeWorker_.request_stop(); resumeWorker_.join(); }
}

void LanGameExchange::catalogue() {
    if (!connection_.ready() || !requestId_.isEmpty())
        throw std::runtime_error("Connect to a trusted peer before listing games.");
    requestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connection_.send({{"type", "game-list"}, {"request_id", requestId_}});
    timeout_.start(30000);
}

void LanGameExchange::install(const QJsonObject &selected) {
    if (!connection_.ready() || !requestId_.isEmpty() || !validSummary(selected))
        throw std::runtime_error("Choose a valid paired-device game to install.");
    for (const auto &local : library_.games()) {
        const auto config = loadSaveConfig(library_, local.id);
        if (config.profileGameId == selected["profile_game_id"].toString())
            throw std::runtime_error("This paired-device game is already installed locally.");
        if (!config.gameIdentity.isEmpty() &&
            config.gameIdentity == selected["game_identity"].toString() &&
            config.releaseIdentity != selected["release_identity"].toString())
            throw std::runtime_error("This title has a different local build. Review save compatibility before installing another copy.");
    }
    descriptor_ = selected;
    remoteGameId_ = selected["profile_game_id"].toString();
    requestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connection_.send({{"type", "game-manifest"}, {"request_id", requestId_},
                      {"profile_game_id", remoteGameId_}});
    timeout_.start(120000);
}

void LanGameExchange::sendCatalogue(const QJsonObject &request) {
    const auto id = request["request_id"].toString();
    if (!uuid(id)) throw std::runtime_error("Invalid LAN game catalogue request.");
    QJsonArray games;
    for (const auto &game : library_.games()) {
        if (game.backend != "umu" || game.installPending || library_.busy(game.id) ||
            !QFileInfo(game.folder).isDir() || QFileInfo(game.folder).isSymbolicLink() ||
            folderIdentity(game.folder) != game.folderIdentity ||
            QFileInfo(game.folder).absolutePath() != library_.root()) continue;
        try {
            const auto full = descriptor(library_, game);
            if (validDescriptor(full)) {
                const auto item = summary(full);
                auto next = games; next.append(item);
                if (QJsonDocument(QJsonObject{{"type", "game-found"}, {"request_id", id},
                                              {"games", next}}).toJson(QJsonDocument::Compact).size() > 60 * 1024)
                    break;
                games = next;
            }
        } catch (const std::exception &) { /* One damaged game does not hide healthy games. */ }
        if (games.size() >= 64) break;
    }
    connection_.send({{"type", "game-found"}, {"request_id", id}, {"games", games}});
}

void LanGameExchange::sendManifest(const QJsonObject &request) {
    const auto id = request["request_id"].toString();
    const auto profileGameId = request["profile_game_id"].toString();
    if (!uuid(id) || !uuid(profileGameId) || sendingManifest_ || sendingFileActive_ ||
        manifestPoll_.isActive())
        throw std::runtime_error("Invalid or busy LAN game request.");
    Game chosen;
    for (const auto &game : library_.games())
        if (loadSaveConfig(library_, game.id).profileGameId == profileGameId) {
            if (!chosen.id.isEmpty()) throw std::runtime_error("Peer game identity is duplicated.");
            chosen = game;
        }
    if (chosen.id.isEmpty() || chosen.backend != "umu" || chosen.installPending ||
        library_.busy(chosen.id) || QFileInfo(chosen.folder).absolutePath() != library_.root() ||
        folderIdentity(chosen.folder) != chosen.folderIdentity)
        throw std::runtime_error("That game is unavailable for LAN installation.");
    beginSaveObservation(library_, chosen);
    const auto undecided = unresolvedGameSaveLocations(library_, chosen.id);
    if (!undecided.isEmpty()) {
        connection_.send({{"type", "game-error"}, {"request_id", id},
            {"message", ("Choose the save location on the source device before transfer: " +
                         undecided.join(", ")).left(180)}});
        return;
    }
    if (manifestWorker_.joinable()) manifestWorker_.join();
    sendingFolder_ = chosen.folder; sendingRequest_ = id;
    sendingDescriptor_ = descriptor(library_, chosen);
    const auto excluded = transferExclusions(loadSaveConfig(library_, chosen.id));
    if (!validDescriptor(sendingDescriptor_))
        throw std::runtime_error("This game's portable settings exceed the LAN transfer limit.");
    lastManifestHeartbeat_ = QDateTime::currentMSecsSinceEpoch();
    std::promise<QPair<QList<Entry>, qint64>> promise;
    manifestFuture_ = promise.get_future();
    manifestWorker_ = std::jthread([folder = chosen.folder, excluded, root = library_.root(),
                                    gameId = chosen.id, promise = std::move(promise)]
                                   (std::stop_token stop) mutable {
        try {
            if (stop.stop_requested()) throw std::runtime_error("Game manifest scan was cancelled.");
            Library worker(root);
            captureAvailableSaveSnapshot(worker, gameId, "before game transfer");
            if (stop.stop_requested()) throw std::runtime_error("Game manifest scan was cancelled.");
            QList<Entry> entries; qint64 bytes = 0;
            collect(folder, excluded, entries, bytes, stop);
            promise.set_value({std::move(entries), bytes});
        } catch (...) { promise.set_exception(std::current_exception()); }
    });
    manifestPoll_.start();
}

void LanGameExchange::sendFile(const QJsonObject &request) {
    const auto id = request["request_id"].toString();
    const auto path = request["path"].toString();
    if (id != sendingRequest_ || !uuid(id) || !safePath(path) || sendingManifest_ ||
        sendingFileActive_ || sendingFolder_.isEmpty())
        throw std::runtime_error("Unexpected LAN game file request.");
    const auto found = sendingMap_.constFind(path);
    if (found == sendingMap_.cend() || found->directory)
        throw std::runtime_error("Requested game file is not in the manifest.");
    noLinks(sendingFolder_ + '/' + path, sendingFolder_);
    sendingFile_.setFileName(sendingFolder_ + '/' + path);
    if (!sendingFile_.open(QIODevice::ReadOnly) || sendingFile_.size() != found->size)
        throw std::runtime_error("Game file changed during transfer.");
    sendingPath_ = path; sendingCurrentBytes_ = 0; sendingHash_.reset(); sendingFileActive_ = true;
    connection_.send({{"type", "game-file-begin"}, {"request_id", id},
        {"path", path}, {"size", static_cast<double>(found->size)}, {"sha256", found->sha256}});
    pump();
}

void LanGameExchange::pump() {
    if (!connection_.ready()) return;
    try {
        while (sendingManifest_ && connection_.pendingBytes() < 256 * 1024) {
            if (sendingIndex_ == sendingEntries_.size()) {
                connection_.send({{"type", "game-manifest-end"}, {"request_id", sendingRequest_}});
                sendingManifest_ = false; break;
            }
            const auto &entry = sendingEntries_[sendingIndex_++];
            connection_.send({{"type", "game-entry"}, {"request_id", sendingRequest_},
                {"path", entry.path}, {"directory", entry.directory},
                {"size", static_cast<double>(entry.size)}, {"sha256", entry.sha256}});
        }
        while (sendingFileActive_ && connection_.pendingBytes() < 256 * 1024) {
            const auto bytes = sendingFile_.read(chunkBytes);
            if (!bytes.isEmpty()) {
                sendingCurrentBytes_ += bytes.size(); sendingHash_.addData(bytes);
                connection_.send({{"type", "game-chunk"}, {"request_id", sendingRequest_},
                    {"data", QString::fromLatin1(bytes.toBase64())}});
            } else {
                if (sendingFile_.error() != QFile::NoError)
                    throw std::runtime_error("Cannot read game file during transfer.");
                sendingFile_.close();
                const auto found = sendingMap_.constFind(sendingPath_);
                if (found == sendingMap_.cend() || sendingCurrentBytes_ != found->size ||
                    QString::fromLatin1(sendingHash_.result().toHex()) != found->sha256)
                    throw std::runtime_error("Game file changed during transfer.");
                connection_.send({{"type", "game-file-end"}, {"request_id", sendingRequest_},
                                  {"path", sendingPath_}});
                sendingPath_.clear(); sendingFileActive_ = false;
            }
        }
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

void LanGameExchange::receiveBegin(const QJsonObject &message) {
    const auto incoming = message["descriptor"].toObject();
    const auto count = message["entries"].toInt(-1);
    const auto bytes = message["bytes"].toDouble(-1);
    if (message["request_id"].toString() != requestId_ ||
        message["profile_game_id"].toString() != remoteGameId_ ||
        !validDescriptor(incoming) || summary(incoming) != descriptor_ ||
        count < 1 || count > maxGameEntries || bytes < 0 || bytes > maxGameBytes ||
        bytes != qint64(bytes) || receivingManifest_)
        throw std::runtime_error("Invalid paired-device game manifest.");
    if (QFileInfo(library_.root()).isSymbolicLink())
        throw std::runtime_error("Games root is a symbolic link.");
    descriptor_ = incoming;
    stage_ = library_.root() + "/.novelty-incoming-" + connection_.peerFingerprint().left(16) + '-' + remoteGameId_;
    if (QFileInfo(stage_).isSymbolicLink()) throw std::runtime_error("LAN game staging path is unsafe.");
    const auto marker = stage_ + "/.novelty-transfer.json";
    const QJsonObject identity{{"schema", 2}, {"peer", connection_.peerFingerprint()},
        {"profile_game_id", remoteGameId_},
        {"game_identity", descriptor_["game_identity"]},
        {"release_identity", descriptor_["release_identity"]},
        {"save_compatibility", descriptor_["save_compatibility"]}};
    if (QFileInfo(stage_).exists()) {
        if (!QFileInfo(stage_).isDir() || QFileInfo(marker).isSymbolicLink() || readJson(marker) != identity)
            throw std::runtime_error("An incomplete game folder needs review before retrying this transfer.");
    } else {
        if (!QDir().mkdir(stage_)) throw std::runtime_error("Cannot stage the incoming game inside Games.");
        writeJson(marker, identity);
    }
    entries_.clear(); receivedPaths_.clear(); expectedEntries_ = count; receivedEntries_ = 0; totalBytes_ = qint64(bytes);
    completeBytes_ = 0; nextEntry_ = 0; receivingManifest_ = true;
    timeout_.start(120000);
}

void LanGameExchange::receiveEntry(const QJsonObject &message) {
    if (!receivingManifest_ || message["request_id"].toString() != requestId_ ||
        receivedEntries_ >= expectedEntries_)
        throw std::runtime_error("Unexpected game manifest entry.");
    const auto rawSize = message["size"].toDouble(-1);
    if (rawSize < 0 || rawSize > maxGameBytes || rawSize != qint64(rawSize))
        throw std::runtime_error("Invalid game manifest entry size.");
    Entry entry{message["path"].toString(), message["directory"].toBool(),
                qint64(rawSize), message["sha256"].toString()};
    if (!safePath(entry.path) ||
        (entry.directory && (entry.size != 0 || !entry.sha256.isEmpty())) ||
        (!entry.directory && (!digest(entry.sha256) || entry.size > maxGameBytes)))
        throw std::runtime_error("Invalid game manifest entry.");
    if (receivedPaths_.contains(entry.path)) throw std::runtime_error("Duplicate game manifest path.");
    receivedPaths_.insert(entry.path);
    entries_.append(entry); ++receivedEntries_;
    timeout_.start(120000);
}

void LanGameExchange::receiveManifestEnd(const QJsonObject &message) {
    if (!receivingManifest_ || message["request_id"].toString() != requestId_ ||
        receivedEntries_ != expectedEntries_) throw std::runtime_error("Incomplete game manifest.");
    validateLanGameManifest(descriptor_, entries_, totalBytes_);
    receivingManifest_ = false;
    timeout_.stop();
    if (resumeWorker_.joinable()) resumeWorker_.join();
    const auto entries = entries_;
    const auto stage = stage_;
    std::promise<QSet<QString>> promise;
    resumeFuture_ = promise.get_future();
    resumeWorker_ = std::jthread([entries, stage, promise = std::move(promise)](std::stop_token stop) mutable {
        try {
            QSet<QString> verified;
            for (const auto &entry : entries) {
                if (stop.stop_requested()) throw std::runtime_error("Staged game verification was cancelled.");
                if (entry.directory) continue;
                const auto target = stage + '/' + entry.path;
                noLinks(target, stage);
                const QFileInfo info(target);
                if (!info.exists()) continue;
                if (!info.isFile() || info.size() != entry.size ||
                    QString::fromLatin1(fileHash(target, stop)) != entry.sha256)
                    throw std::runtime_error("A staged game file changed. Review the incomplete transfer folder.");
                verified.insert(entry.path);
            }
            promise.set_value(std::move(verified));
        } catch (...) { promise.set_exception(std::current_exception()); }
    });
    resumePoll_.start();
}

void LanGameExchange::requestNext() {
    while (nextEntry_ < entries_.size()) {
        const auto &entry = entries_[nextEntry_];
        const auto target = stage_ + '/' + entry.path;
        noLinks(target, stage_);
        if (entry.directory) {
            if (!QDir().mkpath(target)) throw std::runtime_error("Cannot create staged game directory.");
            ++nextEntry_; continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            throw std::runtime_error("Cannot create staged game parent directory.");
        if (verifiedStagedFiles_.contains(entry.path)) {
            completeBytes_ += entry.size;
            ++nextEntry_;
            if (onProgress) onProgress(completeBytes_, totalBytes_);
            continue;
        }
        connection_.send({{"type", "game-file-get"}, {"request_id", requestId_},
                          {"path", entry.path}});
        timeout_.start(120000);
        return;
    }
    finalize();
}

void LanGameExchange::receiveFileBegin(const QJsonObject &message) {
    if (nextEntry_ >= entries_.size() || currentFile_.isOpen() ||
        message["request_id"].toString() != requestId_)
        throw std::runtime_error("Unexpected paired-device game file.");
    const auto &entry = entries_[nextEntry_];
    if (entry.directory || message["path"].toString() != entry.path ||
        message["size"].toDouble(-1) != entry.size ||
        message["sha256"].toString() != entry.sha256)
        throw std::runtime_error("Game file does not match the verified manifest.");
    const auto path = stage_ + '/' + entry.path + ".novelty-part";
    noLinks(path, stage_);
    if (QFileInfo(path).exists() && (!QFileInfo(path).isFile() || !QFile::remove(path)))
        throw std::runtime_error("Cannot replace incomplete staged game data.");
    currentFile_.setFileName(path);
    if (!currentFile_.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create staged game file.");
    currentPath_ = entry.path; currentBytes_ = 0; currentHash_.reset();
    timeout_.start(120000);
}

void LanGameExchange::receiveChunk(const QJsonObject &message) {
    if (!currentFile_.isOpen() || message["request_id"].toString() != requestId_ ||
        nextEntry_ >= entries_.size()) throw std::runtime_error("Unexpected game data chunk.");
    const auto encoded = message["data"].toString().toLatin1();
    const auto bytes = QByteArray::fromBase64(encoded);
    const auto &entry = entries_[nextEntry_];
    if (bytes.isEmpty() || bytes.size() > chunkBytes || bytes.toBase64() != encoded ||
        bytes.size() > entry.size - currentBytes_)
        throw std::runtime_error("Invalid game data chunk.");
    if (currentFile_.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write staged game data.");
    currentHash_.addData(bytes); currentBytes_ += bytes.size();
    if (onProgress) onProgress(completeBytes_ + currentBytes_, totalBytes_);
    timeout_.start(120000);
}

void LanGameExchange::receiveFileEnd(const QJsonObject &message) {
    if (!currentFile_.isOpen() || message["request_id"].toString() != requestId_ ||
        nextEntry_ >= entries_.size()) throw std::runtime_error("Unexpected game file completion.");
    const auto &entry = entries_[nextEntry_];
    if (message["path"].toString() != currentPath_ || currentPath_ != entry.path ||
        currentBytes_ != entry.size || QString::fromLatin1(currentHash_.result().toHex()) != entry.sha256 ||
        !currentFile_.flush()) throw std::runtime_error("Transferred game file failed verification.");
    currentFile_.close();
    const auto part = stage_ + '/' + currentPath_ + ".novelty-part";
    const auto final = stage_ + '/' + currentPath_;
    if (QFileInfo(final).exists() || QFileInfo(final).isSymbolicLink() ||
        !QFile::rename(part, final))
        throw std::runtime_error("Cannot publish verified staged game file.");
    completeBytes_ += entry.size; ++nextEntry_; currentPath_.clear();
    verifiedStagedFiles_.insert(entry.path);
    if (onProgress) onProgress(completeBytes_, totalBytes_);
    requestNext();
}

void LanGameExchange::finalize() {
    if (currentFile_.isOpen() || nextEntry_ != entries_.size() || completeBytes_ != totalBytes_)
        throw std::runtime_error("LAN game installation is incomplete.");
    QSet<QString> expected{QStringLiteral(".novelty-transfer.json")};
    for (const auto &entry : entries_) {
        expected.insert(entry.path);
        const auto path = stage_ + '/' + entry.path;
        noLinks(path, stage_);
        const QFileInfo info(path);
        if (entry.directory ? !info.isDir() : (!info.isFile() || info.size() != entry.size ||
            !verifiedStagedFiles_.contains(entry.path)))
            throw std::runtime_error("Staged game failed final verification.");
    }
    QDirIterator it(stage_, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto info = it.nextFileInfo();
        const auto relative = QDir(stage_).relativeFilePath(info.absoluteFilePath());
        if (info.isSymbolicLink() || !expected.contains(relative))
            throw std::runtime_error("Staged game contains an unverified file.");
    }
    const auto destination = library_.root() + '/' + descriptor_["name"].toString();
    for (const auto &local : library_.games())
        if (loadSaveConfig(library_, local.id).profileGameId == remoteGameId_)
            throw std::runtime_error("This paired-device game was installed during the transfer.");
    if (QFileInfo(destination).exists() || QFileInfo(destination).isSymbolicLink())
        throw std::runtime_error("A game folder with this name already exists in Games. The verified transfer is kept.");
    if (!QFile::remove(stage_ + "/.novelty-transfer.json") || !QDir().rename(stage_, destination))
        throw std::runtime_error("Cannot place verified game in Games.");
    const auto before = library_.games().size();
    const auto candidate = inspect(destination);
    if (library_.import({candidate}, library_.settings()["runner"].toString()) != 1 ||
        library_.games().size() != before + 1)
        throw std::runtime_error("Game files arrived, but Novelty could not register them. Use Add → Select folder.");
    Game local;
    for (const auto &game : library_.games()) if (game.folder == destination) local = game;
    if (local.id.isEmpty()) throw std::runtime_error("Transferred game was not registered.");
    local.name = descriptor_["name"].toString(); local.backend = "umu";
    local.locale = descriptor_["locale"].toString();
    local.metadata = descriptor_["metadata"].toObject();
    for (const auto &argument : descriptor_["arguments"].toArray()) local.arguments.append(argument.toString());
    const auto executable = descriptor_["executable"].toString();
    if (!executable.isEmpty()) {
        const auto path = destination + '/' + executable;
        if (!QFileInfo(path).isFile() || QFileInfo(path).isSymbolicLink())
            throw std::runtime_error("Transferred game executable is missing.");
        local.executable = path;
    }
    const auto cwd = descriptor_["cwd"].toString();
    local.workingDirectory = cwd.isEmpty() ? destination : destination + '/' + cwd;
    if (!QFileInfo(local.workingDirectory).isDir())
        throw std::runtime_error("Transferred game working directory is missing.");
    library_.save(local);
    auto config = loadSaveConfig(library_, local.id);
    config.profileGameId = remoteGameId_;
    config.gameIdentity = descriptor_["game_identity"].toString();
    config.releaseIdentity = descriptor_["release_identity"].toString();
    config.saveCompatibility = descriptor_["save_compatibility"].toString();
    for (const auto &value : descriptor_["mappings"].toArray()) {
        const auto item = value.toObject();
        config.mappings.append({item["scope"].toString(), item["path"].toString()});
    }
    saveSaveConfig(library_, local.id, config);
    timeout_.stop(); requestId_.clear();
    if (onInstalled) onInstalled(local.id);
}

void LanGameExchange::fail(const QString &reason) {
    if (!connection_.ready()) return;
    timeout_.stop(); manifestPoll_.stop(); resumePoll_.stop();
    if (manifestWorker_.joinable()) manifestWorker_.request_stop();
    if (resumeWorker_.joinable()) resumeWorker_.request_stop();
    currentFile_.close(); sendingFile_.close();
    if (onError) onError(reason);
    connection_.close();
}

void LanGameExchange::handle(const QJsonObject &message) {
    const auto type = message["type"].toString();
    try {
        if (type == "game-list") { sendCatalogue(message); return; }
        if (type == "game-manifest") { sendManifest(message); return; }
        if (type == "game-file-get") { sendFile(message); return; }
        if (type == "game-error") {
            if (message["request_id"].toString() != requestId_ || !uuid(requestId_) ||
                message["message"].toString().size() > 180)
                throw std::runtime_error("Unexpected paired-device game error.");
            fail(message["message"].toString()); return;
        }
        if (type == "game-found") {
            if (message["request_id"].toString() != requestId_ || !message["games"].isArray() ||
                message["games"].toArray().size() > 64)
                throw std::runtime_error("Unexpected paired-device game catalogue.");
            const auto games = message["games"].toArray();
            for (const auto &game : games) if (!validSummary(game.toObject()))
                throw std::runtime_error("Peer game catalogue is invalid.");
            timeout_.stop(); requestId_.clear();
            if (onCatalogue) onCatalogue(games);
            return;
        }
        if (type == "game-manifest-progress") {
            if (message["request_id"].toString() != requestId_ || remoteGameId_.isEmpty() || receivingManifest_)
                throw std::runtime_error("Unexpected game manifest progress.");
            timeout_.start(120000); return;
        }
        if (type == "game-manifest-begin") { receiveBegin(message); return; }
        if (type == "game-entry") { receiveEntry(message); return; }
        if (type == "game-manifest-end") { receiveManifestEnd(message); return; }
        if (type == "game-file-begin") { receiveFileBegin(message); return; }
        if (type == "game-chunk") { receiveChunk(message); return; }
        if (type == "game-file-end") { receiveFileEnd(message); return; }
        throw std::runtime_error("Unknown paired-device game message.");
    } catch (const std::exception &e) { fail(QString::fromUtf8(e.what())); }
}

} // namespace novelty
