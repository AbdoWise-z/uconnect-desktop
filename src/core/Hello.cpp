#include "Hello.hpp"

#include <array>
#include <cstring>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

namespace ucd {
namespace {

constexpr std::array<std::pair<Service, const char*>, 5> kServiceNames{{
    {ServiceStorage, "storage"},
    {ServiceCamera, "camera"},
    {ServiceScreen, "screen"},
    {ServiceControl, "control"},
    {ServiceDisplay, "display"},
}};

constexpr qsizetype kMaxNameLength = 64;

}  // namespace

QStringList serviceNames(uint32_t services) {
    QStringList out;
    for (const auto& [bit, name] : kServiceNames)
        if (services & bit) out << QString::fromLatin1(name);
    return out;
}

uint32_t servicesFromNames(const QStringList& names) {
    uint32_t out = ServiceNone;
    for (const auto& [bit, name] : kServiceNames)
        if (names.contains(QString::fromLatin1(name))) out |= bit;
    return out;
}

std::vector<uint8_t> encodeControl(const QJsonObject& message) {
    const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);

    std::vector<uint8_t> out(static_cast<size_t>(json.size()) + 1);
    out[0] = static_cast<uint8_t>(MessageKind::Control);
    std::memcpy(out.data() + 1, json.constData(), static_cast<size_t>(json.size()));
    return out;
}

std::optional<QJsonObject> decodeControl(std::span<const uint8_t> message) {
    if (message.empty() || message[0] != static_cast<uint8_t>(MessageKind::Control))
        return std::nullopt;
    const auto body = message.subspan(1);
    const auto doc  = QJsonDocument::fromJson(QByteArray::fromRawData(
        reinterpret_cast<const char*>(body.data()), static_cast<qsizetype>(body.size())));
    if (!doc.isObject()) return std::nullopt;
    return doc.object();
}

std::vector<uint8_t> Hello::encode() const {
    return encodeControl({
        {"t", "hello"},
        {"v", kVersion},
        {"name", name},
        {"os", os},
        {"svc", QJsonArray::fromStringList(serviceNames(services))},
    });
}

std::optional<Hello> Hello::decode(std::span<const uint8_t> message) {
    const auto parsed = decodeControl(message);
    if (!parsed) return std::nullopt;
    const QJsonObject& o = *parsed;
    if (o.value("t").toString() != QLatin1String("hello")) return std::nullopt;

    // The peer holds the topic key, but its claims about itself are still just
    // claims: keep them short enough that they cannot wreck the layout.
    Hello h;
    h.name = o.value("name").toString().left(kMaxNameLength);
    h.os   = o.value("os").toString().left(32);
    QStringList svc;
    for (const auto& v : o.value("svc").toArray()) svc << v.toString();
    h.services = servicesFromNames(svc);
    return h;
}

QString localOsName() {
    return QSysInfo::productType();
}

}  // namespace ucd
