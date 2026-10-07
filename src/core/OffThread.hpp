#pragma once

#include <memory>
#include <optional>

#include <QFutureWatcher>
#include <QObject>
#include <QPromise>
#include <QThreadPool>

namespace ucd {

// Runs `work` on a pool thread and `done` with its result (nullopt if it never
// ran) back on `context`'s thread; nothing runs once `context` is gone.
template <class T, class Work, class Done>
void runOffThread(QObject* context, Work work, Done done) {
    auto  promise = std::make_shared<QPromise<T>>();
    auto* watcher = new QFutureWatcher<T>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, context, [watcher, done] {
        std::optional<T> result;
        if (watcher->future().resultCount() > 0) result = watcher->result();
        watcher->deleteLater();
        done(std::move(result));
    });
    watcher->setFuture(promise->future());
    promise->start();
    QThreadPool::globalInstance()->start([promise, work] {
        promise->addResult(work());
        promise->finish();
    });
}

}  // namespace ucd
