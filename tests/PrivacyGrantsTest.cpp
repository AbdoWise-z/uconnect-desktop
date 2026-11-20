// Which macOS privacy grants a build forgets on its first launch
// (core/PrivacyGrants.hpp): those on record for another build, never one the
// app can see it holds.

#include <QtTest>

#include "core/PrivacyGrants.hpp"

using namespace ucd::privacy;

class PrivacyGrantsTest : public QObject {
    Q_OBJECT

private:
    const QString adhocA = QStringLiteral("cdhash H\"99c31686e7d7fe257ecbfe45b2c707702e5b6f8b\"");
    const QString adhocB = QStringLiteral("cdhash H\"0123456789abcdef0123456789abcdef01234567\"");
    const QString signedApp = QStringLiteral(
        "identifier \"dev.uconnect.desktop\" and certificate leaf = H\"9477ebf9ce2a02ac6253fcc2809840de09a3c2d4\"");

    static QList<Grant> grants(bool screen, bool accessibility) {
        return {
            {QStringLiteral("ScreenCapture"), true, screen},
            {QStringLiteral("Accessibility"), true, accessibility},
            {QStringLiteral("Camera"), false, false},
        };
    }

private slots:
    // The case this exists for: an update of an ad-hoc signed app. The entries
    // macOS holds name the old build; all of them go, checkable or not.
    void anUpdateForgetsEverythingTheOldBuildWasGiven() {
        QCOMPARE(staleGrants(adhocA, adhocB, grants(false, false)),
                 QStringList({"ScreenCapture", "Accessibility", "Camera"}));
    }

    // The same build again, or a rebuild signed with the same certificate: the
    // grants still match, and one the user refused is not asked for again.
    void theSameAppForgetsNothing() {
        QVERIFY(staleGrants(adhocA, adhocA, grants(false, false)).isEmpty());
        QVERIFY(staleGrants(signedApp, signedApp, grants(false, true)).isEmpty());
    }

    // A grant the app can see it holds is kept whatever is on record: it
    // cannot be stale.
    void aGrantThatWorksIsKept() {
        QCOMPARE(staleGrants(adhocA, adhocB, grants(true, false)), QStringList({"Accessibility", "Camera"}));
        QCOMPARE(staleGrants(adhocA, signedApp, grants(true, true)), QStringList({"Camera"}));
    }

    // The first launch of a version that keeps a record, coming from one that
    // did not: what the app can see it lacks may be stale and is forgotten;
    // what it cannot check is left alone.
    void withNothingOnRecordOnlyVisiblyMissingGrantsGo() {
        QCOMPARE(staleGrants(QString(), adhocB, grants(false, true)), QStringList({"ScreenCapture"}));
        QVERIFY(staleGrants(QString(), adhocB, grants(true, true)).isEmpty());
    }

    // Unsigned or unknown code: there is nothing to compare, so nothing goes.
    void withoutARequirementNothingIsForgotten() {
        QVERIFY(staleGrants(adhocA, QString(), grants(false, false)).isEmpty());
        QVERIFY(staleGrants(QString(), QString(), grants(false, false)).isEmpty());
    }
};

QTEST_GUILESS_MAIN(PrivacyGrantsTest)
#include "PrivacyGrantsTest.moc"
