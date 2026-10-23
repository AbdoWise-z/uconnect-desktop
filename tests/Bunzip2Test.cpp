// The bzip2 decoder against streams made by Python's bz2 module
// (tests/data/bzip2: empty, text, byte values and runs, incompressible data,
// several blocks, concatenated streams), and damaged ones.
#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/Bunzip2.hpp"

class Bunzip2Test : public QObject {
    Q_OBJECT
private:
    static QByteArray read(const QString& name) {
        QFile file(QStringLiteral(UCD_TEST_DATA "/bzip2/") + name);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

private slots:
    void decodesPythonStreams_data() {
        QTest::addColumn<QString>("name");
        const QJsonObject expected = QJsonDocument::fromJson(read("expected.json")).object();
        QVERIFY(!expected.isEmpty());
        for (const QString& name : expected.keys()) QTest::newRow(qPrintable(name)) << name;
    }
    void decodesPythonStreams() {
        QFETCH(QString, name);
        const QJsonObject expected = QJsonDocument::fromJson(read("expected.json")).object().value(name).toObject();
        const QByteArray  compressed = read(name + ".bz2");
        QVERIFY(!compressed.isEmpty());
        QString    error;
        const auto out = ucd::bunzip2(compressed, 8 * 1024 * 1024, &error);
        QVERIFY2(out, qPrintable(error));
        QCOMPARE(out->size(), expected.value("size").toInteger());
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(*out, QCryptographicHash::Sha256).toHex()),
                 expected.value("sha256").toString());
    }

    void rejectsDamage() {
        QByteArray damaged = read("text.bz2");
        damaged[damaged.size() / 2] = char(damaged[damaged.size() / 2] ^ 0x10);
        QString error;
        QVERIFY(!ucd::bunzip2(damaged, 1 << 20, &error));
        QVERIFY(!error.isEmpty());

        QVERIFY(!ucd::bunzip2(read("multiblock.bz2").left(30000), 8 << 20, &error));
        QCOMPARE(error, QStringLiteral("truncated"));

        QVERIFY(!ucd::bunzip2("not bzip2 at all", 1 << 20, &error));
        QCOMPARE(error, QStringLiteral("not a bzip2 stream"));
    }

    void stopsAtTheSizeLimit() {
        QString error;
        QVERIFY(!ucd::bunzip2(read("multiblock.bz2"), 100000, &error));
        QCOMPARE(error, QStringLiteral("output too large"));
    }
};

QTEST_GUILESS_MAIN(Bunzip2Test)
#include "Bunzip2Test.moc"
