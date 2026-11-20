#pragma once
// macOS privacy grants (Screen Recording, Accessibility and the like) and app
// updates.
//
// macOS files each grant under the app's bundle identifier together with the
// code requirement of the build it was given to. For a build signed with a
// stable identity that requirement names the certificate and survives
// updates. For an ad-hoc signed build, which is what uConnect ships as, it is
// that one build's hash. After an update the entry is still there and System
// Settings still shows uConnect switched on, but it no longer matches the app:
// access is refused, and because an entry exists macOS never asks again. The
// way out by hand is `tccutil reset <service> <bundle id>` in a terminal.
//
// So the app does that itself: on a build's first launch it forgets the
// grants on record for another build, and macOS asks afresh the next time one
// is needed.

#include <QList>
#include <QString>
#include <QStringList>

namespace ucd::privacy {

struct Grant {
    QString service;    // as tccutil names it: ScreenCapture, Accessibility, ...
    bool checkable;     // whether the app can tell if it holds this one now
    bool granted;       // it does (meaningful only when checkable)
};

// Which of `grants` to forget, given the code requirement on record from the
// last launch (empty: none recorded yet) and this build's (empty: unsigned or
// unknown, and then nothing is forgotten).
//
//  - Same requirement: the same app as far as macOS is concerned. Nothing.
//  - Another build's on record: every grant that build was given is stale,
//    whether or not the app can check it. One the app can see it holds is
//    kept all the same.
//  - None on record (the first launch of a version that keeps one): only what
//    the app can see it lacks. Forgetting a grant that was never given is
//    harmless; forgetting a good one it cannot check would not be.
inline QStringList staleGrants(const QString& recorded, const QString& current, const QList<Grant>& grants) {
    QStringList stale;
    if (current.isEmpty() || recorded == current) return stale;
    const bool anotherBuild = !recorded.isEmpty();
    for (const Grant& grant : grants) {
        if (grant.checkable ? !grant.granted : anotherBuild) stale << grant.service;
    }
    return stale;
}

#ifdef Q_OS_MACOS
// Forgets this app's stale grants (see above) and records this build's code
// requirement for the next launch. Returns the services forgotten. Call once,
// early, before anything asks macOS for access. (app/PrivacyGrants_mac.mm)
QStringList forgetStaleGrants();
#endif

}  // namespace ucd::privacy
