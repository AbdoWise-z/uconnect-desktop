#pragma once
// Network rules: per-topic permission defaults chosen by where a device's
// direct connection comes from (docs/protocol.md section 3.4).
//
// A rule's pattern has four dot-separated parts, one per IPv4 byte:
//   x  any value            m  the same as this device's own address
//   0-255  exactly that value
// so "m.m.m.x" is "my /24", "100.x.x.x" is anything starting 100, and
// "x.x.x.x" is any address. The first rule that matches a proven device
// decides what it may do here, unless the device has special rules of its own
// (see AccessPolicy).

#include <QHostAddress>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ucd {

struct NetworkRule {
    QString     pattern;
    QString     label;
    QStringList permissions;

    bool operator==(const NetworkRule&) const = default;
};

constexpr int kMaxNetworkRules = 64;

// Why `pattern` is not a valid rule pattern, or empty if it is.
QString networkPatternProblem(const QString& pattern);

// Whether `peer` matches `pattern`, comparing "m" parts with each of `mine`
// (this device's IPv4 addresses) in turn. "x.x.x.x" matches any address,
// IPv6 included; other patterns only match IPv4 (or IPv4-mapped) addresses.
bool networkPatternMatches(const QString& pattern, const QHostAddress& peer, const QList<QHostAddress>& mine);

// The index of the first rule `peer` matches, or -1.
int firstMatchingRule(const QVector<NetworkRule>& rules, const QHostAddress& peer, const QList<QHostAddress>& mine);

// Wire / settings form: [{"pattern","label","perms":[...]}].
// fromJson drops invalid rules and unknown permissions and keeps at most
// kMaxNetworkRules.
QJsonArray            networkRulesToJson(const QVector<NetworkRule>& rules);
QVector<NetworkRule>  networkRulesFromJson(const QJsonArray& array);

}  // namespace ucd
