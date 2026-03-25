// Copyright (C) 2026 Robert Griebl
// SPDX-License-Identifier: MIT

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <QtCore/QXmlStreamReader>
#include <QtCore/QDebug>
#include <QtCore/QUrl>
#include <QtCore/QStack>

#include <dlfcn.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <mutex>

static void objectTrack(void *qmlComponentPtr, const QByteArray &url, bool startOfCreation,
                        size_t preMeasuredHeap = 0);
static size_t measureHeapSize();


typedef void *(*beginCreate_fn_t)(void *self, void *context);
typedef void *(*completeCreate_fn_t)(void *self);
typedef QUrl  (*url_fn_t)(const void *self);
typedef void *(*objectCreator_create_fn_t)(void *self, int subComponentIndex, void *parent,
                                           void *interrupt, int flags);
// QQmlContextData::initFromTypeCompilationUnit — called inside create(), sets URL on context
typedef void  (*initFromType_fn_t)(void *self, const void *unit, int subComponentIndex);


static beginCreate_fn_t          s_beginFn                = nullptr;
static completeCreate_fn_t       s_completeFn             = nullptr;
static url_fn_t                  s_urlFn                  = nullptr;
static url_fn_t                  s_contextUrlFn           = nullptr;
static objectCreator_create_fn_t s_objectCreatorCreateFn  = nullptr;
static initFromType_fn_t         s_initFromTypeFn         = nullptr;

// Thread-local stack: initFromTypeCompilationUnit pushes {url, startHeap}, create() pops
struct CreationEntry {
    QByteArray url;
    size_t startHeapSize;
};
static thread_local QStack<CreationEntry> tls_creationStack;

