#pragma once
#include "core.h"

namespace novelty {

struct DeviceIdentity {
    QString certificatePath;
    QString privateKeyPath;
    QString fingerprint; // SHA-256 of the certificate DER, lowercase hexadecimal.
};

DeviceIdentity ensureDeviceIdentity(const Library &);

struct TrustedPeer {
    QString fingerprint;
    QString name;
    QString approvedAt;
    QString address; // Last explicitly used LAN endpoint; never a trust authority.
    quint16 port = 0;
};
QList<TrustedPeer> trustedPeers(const Library &);
bool isTrustedPeer(const Library &, const QString &fingerprint);
// Call only after both people approve a verified pairing phrase.
void approvePairedPeer(const Library &, const QString &fingerprint, const QString &name);
void revokePeer(const Library &, const QString &fingerprint);
void rememberPeerEndpoint(const Library &, const QString &fingerprint, const QString &address, quint16 port);

} // namespace novelty
