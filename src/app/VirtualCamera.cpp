#include "app/VirtualCamera.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>

#include "app/vcam/VcamOutput.hpp"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace ucd {

namespace {

#ifdef Q_OS_WIN
// The component and its installer: next to the app in a package, or wherever
// UCONNECT_VCAM_DIR says (a development build).
QString componentDir() {
    const QString custom = qEnvironmentVariable("UCONNECT_VCAM_DIR");
    return custom.isEmpty() ? QCoreApplication::applicationDirPath() + "/vcam" : custom;
}
#endif

}  // namespace

VirtualCamera::VirtualCamera(QObject* parent) : QObject(parent) {
    refreshSupport();
    worker_ = std::thread([this] { run(); });
}

VirtualCamera::~VirtualCamera() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool VirtualCamera::canInstall() const {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void VirtualCamera::refreshSupport() {
    const vcam::Support s = vcam::support();
    supported_ = s.supported;
    installed_ = s.installed;
    hint_      = s.hint;
}

void VirtualCamera::start(const QString& peer, int rotation) {
    refreshSupport();
    if (!installed_) {
        emit changed();
        emit notify(hint_);
        return;
    }
    const bool restart = !peer_.isEmpty();
    peer_ = peer;
    {
        std::lock_guard lock(mutex_);
        rotation_    = rotation;
        wantRunning_ = true;
        hasFrame_    = false;
    }
    wake_.notify_all();
    if (!restart) live_ = false;
    emit changed();
}

void VirtualCamera::stop() {
    if (peer_.isEmpty()) return;
    peer_ = QString();
    {
        std::lock_guard lock(mutex_);
        wantRunning_ = false;
        hasFrame_    = false;
        frame_       = QVideoFrame();
    }
    wake_.notify_all();
    emit changed();
}

void VirtualCamera::setRotation(int rotation) {
    std::lock_guard lock(mutex_);
    rotation_ = rotation;
}

void VirtualCamera::push(const QString& peer, const QVideoFrame& frame) {
    if (peer != peer_ || !frame.isValid()) return;
    {
        std::lock_guard lock(mutex_);
        frame_    = frame;  // only the newest is worth converting
        hasFrame_ = true;
    }
    wake_.notify_all();
}

void VirtualCamera::setState(bool live, bool inUse) {
    if (live == live_ && inUse == inUse_) return;
    live_  = live;
    inUse_ = inUse;
    emit changed();
}

void VirtualCamera::run() {
    std::unique_ptr<vcam::Output> output;
    bool                          reportedInUse = false, reportedLive = false;
    const auto report = [&](bool live, bool inUse) {
        if (live == reportedLive && inUse == reportedInUse) return;
        reportedLive  = live;
        reportedInUse = inUse;
        QMetaObject::invokeMethod(this, [this, live, inUse] { setState(live, inUse); }, Qt::QueuedConnection);
    };

    std::unique_lock lock(mutex_);
    while (!quit_) {
        // Wake for work, or now and then to see whether an app still reads it.
        wake_.wait_for(lock, std::chrono::milliseconds(500),
                       [&] { return quit_ || wantRunning_ != bool(output) || (output && hasFrame_); });
        if (quit_) break;

        if (wantRunning_ && !output) {
            lock.unlock();
            auto    started = vcam::makeOutput();
            QString error   = started ? QString() : tr("The virtual camera is not available on this system.");
            if (started && !started->start(error)) started.reset();
            lock.lock();
            if (started) {
                output = std::move(started);
                report(true, false);
            } else {
                wantRunning_ = false;
                QMetaObject::invokeMethod(this, [this, error] {
                    peer_ = QString();
                    live_ = inUse_ = false;
                    emit changed();
                    emit notify(error);
                }, Qt::QueuedConnection);
            }
            continue;
        }
        if (!wantRunning_ && output) {
            lock.unlock();
            output->stop();
            output.reset();
            lock.lock();
            report(false, false);
            continue;
        }
        if (!output) continue;

        const QSize wanted = output->wanted();
        report(true, !wanted.isEmpty());
        if (!hasFrame_ || wanted.isEmpty()) {
            hasFrame_ = false;
            continue;
        }
        QVideoFrame frame    = frame_;
        const int   rotation = rotation_;
        hasFrame_            = false;
        lock.unlock();
        const QImage image = frame.toImage();
        if (!image.isNull()) output->write(vcam::fit(image, rotation, wanted));
        lock.lock();
    }
    if (output) {
        lock.unlock();
        output->stop();
    }
}

void VirtualCamera::install() { runInstaller(false); }

void VirtualCamera::uninstall() {
    stop();  // nothing may be using it
    runInstaller(true);
}

// Runs install-vcam.ps1 elevated. Asking Windows for that blocks until its
// prompt is up, so it all happens off the GUI thread.
void VirtualCamera::runInstaller(bool uninstall) {
#ifdef Q_OS_WIN
    if (!task_.isEmpty()) return;
    const QString dir    = componentDir();
    const QString dll    = QDir::toNativeSeparators(dir + "/uconnect-vcam.dll");
    QString       script = QDir::toNativeSeparators(dir + "/install-vcam.ps1");
    // Removing it works from the copy the installer leaves beside it, too.
    const QString installedScript = QDir::toNativeSeparators(
        qEnvironmentVariable("ProgramFiles", "C:/Program Files") + "/uConnect/VirtualCamera/install-vcam.ps1");
    if (uninstall && !QFileInfo::exists(script) && QFileInfo::exists(installedScript)) {
        // Run a copy: the script removes the folder it is kept in.
        script = QDir::toNativeSeparators(QDir::temp().filePath("uconnect-uninstall-vcam.ps1"));
        QFile::remove(script);
        QFile::copy(installedScript, script);
    }
    if (!QFileInfo::exists(script) || (!uninstall && !QFileInfo::exists(dll))) {
        emit notify(tr("This build has no uConnect Camera component (%1).").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    const QString arguments = uninstall
        ? QStringLiteral("-NoProfile -ExecutionPolicy Bypass -File \"%1\" -Uninstall").arg(script)
        : QStringLiteral("-NoProfile -ExecutionPolicy Bypass -File \"%1\" -Source \"%2\"").arg(script, dll);
    task_ = uninstall ? QStringLiteral("uninstall") : QStringLiteral("install");
    emit changed();

    QPointer<VirtualCamera> self(this);
    std::thread([self, uninstall, parameters = arguments.toStdWString()] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb       = L"runas";
        info.lpFile       = L"powershell.exe";
        info.lpParameters = parameters.c_str();
        info.nShow        = SW_HIDE;
        const bool  started = ShellExecuteExW(&info) && info.hProcess;
        const DWORD error   = started ? 0 : GetLastError();
        DWORD       code    = 1;
        if (started) {
            WaitForSingleObject(info.hProcess, INFINITE);
            GetExitCodeProcess(info.hProcess, &code);
            CloseHandle(info.hProcess);
        }
        CoUninitialize();
        QMetaObject::invokeMethod(qApp, [self, uninstall, started, error, code] {
            if (!self) return;
            self->task_.clear();
            self->refreshSupport();
            emit self->changed();
            if (!started) {
                if (error != ERROR_CANCELLED)
                    emit self->notify(tr("Could not start the installer (error %1).").arg(error));
            } else if (uninstall) {
                emit self->notify(!self->installed_ ? tr("The uConnect Camera was removed.")
                                                    : tr("Removing the uConnect Camera failed (code %1).").arg(code));
            } else {
                emit self->notify(self->installed_ ? tr("The uConnect Camera is installed.")
                                                   : tr("Installing the uConnect Camera failed (code %1).").arg(code));
            }
        });
    }).detach();
#else
    Q_UNUSED(uninstall);
#endif
}

}  // namespace ucd
