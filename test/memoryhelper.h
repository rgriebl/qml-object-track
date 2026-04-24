#pragma once

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#include <malloc.h>
#include <unistd.h>

#include <QObject>
#include <QtQml/qqml.h>
#include <QtDebug>

class MemoryHelper : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit MemoryHelper(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void trimHeap()
    {
        ::malloc_trim(0);
    }
};
