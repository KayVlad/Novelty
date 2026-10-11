#include "peer_tls.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qfile.h>
#include <openssl/x509.h>
#include <memory>
#include <stdexcept>

namespace novelty {
namespace {
bool validFingerprint(const QString &value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
bool validPeerCertificate(const QSslCertificate &certificate) {
    const auto now = QDateTime::currentDateTimeUtc();
    if (certificate.isNull() || certificate.isBlacklisted() ||
        !certificate.effectiveDate().isValid() || !certificate.expiryDate().isValid() ||
        certificate.effectiveDate() > now || certificate.expiryDate() <= now) return false;
    const auto der = certificate.toDer();
    const auto *begin = reinterpret_cast<const unsigned char *>(der.constData());
    auto *cursor = begin;
    std::unique_ptr<X509, decltype(&X509_free)> x509(d2i_X509(nullptr, &cursor, der.size()), X509_free);
    if (!x509 || cursor != begin + der.size() ||
        X509_NAME_cmp(X509_get_subject_name(x509.get()), X509_get_issuer_name(x509.get())) != 0)
        return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(X509_get_pubkey(x509.get()), EVP_PKEY_free);
    return key && X509_verify(x509.get(), key.get()) == 1;
}
}

void configureDeviceTls(QSslSocket &socket, const DeviceIdentity &identity) {
    if (!validFingerprint(identity.fingerprint)) throw std::runtime_error("Invalid local TLS identity.");
    QFile file(identity.certificatePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024)
        throw std::runtime_error("Cannot read local TLS certificate.");
    const auto certificates = QSslCertificate::fromData(file.readAll());
    if (certificates.size() != 1 || peerCertificateFingerprint(certificates.first()) != identity.fingerprint)
        throw std::runtime_error("Local TLS certificate changed while opening it.");
    socket.setProtocol(QSsl::TlsV1_2OrLater);
    socket.setLocalCertificate(certificates.first());
    socket.setPrivateKey(identity.privateKeyPath, QSsl::Ec);
    socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
    socket.setPeerVerifyDepth(1);
    socket.setReadBufferSize(128 * 1024);
}

QString peerCertificateFingerprint(const QSslCertificate &certificate) {
    if (!validPeerCertificate(certificate)) return {};
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
}

bool expectedPeerTlsErrors(const QSslCertificate &certificate, const QList<QSslError> &errors,
                           const QString &expectedFingerprint) {
    const auto fingerprint = peerCertificateFingerprint(certificate);
    if (fingerprint.isEmpty() || (!expectedFingerprint.isEmpty() && fingerprint != expectedFingerprint)) return false;
    if (errors.isEmpty()) return true;
    for (const auto &error : errors)
        if (error.error() != QSslError::SelfSignedCertificate || error.certificate() != certificate) return false;
    return true;
}

bool acceptExpectedPeerTlsErrors(QSslSocket &socket, const QList<QSslError> &errors,
                                 const QString &expectedFingerprint) {
    if (!expectedPeerTlsErrors(socket.peerCertificate(), errors, expectedFingerprint)) return false;
    socket.ignoreSslErrors(errors);
    return true;
}

QString authorizedPeerFingerprint(const Library &lib, const QSslSocket &socket) {
    if (!socket.isEncrypted()) throw std::runtime_error("Peer connection is not encrypted.");
    const auto certificate = socket.peerCertificate();
    const auto fingerprint = peerCertificateFingerprint(certificate);
    if (fingerprint.isEmpty() ||
        !expectedPeerTlsErrors(certificate, socket.sslHandshakeErrors(), fingerprint) ||
        !isTrustedPeer(lib, fingerprint))
        throw std::runtime_error("Peer certificate is not trusted for this Games library.");
    return fingerprint;
}

} // namespace novelty
