#include "NetworkRules.hpp"

#include <QJsonObject>
#include <array>
#include <optional>

#include "Permissions.hpp"

namespace ucd {
namespace {

// The pattern's parts: -1 for x, -2 for m, else the byte value.
std::optional<std::array<int, 4>> parse(const QString& pattern) {
    const QStringList parts = pattern.trimmed().toLower().split(QLatin1Char('.'));
    if (parts.size() != 4) return std::nullopt;
    std::array<int, 4> out{};
    for (int i = 0; i < 4; ++i) {
        const QString& p = parts[i];
        if (p == QLatin1String("x") || p == QLatin1String("*")) {
            out[i] = -1;
        } else if (p == QLatin1String("m")) {
            out[i] = -2;
        } else {
            if (p.isEmpty() || p.size() > 3) return std::nullopt;
            bool ok = false;
            const int v = p.toInt(&ok);
            if (!ok || v < 0 || v > 255) return std::nullopt;
            for (const QChar c : p)
                if (!c.isDigit()) return std::nullopt;
            out[i] = v;
        }
    }
    return out;
}

std::optional<quint32> ipv4(const QHostAddress& a) {
    bool ok = false;
    const quint32 v = a.toIPv4Address(&ok);  // also unwraps ::ffff:a.b.c.d
    return ok ? std::optional<quint32>(v) : std::nullopt;
}

int byteOf(quint32 v, int i) { return int((v >> (24 - 8 * i)) & 0xff); }

}  // namespace

QString networkPatternProblem(const QString& pattern) {
    const QString p = pattern.trimmed();
    if (p.isEmpty()) return QStringLiteral("Enter a pattern such as m.m.m.x");
    if (p.count(QLatin1Char('.')) != 3) return QStringLiteral("Use four parts separated by dots, like m.m.m.x");
    if (!parse(p)) return QStringLiteral("Each part must be x, m or a number from 0 to 255");
    return {};
}

bool networkPatternMatches(const QString& pattern, const QHostAddress& peer, const QList<QHostAddress>& mine) {
    const auto parts = parse(pattern);
    if (!parts || peer.isNull()) return false;
    const bool anyAddress = (*parts)[0] == -1 && (*parts)[1] == -1 && (*parts)[2] == -1 && (*parts)[3] == -1;
    if (anyAddress) return true;
    const auto p = ipv4(peer);
    if (!p) return false;

    bool usesMine = false;
    for (int i = 0; i < 4; ++i) {
        const int want = (*parts)[i];
        if (want == -2) usesMine = true;
        else if (want >= 0 && byteOf(*p, i) != want) return false;
    }
    if (!usesMine) return true;
    for (const QHostAddress& a : mine) {
        const auto m = ipv4(a);
        if (!m) continue;
        bool all = true;
        for (int i = 0; i < 4 && all; ++i)
            if ((*parts)[i] == -2 && byteOf(*p, i) != byteOf(*m, i)) all = false;
        if (all) return true;
    }
    return false;
}

int firstMatchingRule(const QVector<NetworkRule>& rules, const QHostAddress& peer, const QList<QHostAddress>& mine) {
    for (int i = 0; i < rules.size(); ++i)
        if (networkPatternMatches(rules[i].pattern, peer, mine)) return i;
    return -1;
}

QJsonArray networkRulesToJson(const QVector<NetworkRule>& rules) {
    QJsonArray out;
    for (const NetworkRule& r : rules) {
        out.append(QJsonObject{{"pattern", r.pattern},
                               {"label", r.label},
                               {"perms", QJsonArray::fromStringList(r.permissions)}});
    }
    return out;
}

QVector<NetworkRule> networkRulesFromJson(const QJsonArray& array) {
    QVector<NetworkRule> out;
    for (const QJsonValue& v : array) {
        if (out.size() >= kMaxNetworkRules) break;
        const QJsonObject o = v.toObject();
        NetworkRule r;
        r.pattern = o.value("pattern").toString().trimmed().toLower();
        if (!networkPatternProblem(r.pattern).isEmpty()) continue;
        r.label = o.value("label").toString().left(64);
        for (const QJsonValue& p : o.value("perms").toArray()) {
            const QString id = p.toString();
            if (isKnownPermission(id) && !r.permissions.contains(id)) r.permissions << id;
        }
        r.permissions.sort();
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace ucd
