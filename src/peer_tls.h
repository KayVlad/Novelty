#pragma once

#include "devices.h"
#include <QtNetwork/qsslcertificate.h>
#include <QtNetwork/qsslerror.h>
#include <QtNetwork/qsslsocket.h>

namespace novelty {

void configureDeviceTls(QSslSocket &, const DeviceIdentity &);
QString peerCertificateFingerprint(const QSslCertificate &);
// Only the self-signed leaf error for the presented certificate may be bypassed.
bool expectedPeerTlsErrors(const QSslCertificate &, const QList<QSslError> &,
                           const QString &expectedFingerprint = {});
// Call inside sslErrors; the encrypted signal must still validate the certificate.
bool acceptExpectedPeerTlsErrors(QSslSocket &, const QList<QSslError> &,
                                 const QString &expectedFingerprint = {});
// Call after encrypted, before reading or writing application data.
QString authorizedPeerFingerprint(const Library &, const QSslSocket &);

} // namespace novelty
