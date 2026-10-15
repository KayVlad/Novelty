#include "steam_shortcut.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qsavefile.h>
#include <QtCore/qset.h>
#include <QtCore/quuid.h>
#include <algorithm>
#include <climits>
#include <stdexcept>

namespace novelty {
namespace {
[[noreturn]] void fail(const QString &s) { throw std::runtime_error(s.toStdString()); }
struct Reader {
    QByteArray data; qsizetype at = 0;
    quint8 byte() { if (at >= data.size()) fail("Steam shortcuts file ended unexpectedly."); return quint8(data[at++]); }
    QByteArray string() {
        const auto end = data.indexOf('\0', at);
        if (end < 0 || end - at > 1024 * 1024) fail("Steam shortcuts contain an invalid string.");
        const auto value = data.mid(at, end - at); at = end + 1; return value;
    }
    quint32 integer() {
        if (data.size() - at < 4) fail("Steam shortcuts contain an incomplete number.");
        const auto p = reinterpret_cast<const unsigned char *>(data.constData() + at); at += 4;
        return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
    }
    void value(quint8 type, int depth, QMap<QByteArray, QByteArray> *fields = nullptr,
               QMap<QByteArray, QPair<qsizetype, qsizetype>> *spans = nullptr) {
        if (depth > 12) fail("Steam shortcuts nesting is too deep.");
        if (type == 0) {
            for (;;) {
                const auto next = byte(); if (next == 8) return;
                const auto key = string();
                if (next == 1) {
                    const auto begin = at; const auto v = string();
                    if (fields) fields->insert(key, v);
                    if (spans) spans->insert(key, {begin, at});
                }
                else if (next == 2) { const auto v = integer(); if (fields) fields->insert(key, QByteArray::number(v)); }
                else value(next, depth + 1);
            }
        } else if (type == 1) string();
        else if (type == 2) integer();
        else fail("Steam shortcuts use an unsupported binary field type. No file was changed.");
    }
};
void key(QByteArray &out, quint8 type, const QByteArray &name) { out.append(char(type)); out.append(name); out.append('\0'); }
void string(QByteArray &out, const QByteArray &name, const QByteArray &value) {
    key(out, 1, name); out.append(value); out.append('\0');
}
void integer(QByteArray &out, const QByteArray &name, quint32 value) {
    key(out, 2, name);
    for (int i = 0; i < 4; ++i) out.append(char((value >> (8 * i)) & 0xff));
}
QByteArray quoted(const QString &path) {
    if (path.contains('"') || path.contains('\n') || path.contains('\r') || path.contains(QChar::Null) ||
        path.contains('$') || path.contains('`') || path.contains('\\'))
        fail("Steam shortcut paths cannot contain quotes, line breaks, backslashes, dollar signs or backticks.");
    return ('"' + path + '"').toUtf8();
}
void commit(const QString &path, const QByteArray &original, const QByteArray &modified) {
    const QFileInfo info(path);
    if (info.exists()) {
        QFile verify(path);
        if (!verify.open(QIODevice::ReadOnly) || verify.readAll() != original)
            fail("Steam shortcuts changed while Novelty was preparing this entry. Retry after closing Steam.");
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(modified) != modified.size() || !file.commit())
        fail("Cannot save Steam shortcuts: " + file.errorString());
}
} // namespace

SteamShortcutResult addSteamShortcut(const Library &lib, const Game &game, const QString &shortcutFile,
                                     const QString &noveltyExecutable) {
    if (game.id.isEmpty() || lib.game(game.id).json() != game.json()) fail("Refresh this game before adding it to Steam.");
    if (!QFileInfo(noveltyExecutable).isFile() || !QFileInfo(noveltyExecutable).isExecutable())
        fail("Novelty's executable is unavailable for the Steam shortcut.");
    const QFileInfo output(shortcutFile);
    if (output.fileName() != "shortcuts.vdf" || output.dir().dirName() != "config" ||
        !QFileInfo(output.dir().absolutePath()).isDir() || output.isSymbolicLink())
        fail("Choose an existing Steam account config/shortcuts.vdf location.");
    QByteArray original;
    if (output.exists()) {
        QFile file(shortcutFile);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) fail("Cannot read Steam shortcuts safely.");
        original = file.readAll();
    } else original = QByteArray("\0shortcuts\0", 11) + char(8);
    Reader reader{original};
    if (reader.byte() != 0 || reader.string() != "shortcuts") fail("Steam shortcuts have an unexpected root.");
    const auto newExe = quoted(QFileInfo(noveltyExecutable).absoluteFilePath());
    const auto newStartDir = quoted(QFileInfo(noveltyExecutable).absolutePath());
    const auto newOptions = "--root " + quoted(lib.root()) + " --run " + game.id.toUtf8() +
                            " --novelty-shortcut " + (lib.profileId() + ':' + game.id).toUtf8();
    qsizetype rootEnd = -1; int nextIndex = 0; quint32 existingAppId = 0;
    QMap<QByteArray, QPair<qsizetype, qsizetype>> existingSpans;
    QSet<quint32> appIds;
    for (;;) {
        const auto start = reader.at;
        const auto type = reader.byte();
        if (type == 8) { rootEnd = start; break; }
        if (type != 0) fail("Steam shortcuts contain an unexpected entry.");
        const auto index = reader.string();
        bool validIndex = !index.isEmpty();
        for (const auto ch : index) validIndex &= ch >= '0' && ch <= '9';
        if (!validIndex) fail("Steam shortcuts contain an invalid entry number.");
        bool numberOk = false; const int parsed = index.toInt(&numberOk);
        if (!numberOk || parsed == INT_MAX) fail("Steam shortcuts entry number is too large.");
        nextIndex = std::max(nextIndex, parsed + 1);
        QMap<QByteArray, QByteArray> fields;
        QMap<QByteArray, QPair<qsizetype, qsizetype>> spans;
        reader.value(0, 1, &fields, &spans);
        bool idOk = false; const auto appId = fields.value("appid").toUInt(&idOk);
        if (idOk) appIds.insert(appId);
        const auto marker = "--novelty-shortcut " + (lib.profileId() + ':' + game.id).toUtf8();
        if (fields.value("LaunchOptions").contains(marker)) {
            if (!existingSpans.isEmpty()) fail("Steam shortcuts contain duplicate Novelty entries for this game.");
            existingAppId = appId; existingSpans = spans;
        }
    }
    if (reader.at < original.size() && !(reader.at + 1 == original.size() && reader.byte() == 8))
        fail("Steam shortcuts contain unexpected trailing data.");
    if (!existingSpans.isEmpty()) {
        for (const auto &field : {"exe", "StartDir", "LaunchOptions"})
            if (!existingSpans.contains(field)) fail("Existing Novelty shortcut is incomplete. No file was changed.");
        QByteArray modified = original;
        QList<QPair<QPair<qsizetype, qsizetype>, QByteArray>> replacements{
            {existingSpans["exe"], newExe}, {existingSpans["StartDir"], newStartDir},
            {existingSpans["LaunchOptions"], newOptions}};
        std::sort(replacements.begin(), replacements.end(), [](const auto &a, const auto &b) { return a.first.first > b.first.first; });
        for (const auto &replacement : replacements) {
            auto value = replacement.second; value.append('\0');
            modified.replace(replacement.first.first, replacement.first.second - replacement.first.first, value);
        }
        if (modified == original) return {false, existingAppId};
        commit(shortcutFile, original, modified);
        return {true, existingAppId};
    }
    const auto digest = QCryptographicHash::hash((lib.profileId() + game.id).toUtf8(), QCryptographicHash::Sha256);
    quint32 appId = quint32(quint8(digest[0])) | (quint32(quint8(digest[1])) << 8) |
                    (quint32(quint8(digest[2])) << 16) | (quint32(quint8(digest[3])) << 24);
    appId |= 0x80000000u;
    while (appIds.contains(appId)) ++appId;
    QByteArray entry; key(entry, 0, QByteArray::number(nextIndex));
    integer(entry, "appid", appId);
    string(entry, "appname", game.name.toUtf8());
    string(entry, "exe", newExe);
    string(entry, "StartDir", newStartDir);
    string(entry, "icon", QByteArray());
    string(entry, "ShortcutPath", QByteArray());
    string(entry, "LaunchOptions", newOptions);
    for (const auto &field : {"IsHidden", "AllowDesktopConfig", "AllowOverlay", "OpenVR", "Devkit"})
        integer(entry, field, QByteArray(field) == "AllowOverlay" ? 1 : 0);
    string(entry, "DevkitGameID", QByteArray()); string(entry, "FlatpakAppID", QByteArray());
    key(entry, 0, "tags"); entry.append(char(8)); entry.append(char(8));
    QByteArray modified = original; modified.insert(rootEnd, entry);
    commit(shortcutFile, original, modified);
    return {true, appId};
}
} // namespace novelty
