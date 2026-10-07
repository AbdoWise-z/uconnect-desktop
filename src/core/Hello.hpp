#pragma once
// The first message each side sends once a peer session is up. It travels over
// the encrypted session, never in the rendezvous `meta` blob: the server stores
// and serves `meta` in plaintext, and a device's name and offered services are
// not something it needs to know.
//
// Wire format of every app message: [u8 kind][payload]. Kind 1 is a UTF-8 JSON
// control message whose "t" field names its type, so other implementations
// (e.g. the Flutter app) can parse it without sharing code with this one.
// Kind 2 carries raw bytes for a service; its framing belongs to that service.
// Kind 3 carries storage upload bytes, independently of kind 2 downloads.
// docs/protocol.md specifies all of it.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ucd {

enum class MessageKind : uint8_t {
    Control = 1,
    Data    = 2,
    StorageUploadData = 3,  // separate from downloads, whose request ids can overlap
};

std::vector<uint8_t>       encodeControl(const QJsonObject& message);
std::optional<QJsonObject> decodeControl(std::span<const uint8_t> message);

enum Service : uint32_t {
    ServiceNone    = 0,
    ServiceStorage = 1u << 0,
    ServiceCamera  = 1u << 1,
    ServiceScreen  = 1u << 2,
    ServiceControl = 1u << 3,
};

QStringList serviceNames(uint32_t services);
uint32_t    servicesFromNames(const QStringList& names);

struct Hello {
    static constexpr int kVersion = 1;

    QString  name;
    QString  os;
    uint32_t services = ServiceNone;

    std::vector<uint8_t>        encode() const;
    static std::optional<Hello> decode(std::span<const uint8_t> message);
};

QString localOsName();

}  // namespace ucd
