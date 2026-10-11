#pragma once

#include <QtCore/qbytearray.h>
#include <QtCore/qstring.h>

namespace novelty {

// A fresh nonce is exchanged by each side inside the encrypted pairing session.
QByteArray newPairingNonce();
// Both screens must show this exact phrase before either peer is trusted.
QString pairingPhrase(const QString &localFingerprint, const QByteArray &localNonce,
                      const QString &remoteFingerprint, const QByteArray &remoteNonce);

struct DiscoveryAnnouncement { QByteArray sessionId; quint16 port = 0; };
QByteArray newDiscoverySessionId();
QByteArray encodeDiscoveryAnnouncement(const DiscoveryAnnouncement &);
// Returns false for unrelated or malformed datagrams. Announcements carry no library data.
bool decodeDiscoveryAnnouncement(const QByteArray &, DiscoveryAnnouncement &);

// Control frames have a two-byte magic, one-byte version and a big-endian length.
// Their payload is deliberately kept separate from the untrusted transport.
constexpr qsizetype maxControlPayload = 64 * 1024;
QByteArray encodeControlFrame(const QByteArray &payload);
// Returns false for a partial frame. Throws for malformed or oversized input.
bool takeControlFrame(QByteArray &buffer, QByteArray &payload);

} // namespace novelty