static void resolveFunctions()
{
    if (s_beginFn) [[likely]]
        return;

    *(void **) (&s_beginFn) = dlsym(RTLD_NEXT, "_ZN13QQmlComponent11beginCreateEP11QQmlContext");
    if (!s_beginFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlComponent::beginCreate: %s", dlerror());

    *(void **) (&s_completeFn) = dlsym(RTLD_NEXT, "_ZN13QQmlComponent14completeCreateEv");
    if (!s_completeFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlComponent::completeCreate: %s", dlerror());

    *(void **) (&s_urlFn) = dlsym(RTLD_NEXT, "_ZNK13QQmlComponent3urlEv");
    if (!s_urlFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlComponent::url: %s", dlerror());

    // QQmlContextData::url() const — same calling convention as QQmlComponent::url()
    *(void **) (&s_contextUrlFn) = dlsym(RTLD_NEXT, "_ZNK15QQmlContextData3urlEv");
    if (!s_contextUrlFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlContextData::url: %s", dlerror());

    *(void **) (&s_objectCreatorCreateFn) = dlsym(RTLD_NEXT,
        "_ZN17QQmlObjectCreator6createEiP7QObjectP26QQmlInstantiationInterrupti");
    if (!s_objectCreatorCreateFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlObjectCreator::create: %s", dlerror());

    // QQmlContextData::initFromTypeCompilationUnit — called inside create(), sets URL on context
    *(void **) (&s_initFromTypeFn) = dlsym(RTLD_NEXT,
        "_ZN15QQmlContextData27initFromTypeCompilationUnitERK14QQmlRefPointerIN3QV425ExecutableCompilationUnitEEi");
    if (!s_initFromTypeFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlContextData::initFromTypeCompilationUnit: %s", dlerror());
}

static QByteArray componentUrl(void *self)
{
    QUrl url = s_urlFn(self);
    return url.toString().toUtf8();
}

static QByteArray contextUrl(const void *contextData)
{
    QUrl url = s_contextUrlFn(contextData);
    return url.toString().toUtf8();
}


// // Hook for QQmlComponent::completeCreate(), Itanium C++ ABI
// [[gnu::visibility("default")]] void *qml_beginCreate_hook(void *self, void *context)
//     __asm__("_ZN13QQmlComponent11beginCreateEP11QQmlContext");

// void *qml_beginCreate_hook(void *self, void *context)
// {
//     resolveFunctions();
//     objectTrack(self, componentUrl(self), true);
//     return s_beginFn(self, context);
// }

// // Hook for QQmlComponent::completeCreate(), Itanium C++ ABI
// [[gnu::visibility("default")]] void qml_completeCreate_hook(void *self)
//     __asm__("_ZN13QQmlComponent14completeCreateEv");

// void qml_completeCreate_hook(void *self)
// {
//     resolveFunctions();
//     s_completeFn(self);
//     objectTrack(self, componentUrl(self), false);
// }

// Hook for QQmlContextData::initFromTypeCompilationUnit — called early inside create(),
// before createInstance() does the actual allocations. We capture URL + start heap here.
[[gnu::visibility("default")]] void qml_initFromType_hook(
    void *self, const void *unit, int subComponentIndex)
    __asm__("_ZN15QQmlContextData27initFromTypeCompilationUnitERK14QQmlRefPointerIN3QV425ExecutableCompilationUnitEEi");

void qml_initFromType_hook(void *self, const void *unit, int subComponentIndex)
{
    resolveFunctions();
    s_initFromTypeFn(self, unit, subComponentIndex);
    // After the real call, this QQmlContextData has the URL set
    tls_creationStack.push({ contextUrl(self), measureHeapSize() });
}

// Hook for QQmlObjectCreator::create
[[gnu::visibility("default")]] void *qml_objectCreator_create_hook(
    void *self, int subComponentIndex, void *parent, void *interrupt, int flags)
    __asm__("_ZN17QQmlObjectCreator6createEiP7QObjectP26QQmlInstantiationInterrupti");

void *qml_objectCreator_create_hook(
    void *self, int subComponentIndex, void *parent, void *interrupt, int flags)
{
    resolveFunctions();

    int stackDepthBefore = tls_creationStack.size();
    void *result = s_objectCreatorCreateFn(self, subComponentIndex, parent, interrupt, flags);

    // initFromTypeCompilationUnit is only called when phase == Startup (normal path).
    // Phase CreatingObjectsPhase2 returns early without it — nothing to track.
    if (tls_creationStack.size() > stackDepthBefore) {
        auto entry = tls_creationStack.pop();
        objectTrack(self, entry.url, true, entry.startHeapSize);
        objectTrack(self, entry.url, false);
    }

    return result;
}


// ── Heap measurement ────────────────────────────────────────────────────

// Call malloc_info(3) into an in-memory buffer, parse the XML with
// QXmlStreamReader, and return the total current heap size in bytes.
static size_t measureHeapSize()
{
    char *buf  = nullptr;
    size_t size = 0;

    auto memf = ::open_memstream(&buf, &size);
    if (!memf)
        return 0;

    ::malloc_info(0, memf);
    ::fclose(memf); // flush and finalize buf

    if (!buf)
        return 0;

    auto data = QByteArray::fromRawData(buf, static_cast<int>(size));

    size_t totalHeapSize = 0;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement &&
                xml.name() == QLatin1String("system") &&
                xml.attributes().value(QLatin1String("type")) == QLatin1String("current")) {
            // we only need the last entry (total)
            totalHeapSize = xml.attributes().value(QLatin1String("size")).toULongLong();
        }
    }

    qWarning() << "QML-OBJECT-TRACK: current heap size:" << totalHeapSize;

    ::free(buf);
    return totalHeapSize;
}

void objectTrack(void *qmlComponentPtr, const QByteArray &url, bool startOfCreation,
                 size_t preMeasuredHeap)
{
    // File-scope state
    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    static int s_nestingLevel = 0;
    static int s_startCount = 0; // cumulative calls with startOfFunction=true
    static QStack<std::tuple<void *, size_t>> s_creationStack;

    // Use pre-measured heap if provided, otherwise measure now
    const size_t heapSize = preMeasuredHeap ? preMeasuredHeap : measureHeapSize();
    QByteArray heapSizeDeltaStr;

    if (startOfCreation) {
        ++s_nestingLevel;
        ++s_startCount;
        s_creationStack.push({ qmlComponentPtr, heapSize });
    } else {
        if (s_creationStack.isEmpty()) {
            qWarning() << "QML-OBJECT-TRACK: unexpected completeCreate for component at" << url;
        } else {
            auto [qmlComponentPtrBegin, heapSizeBegin] = s_creationStack.pop();
            if (qmlComponentPtrBegin != qmlComponentPtr)
                qWarning() << "QML-OBJECT-TRACK: mismatched completeCreate for component at" << url;
            else
                heapSizeDeltaStr = QByteArray::number(heapSize - heapSizeBegin);
        }
    }
    // Lazy-open the CSV file on the first write
    static FILE *s_csvFile = [] {
        char filename[64];
        std::snprintf(filename, sizeof(filename), "/tmp/qml-object-track.%d.csv", getpid());
        auto f = std::fopen(filename, "w");
        if (f)
            qWarning() << "QML-OBJECT-TRACK: logging into" << filename;
        else
            qFatal() << "QML-OBJECT-TRACK: could not open" << filename << "for writing";
        return f;
    }();

    std::fprintf(s_csvFile,
                 "%s,%s,%zu,%s,%d,%d\n",
                 QByteArray(s_nestingLevel, startOfCreation ? '>' : '<').constData(),
                 url.constData(),
                 heapSize,
                 heapSizeDeltaStr.constData(),
                 s_nestingLevel,
                 s_startCount);
    std::fflush(s_csvFile);

    if (!startOfCreation)
        --s_nestingLevel;
}
