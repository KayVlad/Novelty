#include "pairing.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qstringlist.h>
#include <openssl/rand.h>
#include <stdexcept>

namespace novelty {
namespace {
bool fingerprintValid(const QString &value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
void appendParticipant(QByteArray &transcript, const QString &fingerprint, const QByteArray &nonce) {
    transcript += QByteArray::fromHex(fingerprint.toLatin1());
    transcript += nonce;
}
}

QByteArray newPairingNonce() {
    QByteArray nonce(32, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1)
        throw std::runtime_error("Cannot create a pairing nonce.");
    return nonce;
}

QByteArray newDiscoverySessionId() {
    QByteArray id(16, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(id.data()), id.size()) != 1)
        throw std::runtime_error("Cannot create a discovery session ID.");
    return id;
}

QByteArray encodeDiscoveryAnnouncement(const DiscoveryAnnouncement &item) {
    if (item.sessionId.size() != 16 || item.port == 0)
        throw std::runtime_error("Invalid LAN discovery announcement.");
    QByteArray bytes("NVD1", 4);
    bytes += item.sessionId;
    bytes.append(char(item.port >> 8));
    bytes.append(char(item.port & 0xff));
    return bytes;
}

bool decodeDiscoveryAnnouncement(const QByteArray &bytes, DiscoveryAnnouncement &item) {
    if (bytes.size() != 22 || bytes.left(4) != "NVD1") return false;
    const auto port = quint16((quint16(quint8(bytes[20])) << 8) | quint8(bytes[21]));
    if (port == 0) return false;
    item = {bytes.mid(4, 16), port};
    return true;
}

QString pairingPhrase(const QString &localFingerprint, const QByteArray &localNonce,
                      const QString &remoteFingerprint, const QByteArray &remoteNonce) {
    if (!fingerprintValid(localFingerprint) || !fingerprintValid(remoteFingerprint) ||
        localFingerprint == remoteFingerprint || localNonce.size() != 32 || remoteNonce.size() != 32)
        throw std::runtime_error("Invalid pairing identity or nonce.");
    QByteArray transcript("Novelty LAN pairing phrase v1\0", 30);
    if (localFingerprint < remoteFingerprint) {
        appendParticipant(transcript, localFingerprint, localNonce);
        appendParticipant(transcript, remoteFingerprint, remoteNonce);
    } else {
        appendParticipant(transcript, remoteFingerprint, remoteNonce);
        appendParticipant(transcript, localFingerprint, localNonce);
    }
    const auto digest = QCryptographicHash::hash(transcript, QCryptographicHash::Sha256).left(10).toHex().toUpper();
    QStringList groups;
    for (int i = 0; i < digest.size(); i += 4) groups.append(QString::fromLatin1(digest.mid(i, 4)));
    return groups.join(QLatin1Char(' '));
}

QByteArray encodeControlFrame(const QByteArray &payload) {
    if (payload.size() > maxControlPayload) throw std::runtime_error("Control message exceeds its size limit.");
    const auto size = quint32(payload.size());
    QByteArray frame;
    frame.reserve(7 + payload.size());
    frame.append("NV", 2);
    frame.append(char(1));
    for (int shift = 24; shift >= 0; shift -= 8) frame.append(char((size >> shift) & 0xff));
    frame += payload;
    return frame;
}

bool takeControlFrame(QByteArray &buffer, QByteArray &payload) {
    if (buffer.size() > 2 * (7 + maxControlPayload))
        throw std::runtime_error("Control input exceeds its buffer limit.");
    if (buffer.size() < 7) return false;
    if (buffer[0] != 'N' || buffer[1] != 'V' || buffer[2] != 1)
        throw std::runtime_error("Unsupported control frame.");
    quint32 size = 0;
    for (int i = 3; i < 7; ++i) size = (size << 8) | quint8(buffer[i]);
    if (size > maxControlPayload) throw std::runtime_error("Control message exceeds its size limit.");
    if (buffer.size() < 7 + qsizetype(size)) return false;
    payload = buffer.mid(7, size);
    buffer.remove(0, 7 + size);
    return true;
}

} // namespace novelty
