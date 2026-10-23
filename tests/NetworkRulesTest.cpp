// Network rules: the x / m / number pattern language, first-match order, and
// the JSON form exchanged with other devices (rules.list).

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "core/NetworkRules.hpp"

using namespace ucd;

class NetworkRulesTest : public QObject {
    Q_OBJECT

private:
    static QHostAddress ip(const char* s) { return QHostAddress(QString::fromLatin1(s)); }
    const QList<QHostAddress> mine{ip("192.168.1.20"), ip("10.0.5.7")};

private slots:
    void validatesPatterns() {
        QVERIFY(networkPatternProblem("m.m.m.x").isEmpty());
        QVERIFY(networkPatternProblem("x.x.x.x").isEmpty());
        QVERIFY(networkPatternProblem("100.x.x.x").isEmpty());
        QVERIFY(networkPatternProblem(" M.M.0.* ").isEmpty());  // case and * are tolerated
        QVERIFY(!networkPatternProblem("").isEmpty());
        QVERIFY(!networkPatternProblem("m.m.m").isEmpty());
        QVERIFY(!networkPatternProblem("m.m.m.x.x").isEmpty());
        QVERIFY(!networkPatternProblem("256.x.x.x").isEmpty());
        QVERIFY(!networkPatternProblem("1a.x.x.x").isEmpty());
        QVERIFY(!networkPatternProblem("-1.x.x.x").isEmpty());
        QVERIFY(!networkPatternProblem("m..m.x").isEmpty());
    }

    void matchesMineByteForByte() {
        QVERIFY(networkPatternMatches("m.m.m.x", ip("192.168.1.99"), mine));
        QVERIFY(networkPatternMatches("m.m.m.x", ip("10.0.5.200"), mine));  // the other address
        QVERIFY(!networkPatternMatches("m.m.m.x", ip("192.168.2.99"), mine));
        QVERIFY(networkPatternMatches("m.m.x.x", ip("192.168.2.99"), mine));
        // Every "m" compares with the same one of my addresses.
        QVERIFY(!networkPatternMatches("m.m.m.x", ip("192.0.5.1"), mine));
        QVERIFY(!networkPatternMatches("m.x.x.x", ip("172.16.0.1"), {}));  // no address of mine
    }

    void matchesLiteralsAndAnything() {
        QVERIFY(networkPatternMatches("100.x.x.x", ip("100.64.3.2"), mine));
        QVERIFY(!networkPatternMatches("100.x.x.x", ip("10.64.3.2"), mine));
        QVERIFY(networkPatternMatches("x.x.x.x", ip("8.8.8.8"), mine));
        QVERIFY(networkPatternMatches("x.x.x.x", ip("2001:db8::1"), mine));  // any address at all
        QVERIFY(!networkPatternMatches("m.m.m.x", ip("2001:db8::1"), mine));
        QVERIFY(networkPatternMatches("m.m.m.x", ip("::ffff:192.168.1.50"), mine));  // mapped IPv4
        QVERIFY(!networkPatternMatches("x.x.x.x", QHostAddress(), mine));  // unknown address
    }

    void firstMatchWins() {
        const QVector<NetworkRule> rules{
            {"m.m.m.x", "Home", {"storage.observe"}},
            {"x.x.x.x", "Anywhere", {}},
        };
        QCOMPARE(firstMatchingRule(rules, ip("192.168.1.4"), mine), 0);
        QCOMPARE(firstMatchingRule(rules, ip("8.8.8.8"), mine), 1);
        QCOMPARE(firstMatchingRule({}, ip("8.8.8.8"), mine), -1);
    }

    void jsonRoundTripsAndDropsTheInvalid() {
        const QVector<NetworkRule> rules{{"m.m.m.x", "Home", {"camera.view", "storage.observe"}}};
        QCOMPARE(networkRulesFromJson(networkRulesToJson(rules)), rules);

        QJsonArray in;
        in.append(QJsonObject{{"pattern", "bogus"}, {"perms", QJsonArray{"storage.read"}}});
        in.append(QJsonObject{{"pattern", "X.X.X.X"}, {"perms", QJsonArray{"no.such", "storage.read", "storage.read"}}});
        const auto got = networkRulesFromJson(in);
        QCOMPARE(got.size(), 1);
        QCOMPARE(got[0].pattern, QStringLiteral("x.x.x.x"));
        QCOMPARE(got[0].permissions, QStringList{"storage.read"});

        QJsonArray many;
        for (int i = 0; i < kMaxNetworkRules + 10; ++i) many.append(QJsonObject{{"pattern", "x.x.x.x"}});
        QCOMPARE(networkRulesFromJson(many).size(), kMaxNetworkRules);
    }
};

QTEST_GUILESS_MAIN(NetworkRulesTest)
#include "NetworkRulesTest.moc"
