// Device identities: standard Ed25519, stored keys, and the session-bound
// proof that stops a key holder's proof being replayed elsewhere.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include "core/Hello.hpp"
#include "core/Identity.hpp"

using namespace ucd;

namespace {

QByteArray hexBytes(const char* hex) {
    return QByteArray::fromHex(hex);
}

template <size_t N>
std::array<uint8_t, N> toArray(const QByteArray& b) {
    std::array<uint8_t, N> out{};
    std::copy(b.begin(), b.begin() + N, out.begin());
    return out;
}

ChannelBinding binding(uint8_t fill) {
    ChannelBinding b{};
    b.fill(fill);
    return b;
}

QJsonObject decoded(const std::vector<uint8_t>& message) {
    return decodeControl(message).value_or(QJsonObject{});
}

}  // namespace

class IdentityTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

private slots:
    // RFC 8032, section 7.1, TEST 1. Other implementations of the protocol
    // (e.g. the Flutter app) depend on this being plain Ed25519.
    void matchesRfc8032() {
        const QString path = dir_.filePath("rfc.json");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(QJsonObject{
                                  {"v", 1},
                                  {"type", "ed25519"},
                                  {"seed", "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"},
                              })
                    .toJson());
        f.close();

        QString        warning;
        const Identity id = Identity::loadOrCreate(path, &warning);
        QVERIFY2(warning.isEmpty(), qPrintable(warning));
        QCOMPARE(keyToHex(id.publicKey()),
                 QStringLiteral("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));

        const Signature sig = id.sign({});
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(sig.data()), 64),
                 hexBytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065"
                          "224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
        QVERIFY(verifySignature(id.publicKey(), {}, sig));
    }

    void rejectsTamperedSignatures() {
        const Identity             id  = Identity::generate();
        const std::vector<uint8_t> msg = {1, 2, 3};
        Signature                  sig = id.sign(msg);
        QVERIFY(verifySignature(id.publicKey(), msg, sig));

        const std::vector<uint8_t> other = {1, 2, 4};
        QVERIFY(!verifySignature(id.publicKey(), other, sig));
        QVERIFY(!verifySignature(Identity::generate().publicKey(), msg, sig));
        sig[10] ^= 1;
        QVERIFY(!verifySignature(id.publicKey(), msg, sig));
    }

    void proofIsBoundToItsSession() {
        const Identity id    = Identity::generate();
        const auto     proof = decoded(id.proof(binding(7)));

        const auto proven = verifyProof(proof, binding(7));
        QVERIFY(proven.has_value());
        QCOMPARE(*proven, id.publicKey());

        // The same proof replayed into a different session must not verify.
        QVERIFY(!verifyProof(proof, binding(8)).has_value());
    }

    void proofCannotClaimAnotherKey() {
        const Identity attacker = Identity::generate();
        const Identity victim   = Identity::generate();
        QJsonObject    proof    = decoded(attacker.proof(binding(1)));
        proof["key"]            = keyToHex(victim.publicKey());
        QVERIFY(!verifyProof(proof, binding(1)).has_value());
    }

    void rejectsMalformedProofs_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("wrong type") << "t" << QJsonValue("hello");
        QTest::newRow("wrong version") << "v" << QJsonValue(2);
        QTest::newRow("short key") << "key" << QJsonValue("abcd");
        QTest::newRow("non-hex key") << "key" << QJsonValue(QString(64, QLatin1Char('z')));
        QTest::newRow("short signature") << "sig" << QJsonValue("00");
        QTest::newRow("missing signature") << "sig" << QJsonValue();
    }

    void rejectsMalformedProofs() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QJsonObject proof = decoded(Identity::generate().proof(binding(3)));
        if (value.isUndefined() || value.isNull()) proof.remove(field);
        else proof[field] = value;
        QVERIFY(!verifyProof(proof, binding(3)).has_value());
    }

    void persistsAcrossRestarts() {
        const QString path = dir_.filePath("sub/identity.json");
        QString       warning;
        const auto    first = Identity::loadOrCreate(path, &warning);
        QVERIFY2(warning.isEmpty(), qPrintable(warning));
        QVERIFY(QFile::exists(path));
        const auto second = Identity::loadOrCreate(path, &warning);
        QCOMPARE(second.publicKey(), first.publicKey());
    }

    void movesUnusableFilesAside() {
        const QString path = dir_.filePath("broken.json");
        QFile         f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not json");
        f.close();

        QString warning;
        Identity::loadOrCreate(path, &warning);
        QVERIFY(!warning.isEmpty());
        QVERIFY(!QDir(dir_.path()).entryList({"broken.json.unusable-*"}).isEmpty());  // kept, not lost
        QVERIFY(QFile::exists(path));                                                   // and a fresh one made
    }

    void fingerprintFormat() {
        const auto key = Identity::generate().publicKey();
        const QString fp = fingerprint(key);
        QVERIFY2(QRegularExpression("^([0-9A-F]{4} ){7}[0-9A-F]{4}$").match(fp).hasMatch(), qPrintable(fp));
        QCOMPARE(fingerprint(key), fp);
        QCOMPARE(fingerprintOfHex(keyToHex(key)), fp);
        QVERIFY(fingerprintOfHex("nonsense").isEmpty());
    }
};

QTEST_GUILESS_MAIN(IdentityTest)
#include "IdentityTest.moc"
