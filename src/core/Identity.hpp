#pragma once
// A device's long-term identity: an Ed25519 key pair (RFC 8032), generated
// once and kept in a file only this user can read.
//
// Proving it to a peer (wire, kind 1 JSON):
//     {"t":"id", "v":1, "key":"<64 hex public key>", "sig":"<128 hex signature>"}
// where the signature covers  "uconnect-desktop identity v1" || 0x00 ||
// channel_binding || public key,  and channel_binding is the uconnect session's
// value (Topic::channel_binding), identical on both ends. Binding the proof to
// the session stops anyone holding the topic key from replaying another
// device's proof into a session of their own.
//
// Fingerprint, shown to people for comparison: the first 16 bytes of
// SHA-256(public key) as 8 groups of 4 upper-case hex digits.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <QJsonObject>
#include <QString>

namespace ucd {

using PublicKey = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;
using ChannelBinding = std::array<uint8_t, 32>;

class Identity {
public:
    Identity(const Identity& other);
    Identity& operator=(const Identity& other);
    ~Identity();

    static Identity generate();
    // Reads the identity stored at `path`, creating and storing a new one if
    // there is none. A file that exists but cannot be used is moved aside, not
    // overwritten. Always returns an identity; `warning` explains anything that
    // went wrong (e.g. it could not be saved, so it lasts until the app exits).
    static Identity loadOrCreate(const QString& path, QString* warning);

    const PublicKey& publicKey() const { return public_; }
    Signature        sign(std::span<const uint8_t> message) const;

    // The {"t":"id"} proof for one session.
    std::vector<uint8_t> proof(const ChannelBinding& binding) const;

private:
    Identity() = default;
    static Identity fromSeed(std::array<uint8_t, 32> seed);

    std::array<uint8_t, 32> seed_{};
    std::array<uint8_t, 64> secret_{};
    PublicKey               public_{};
};

bool verifySignature(const PublicKey& key, std::span<const uint8_t> message, const Signature& signature);

// Checks a peer's {"t":"id"} message against this session's binding; the
// proven key, or nullopt if the message is malformed or the proof fails.
std::optional<PublicKey> verifyProof(const QJsonObject& message, const ChannelBinding& binding);

QString                  keyToHex(const PublicKey& key);
std::optional<PublicKey> keyFromHex(const QString& hex);
QString                  fingerprint(const PublicKey& key);
QString                  fingerprintOfHex(const QString& keyHex);

}  // namespace ucd
