#pragma once
// One running app per settings profile. The app keeps running in the tray
// with its window closed, so launching it again must bring that one back
// rather than start a second device with the same identity.

#include <QObject>

class QLocalServer;

namespace ucd {

class SingleInstance : public QObject {
    Q_OBJECT
public:
    // Uses the application name, so set it (and the profile) first.
    explicit SingleInstance(QObject* parent = nullptr);

    // Another instance of this profile runs, and has been asked to show itself.
    bool forwarded() const { return forwarded_; }

signals:
    // A later launch asked this instance to show itself.
    void activated();

private:
    QLocalServer* server_ = nullptr;
    bool          forwarded_ = false;
};

}  // namespace ucd
