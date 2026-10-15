#include "devices.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qsavefile.h>
#include <QtCore/quuid.h>
#include <QtNetwork/qhostaddress.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <memory>
#include <stdexcept>

namespace novelty {
bool validDeviceName(const QString &name) {
    if (name.isEmpty() || name.size() > 80 || name != name.trimmed()) return false;
    for (const auto c : name)
        if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format ||
            c.category() == QChar::Other_Surrogate || c.unicode() == 0x034f ||
            (c.unicode() >= 0xfe00 && c.unicode() <= 0xfe0f)) return false;
    return true;
}
namespace {
[[noreturn]] void fail(const QString &s) { throw std::runtime_error(s.toStdString()); }
template <typename T, void (*Free)(T *)> using Owned = std::unique_ptr<T, decltype(Free)>;
bool validFingerprint(const QString &fingerprint) {
    if (fingerprint.size() != 64) return false;
    for (const auto c : fingerprint)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
QByteArray pemKey(EVP_PKEY *key) {
    Owned<BIO, BIO_free_all> bio(BIO_new(BIO_s_mem()), BIO_free_all);
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
        fail("Cannot encode the local device private key.");
    BUF_MEM *data = nullptr; BIO_get_mem_ptr(bio.get(), &data);
    return QByteArray(data->data, qsizetype(data->length));
}
QByteArray pemCertificate(X509 *certificate) {
    Owned<BIO, BIO_free_all> bio(BIO_new(BIO_s_mem()), BIO_free_all);
    if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1)
        fail("Cannot encode the local device certificate.");
    BUF_MEM *data = nullptr; BIO_get_mem_ptr(bio.get(), &data);
    return QByteArray(data->data, qsizetype(data->length));
}
void extension(X509 *certificate, X509V3_CTX &context, int nid, const char *value) {
    Owned<X509_EXTENSION, X509_EXTENSION_free> item(X509V3_EXT_conf_nid(nullptr, &context, nid, const_cast<char *>(value)), X509_EXTENSION_free);
    if (!item || X509_add_ext(certificate, item.get(), -1) != 1) fail("Cannot add a device certificate restriction.");
}
QByteArray createCertificate(EVP_PKEY *key) {
    Owned<X509, X509_free> cert(X509_new(), X509_free);
    if (!cert || X509_set_version(cert.get(), 2) != 1) fail("Cannot create a local device certificate.");
    unsigned char serial[16];
    if (RAND_bytes(serial, sizeof(serial)) != 1) fail("Cannot create a random certificate serial.");
    serial[0] &= 0x7f;
    Owned<BIGNUM, BN_free> number(BN_bin2bn(serial, sizeof(serial), nullptr), BN_free);
    if (!number) fail("Cannot create a certificate serial.");
    Owned<ASN1_INTEGER, ASN1_INTEGER_free> asn1(BN_to_ASN1_INTEGER(number.get(), nullptr), ASN1_INTEGER_free);
    if (!asn1 || X509_set_serialNumber(cert.get(), asn1.get()) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 10L * 365 * 24 * 60 * 60) ||
        X509_set_pubkey(cert.get(), key) != 1) fail("Cannot set device certificate properties.");
    const auto name = X509_get_subject_name(cert.get());
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                            reinterpret_cast<const unsigned char *>("Novelty Device"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1) fail("Cannot name the local device certificate.");
    X509V3_CTX context;
    X509V3_set_ctx(&context, cert.get(), cert.get(), nullptr, nullptr, 0);
    extension(cert.get(), context, NID_basic_constraints, "critical,CA:FALSE");
    extension(cert.get(), context, NID_key_usage, "critical,digitalSignature");
    extension(cert.get(), context, NID_ext_key_usage, "serverAuth,clientAuth");
    extension(cert.get(), context, NID_subject_alt_name, "DNS:novelty.local");
    if (X509_sign(cert.get(), key, EVP_sha256()) <= 0) fail("Cannot sign the local device certificate.");
    return pemCertificate(cert.get());
}
void writePrivate(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) fail("Cannot write local device identity.");
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit()) fail("Cannot save local device identity.");
}
DeviceIdentity readIdentity(const QString &directory) {
    const auto certificatePath = directory + "/certificate.pem", keyPath = directory + "/private-key.pem";
    const QFileInfo certInfo(certificatePath), keyInfo(keyPath);
    if (QFileInfo(directory).isSymbolicLink() || certInfo.isSymbolicLink() || keyInfo.isSymbolicLink() ||
        !certInfo.isFile() || !keyInfo.isFile() || keyInfo.permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther))
        fail("Local device identity is incomplete or its private key is exposed. Repair it before pairing.");
    QFile certFile(certificatePath), keyFile(keyPath);
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly) ||
        certFile.size() <= 0 || keyFile.size() <= 0 ||
        certFile.size() > 64 * 1024 || keyFile.size() > 64 * 1024) fail("Cannot read local device identity safely.");
    const auto certBytes = certFile.readAll(), keyBytes = keyFile.readAll();
    Owned<BIO, BIO_free_all> certBio(BIO_new_mem_buf(certBytes.constData(), certBytes.size()), BIO_free_all);
    Owned<BIO, BIO_free_all> keyBio(BIO_new_mem_buf(keyBytes.constData(), keyBytes.size()), BIO_free_all);
    Owned<X509, X509_free> cert(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free);
    Owned<EVP_PKEY, EVP_PKEY_free> key(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!cert || !key || EVP_PKEY_id(key.get()) != EVP_PKEY_EC ||
        X509_check_private_key(cert.get(), key.get()) != 1 || X509_verify(cert.get(), key.get()) != 1 ||
        X509_cmp_current_time(X509_get0_notBefore(cert.get())) > 0 ||
        X509_cmp_current_time(X509_get0_notAfter(cert.get())) < 0)
        fail("Local device certificate and private key do not match.");
    unsigned char *der = nullptr;
    const int size = i2d_X509(cert.get(), &der);
    if (size <= 0 || !der) fail("Cannot fingerprint the local device certificate.");
    const auto fingerprint = QString::fromLatin1(QCryptographicHash::hash(QByteArray(reinterpret_cast<char *>(der), size),
                                                                          QCryptographicHash::Sha256).toHex());
    OPENSSL_free(der);
    return {certificatePath, keyPath, fingerprint};
}
QString peersPath(const Library &lib) { return lib.dataRoot() + "/devices/" + lib.profileId() + "/peers.json"; }
QList<TrustedPeer> readPeers(const Library &lib) {
    const auto path = peersPath(lib);
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymbolicLink()) return {};
    if (!info.isFile() || info.isSymbolicLink() || info.size() > 256 * 1024 ||
        info.permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther))
        fail("Trusted-device records are missing protection or are damaged.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot read trusted-device records.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail("Trusted-device records are damaged.");
    const auto object = document.object();
    if (object["schema"] != 1 || object["profile_id"] != lib.profileId() || !object["peers"].isArray())
        fail("Trusted-device records belong to another profile or version.");
    const auto records = object["peers"].toArray();
    if (records.size() > 32) fail("Too many trusted-device records.");
    QList<TrustedPeer> peers;
    for (const auto &entry : records) {
        if (!entry.isObject()) fail("Trusted-device record is damaged.");
        const auto record = entry.toObject();
        TrustedPeer peer{record["fingerprint"].toString(), record["name"].toString(), record["approved_at"].toString(),
                         record["address"].toString(), quint16(record["port"].toInt())};
        if (!validFingerprint(peer.fingerprint) || !validDeviceName(peer.name) ||
            !QDateTime::fromString(peer.approvedAt, Qt::ISODateWithMs).isValid())
            fail("Trusted-device record is damaged.");
        if ((!peer.address.isEmpty() || peer.port != 0) &&
            (QHostAddress(peer.address).isNull() || peer.port == 0 ||
             record["port"].toInt() < 1 || record["port"].toInt() > 65535))
            fail("Trusted-device endpoint is damaged.");
        for (const auto &known : peers) if (known.fingerprint == peer.fingerprint)
            fail("Trusted-device record has a duplicate identity.");
        peers.append(peer);
    }
    return peers;
}
void writePeers(const Library &lib, const QList<TrustedPeer> &peers) {
    QJsonArray records;
    for (const auto &peer : peers) {
        QJsonObject record{{"fingerprint", peer.fingerprint}, {"name", peer.name}, {"approved_at", peer.approvedAt}};
        if (!peer.address.isEmpty()) { record["address"] = peer.address; record["port"] = int(peer.port); }
        records.append(record);
    }
    QSaveFile file(peersPath(lib));
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) fail("Cannot protect trusted-device records.");
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"profile_id", lib.profileId()},
                                                 {"peers", records}}).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) fail("Cannot save trusted-device records.");
}
} // namespace

