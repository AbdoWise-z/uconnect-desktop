#include "Identity.hpp"

#include <algorithm>
#include <cstring>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStringList>

#include "core/Hello.hpp"

extern "C" {
#include "monocypher-ed25519.h"
#include "monocypher.h"
}

namespace ucd {
namespace {

constexpr char kProofContext[] = "uconnect identity v1";  // followed by a NUL byte
constexpr int  kFileVersion    = 1;

template <size_t N>
std::optional<std::array<uint8_t, N>> bytesFromHex(const QString& hex) {
    if (hex.size() != static_cast<qsizetype>(N * 2)) return std::nullopt;
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    if (raw.size() != static_cast<qsizetype>(N)) return std::nullopt;
    std::array<uint8_t, N> out{};
    std::copy(raw.begin(), raw.end(), out.begin());
    return out;
}

template <size_t N>
QString bytesToHex(const std::array<uint8_t, N>& bytes) {
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(N)).toHex());
}

std::vector<uint8_t> proofMessage(const ChannelBinding& binding, const PublicKey& key) {
    std::vector<uint8_t> m(std::begin(kProofContext), std::end(kProofContext));  // includes the NUL
    m.insert(m.end(), binding.begin(), binding.end());
    m.insert(m.end(), key.begin(), key.end());
    return m;
}

}  // namespace

Identity::Identity(const Identity& other) = default;
Identity& Identity::operator=(const Identity& other) = default;

Identity::~Identity() {
    crypto_wipe(seed_.data(), seed_.size());
    crypto_wipe(secret_.data(), secret_.size());
}

Identity Identity::fromSeed(std::array<uint8_t, 32> seed) {
    Identity id;
    id.seed_ = seed;
    // crypto_ed25519_key_pair wipes the seed it is given; hand it the copy.
    crypto_ed25519_key_pair(id.secret_.data(), id.public_.data(), seed.data());
    return id;
}

Identity Identity::generate() {
    std::array<quint32, 8> random{};
    QRandomGenerator::system()->fillRange(random.data(), random.size());
    std::array<uint8_t, 32> seed{};
    std::memcpy(seed.data(), random.data(), seed.size());
    crypto_wipe(random.data(), sizeof(random));
    return fromSeed(seed);
}

Identity Identity::loadOrCreate(const QString& path, QString* warning) {
    if (QFile::exists(path)) {
        QByteArray contents;
        if (QFile file(path); file.open(QIODevice::ReadOnly)) contents = file.readAll();  // closed again here
        const QJsonObject o    = QJsonDocument::fromJson(contents).object();
        const auto        seed = bytesFromHex<32>(o.value("seed").toString());
        if (o.value("v").toInt() == kFileVersion && seed) return fromSeed(*seed);

        // Never overwrite a key that might still matter to someone. (The file
        // must be closed first: Windows will not rename an open file.)
        const QString aside =
            path + QStringLiteral(".unusable-") + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        QFile::rename(path, aside);
        if (warning)
            *warning = QObject::tr("The identity file could not be read and was moved to %1. "
                                   "A new identity was created; devices that trusted the old one must trust this one again.")
                           .arg(QDir::toNativeSeparators(aside));
    }

    Identity id = generate();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    const QJsonObject o{
        {"v", kFileVersion},
        {"type", "ed25519"},
        {"seed", bytesToHex(id.seed_)},
    };
    const bool saved = out.open(QIODevice::WriteOnly) &&
                       out.write(QJsonDocument(o).toJson()) > 0 &&
                       out.commit();
    if (saved) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    } else if (warning) {
        *warning = QObject::tr("This device's identity could not be saved to %1; it will change the next time the app starts.")
                       .arg(QDir::toNativeSeparators(path));
    }
    return id;
}

Signature Identity::sign(std::span<const uint8_t> message) const {
    Signature s{};
    crypto_ed25519_sign(s.data(), secret_.data(), message.data(), message.size());
    return s;
}

std::vector<uint8_t> Identity::proof(const ChannelBinding& binding) const {
    const Signature sig = sign(proofMessage(binding, public_));
    return encodeControl({
        {"t", "id"},
        {"v", 1},
        {"key", keyToHex(public_)},
        {"sig", bytesToHex(sig)},
    });
}

bool verifySignature(const PublicKey& key, std::span<const uint8_t> message, const Signature& signature) {
    return crypto_ed25519_check(signature.data(), key.data(), message.data(), message.size()) == 0;
}

std::optional<PublicKey> verifyProof(const QJsonObject& message, const ChannelBinding& binding) {
    if (message.value("t").toString() != QLatin1String("id") || message.value("v").toInt() != 1)
        return std::nullopt;
    const auto key = keyFromHex(message.value("key").toString());
    const auto sig = bytesFromHex<64>(message.value("sig").toString());
    if (!key || !sig) return std::nullopt;
    if (!verifySignature(*key, proofMessage(binding, *key), *sig)) return std::nullopt;
    return key;
}

QString keyToHex(const PublicKey& key) {
    return bytesToHex(key);
}

std::optional<PublicKey> keyFromHex(const QString& hex) {
    return bytesFromHex<32>(hex.toLower());
}

QString fingerprint(const PublicKey& key) {
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char*>(key.data()), static_cast<qsizetype>(key.size())),
        QCryptographicHash::Sha256);
    const QString hex = QString::fromLatin1(digest.left(16).toHex()).toUpper();
    QStringList groups;
    for (int i = 0; i < hex.size(); i += 4) groups << hex.mid(i, 4);
    return groups.join(QLatin1Char(' '));
}

QString fingerprintOfHex(const QString& keyHex) {
    const auto key = keyFromHex(keyHex);
    return key ? fingerprint(*key) : QString();
}

}  // namespace ucd
