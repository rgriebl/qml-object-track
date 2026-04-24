#include "memoryhelper.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#include <malloc.h>

int main(int argc, char *argv[])
{
    // mallopt(M_ARENA_MAX, 1);

    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    engine.loadFromModule("ObjectTrackTest", "Main");
    return app.exec();
}