DeviceIdentity ensureDeviceIdentity(const Library &lib) {
    const auto root = lib.dataRoot() + "/devices", final = root + '/' + lib.profileId();
    if (QFileInfo(root).isSymbolicLink() || !QDir().mkpath(root)) fail("Cannot create local device identity storage.");
    if (QFileInfo(final).exists() || QFileInfo(final).isSymbolicLink()) return readIdentity(final);
    const auto stage = root + "/.staging-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(stage) || !QFile::setPermissions(stage, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        fail("Cannot protect local device identity storage.");
    try {
        Owned<EVP_PKEY, EVP_PKEY_free> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free);
        if (!key) fail("Cannot generate local device key.");
        writePrivate(stage + "/private-key.pem", pemKey(key.get()));
        writePrivate(stage + "/certificate.pem", createCertificate(key.get()));
        readIdentity(stage);
        if (!QDir().rename(stage, final)) {
            if (QFileInfo(final).exists()) { QDir(stage).removeRecursively(); return readIdentity(final); }
            fail("Cannot publish local device identity.");
        }
        return readIdentity(final);
    } catch (...) { QDir(stage).removeRecursively(); throw; }
}
QList<TrustedPeer> trustedPeers(const Library &lib) {
    if (!QFileInfo(peersPath(lib)).exists() && !QFileInfo(peersPath(lib)).isSymbolicLink()) return {};
    ensureDeviceIdentity(lib);
    return readPeers(lib);
}
bool isTrustedPeer(const Library &lib, const QString &fingerprint) {
    if (!validFingerprint(fingerprint)) return false;
    for (const auto &peer : trustedPeers(lib)) if (peer.fingerprint == fingerprint) return true;
    return false;
}
void approvePairedPeer(const Library &lib, const QString &fingerprint, const QString &name) {
    if (!validFingerprint(fingerprint) || !validDeviceName(name) || ensureDeviceIdentity(lib).fingerprint == fingerprint)
        fail("Choose a valid, distinct paired device and name.");
    QLockFile lock(peersPath(lib) + ".lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) fail("Trusted-device records are busy.");
    auto peers = readPeers(lib);
    for (auto &peer : peers) if (peer.fingerprint == fingerprint) {
        peer.name = name; writePeers(lib, peers); return;
    }
    if (peers.size() >= 32) fail("Too many trusted devices. Revoke one before pairing another.");
    peers.append({fingerprint, name, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), {}, 0});
    writePeers(lib, peers);
}
void revokePeer(const Library &lib, const QString &fingerprint) {
    if (!validFingerprint(fingerprint)) fail("Invalid trusted-device identity.");
    ensureDeviceIdentity(lib);
    QLockFile lock(peersPath(lib) + ".lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) fail("Trusted-device records are busy.");
    auto peers = readPeers(lib);
    for (int i = 0; i < peers.size(); ++i) if (peers[i].fingerprint == fingerprint) {
        peers.removeAt(i); writePeers(lib, peers); return;
    }
}
void rememberPeerEndpoint(const Library &lib, const QString &fingerprint, const QString &address, quint16 port) {
    if (!validFingerprint(fingerprint) || QHostAddress(address).isNull() || port == 0)
        fail("Choose a valid paired device LAN address and port.");
    ensureDeviceIdentity(lib);
    QLockFile lock(peersPath(lib) + ".lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) fail("Trusted-device records are busy.");
    auto peers = readPeers(lib);
    for (auto &peer : peers) if (peer.fingerprint == fingerprint) {
        peer.address = QHostAddress(address).toString(); peer.port = port;
        writePeers(lib, peers); return;
    }
    fail("Pair this device before remembering its LAN endpoint.");
}
} // namespace novelty
