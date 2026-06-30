#include "memoryhelper.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTimer>
#include <QEventLoop>

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#include <malloc.h>

// Spin the event loop briefly so queued deleteLater()/DeferredDelete events drain.
static void drainEvents(int ms = 200)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// QML_ENGINE_CHURN=1 : load the heavy repeater into a *throwaway* QQmlEngine and
// delete the whole engine on unload. Deleting the engine destroys its
// jsExpressionGuardPool (the 13 MB QRecyclePool), V4 heap, type/property caches
// etc. -- the in-process equivalent of a process teardown, scoped to one engine.
static int runEngineChurn()
{
    MemoryHelper mh;
    mh.dumpStats("baseline (no engine)");

    for (int cycle = 0; cycle < 2; ++cycle) {
        QQmlEngine *engine = new QQmlEngine;
        QQmlComponent comp(engine, QUrl("qrc:/qt/qml/ObjectTrackTest/repeater.qml"));
        if (comp.isError()) {
            qWarning() << "component error:" << comp.errorString();
            return 1;
        }
        QObject *root = comp.create();
        drainEvents();
        mh.dumpStats(QString("loaded in throwaway engine (cycle %1)").arg(cycle));

        delete root;        // destroy the object tree
        delete engine;      // <-- frees the per-engine guard pool + V4 heap
        drainEvents();
        mh.trimHeap();
        mh.dumpStats(QString("engine deleted + trim (cycle %1)").arg(cycle));
        mh.projectPageReclaim(QString("engine deleted + trim (cycle %1)").arg(cycle));
    }
    return 0;
}

int main(int argc, char *argv[])
{
    // mallopt(M_ARENA_MAX, 1);

    QGuiApplication app(argc, argv);

    if (qEnvironmentVariableIsSet("QML_ENGINE_CHURN"))
        return runEngineChurn();

    QQmlApplicationEngine engine;
    engine.loadFromModule("ObjectTrackTest", "Main");
    return app.exec();
}
