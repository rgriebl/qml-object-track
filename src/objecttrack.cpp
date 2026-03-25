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
#include <unordered_map>

enum TrackType {
    Create = 0,
    Finalize,
    Delegate
};

static void objectTrack(const QByteArray &url, bool startOfCreation, TrackType=Create,
                        const QByteArray &parentUrl = QByteArray());
static size_t measureHeapSize();


typedef void *(*beginCreate_fn_t)(void *self, void *context);
typedef void *(*completeCreate_fn_t)(void *self);
typedef QUrl  (*url_fn_t)(const void *self);
typedef void *(*objectCreator_create_fn_t)(void *self, int subComponentIndex, void *parent,
                                           void *interrupt, int flags);
// QQmlContextData::initFromTypeCompilationUnit — called inside create(), sets URL on context
typedef void  (*initFromType_fn_t)(void *self, const void *unit, int subComponentIndex);
// QQmlObjectCreator::finalize — runs bindings and componentComplete, can allocate
typedef bool  (*objectCreator_finalize_fn_t)(void *self, void *interrupt);
// QQmlObjectCreator constructor — incubator != nullptr means delegate/async creation
typedef void  (*objectCreator_ctor_fn_t)(void *self, const void *parentContext,
                                         const void *compilationUnit,
                                         const void *creationContext,
                                         const void *inlineComponentName,
                                         void *incubator);


static beginCreate_fn_t             s_beginFn                = nullptr;
static completeCreate_fn_t          s_completeFn             = nullptr;
static url_fn_t                     s_urlFn                  = nullptr;
static url_fn_t                     s_contextUrlFn           = nullptr;
static objectCreator_create_fn_t    s_objectCreatorCreateFn  = nullptr;
static initFromType_fn_t            s_initFromTypeFn         = nullptr;
static objectCreator_finalize_fn_t  s_objectCreatorFinalizeFn = nullptr;
static objectCreator_ctor_fn_t      s_objectCreatorCtorFn    = nullptr;

// QQmlObjectCreator* → {isDelegate, parentUrl} from constructor
struct CreatorInfo {
    bool isDelegate;
    QByteArray parentUrl;
};
static std::mutex s_creatorInfoMutex;
static std::unordered_map<void *, CreatorInfo> s_creatorInfoMap;

// Map QQmlObjectCreator* → {URL, parentUrl}, set in create(), used in finalize()
struct CreatorUrlInfo {
    QByteArray url;
    QByteArray parentUrl;
};
static std::mutex s_creatorUrlMutex;
static std::unordered_map<void *, CreatorUrlInfo> s_creatorUrlMap;

// Thread-local stack: initFromTypeCompilationUnit pushes {url}, create() pops
struct CreationEntry {
    QByteArray url;
    TrackType type;
    QByteArray parentUrl;
};
static thread_local QStack<CreationEntry> tls_creationStack;
// Set by create() hook from constructor info
static thread_local bool tls_currentIsDelegate = false;
static thread_local QByteArray tls_currentParentUrl;

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

    // QQmlObjectCreator::finalize(QQmlInstantiationInterrupt &)
    *(void **) (&s_objectCreatorFinalizeFn) = dlsym(RTLD_NEXT,
        "_ZN17QQmlObjectCreator8finalizeER26QQmlInstantiationInterrupt");
    if (!s_objectCreatorFinalizeFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlObjectCreator::finalize: %s", dlerror());

    // QQmlObjectCreator constructor — check incubator parameter
    *(void **) (&s_objectCreatorCtorFn) = dlsym(RTLD_NEXT,
        "_ZN17QQmlObjectCreatorC1ERK14QQmlRefPointerI15QQmlContextDataE"
        "RKS0_IN3QV425ExecutableCompilationUnitEES4_RK7QStringP20QQmlIncubatorPrivate");
    if (!s_objectCreatorCtorFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlObjectCreator::QQmlObjectCreator: %s", dlerror());
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

// Hook for QQmlObjectCreator constructor — detect delegate creation via incubator parameter
[[gnu::visibility("default")]] void qml_objectCreator_ctor_hook(
    void *self, const void *parentContext, const void *compilationUnit,
    const void *creationContext, const void *inlineComponentName, void *incubator)
    __asm__("_ZN17QQmlObjectCreatorC1ERK14QQmlRefPointerI15QQmlContextDataE"
            "RKS0_IN3QV425ExecutableCompilationUnitEES4_RK7QStringP20QQmlIncubatorPrivate");

void qml_objectCreator_ctor_hook(
    void *self, const void *parentContext, const void *compilationUnit,
    const void *creationContext, const void *inlineComponentName, void *incubator)
{
    resolveFunctions();
    s_objectCreatorCtorFn(self, parentContext, compilationUnit,
                          creationContext, inlineComponentName, incubator);

    // Extract parent component URL from parentContext
    QByteArray parentUrl;
    void *contextDataPtr = *(void **)parentContext;
    if (contextDataPtr)
        parentUrl = contextUrl(contextDataPtr);

    std::lock_guard<std::mutex> lock(s_creatorInfoMutex);
    s_creatorInfoMap[self] = { incubator != nullptr, std::move(parentUrl) };
}

// Hook for QQmlContextData::initFromTypeCompilationUnit — called early inside create(),
// before createInstance() does the actual allocations. We capture URL + start heap here.
[[gnu::visibility("default")]] void qml_initFromType_hook(
    void *self, const void *unit, int subComponentIndex)
    __asm__("_ZN15QQmlContextData27initFromTypeCompilationUnitERK14QQmlRefPointerIN3QV425ExecutableCompilationUnitEEi");

void qml_initFromType_hook(void *self, const void *unit, int subComponentIndex)
{
    resolveFunctions();
    s_initFromTypeFn(self, unit, subComponentIndex);
    // After the real call, this QQmlContextData has the URL set — start tracking here
    QByteArray url = contextUrl(self);
    // subComponentIndex >= 0 means an inline sub-component (e.g. GridView delegate definition),
    // incubator != null (tls_currentIsDelegate) catches incubated/async delegate creation.
    TrackType type = (tls_currentIsDelegate || subComponentIndex >= 0) ? Delegate : Create;

    // Determine parent: if we're nested inside another creation, the stack top is the parent.
    // Only fall back to tls_currentParentUrl (from QQmlObjectCreator constructor) for top-level.
    QByteArray parentUrl;
    if (!tls_creationStack.isEmpty()) {
        parentUrl = tls_creationStack.top().url;
    } else if (tls_currentParentUrl != url) {
        // Use constructor's parentContext URL, but filter out self-references (base type creators)
        parentUrl = tls_currentParentUrl;
    }

    objectTrack(url, true, type, parentUrl);
    tls_creationStack.push({ std::move(url), type, std::move(parentUrl) });
}

// Hook for QQmlObjectCreator::create
[[gnu::visibility("default")]] void *qml_objectCreator_create_hook(
    void *self, int subComponentIndex, void *parent, void *interrupt, int flags)
    __asm__("_ZN17QQmlObjectCreator6createEiP7QObjectP26QQmlInstantiationInterrupti");

void *qml_objectCreator_create_hook(
    void *self, int subComponentIndex, void *parent, void *interrupt, int flags)
{
    resolveFunctions();

    // Get constructor info: delegate flag + parent URL
    {
        std::lock_guard<std::mutex> lock(s_creatorInfoMutex);
        auto it = s_creatorInfoMap.find(self);
        if (it != s_creatorInfoMap.end()) {
            tls_currentIsDelegate = it->second.isDelegate;
            tls_currentParentUrl = it->second.parentUrl;
            s_creatorInfoMap.erase(it);
        } else {
            tls_currentIsDelegate = false;
            tls_currentParentUrl.clear();
        }
    }

    int stackDepthBefore = tls_creationStack.size();
    void *result = s_objectCreatorCreateFn(self, subComponentIndex, parent, interrupt, flags);

    // initFromTypeCompilationUnit is only called when phase == Startup (normal path).
    // Phase CreatingObjectsPhase2 returns early without it — nothing to track.
    if (tls_creationStack.size() > stackDepthBefore) {
        auto entry = tls_creationStack.pop();
        objectTrack(entry.url, false, entry.type, entry.parentUrl);

        // Store URL + parentUrl for finalize() which runs later on the top-level QQmlObjectCreator
        std::lock_guard<std::mutex> lock(s_creatorUrlMutex);
        s_creatorUrlMap[self] = { entry.url, std::move(entry.parentUrl) };
    }

    return result;
}

// Hook for QQmlObjectCreator::finalize — runs bindings and componentComplete()
[[gnu::visibility("default")]] bool qml_objectCreator_finalize_hook(void *self, void *interrupt)
    __asm__("_ZN17QQmlObjectCreator8finalizeER26QQmlInstantiationInterrupt");

bool qml_objectCreator_finalize_hook(void *self, void *interrupt)
{
    resolveFunctions();

    QByteArray url;
    QByteArray parentUrl;
    {
        std::lock_guard<std::mutex> lock(s_creatorUrlMutex);
        auto it = s_creatorUrlMap.find(self);
        if (it != s_creatorUrlMap.end()) {
            url = std::move(it->second.url);
            parentUrl = std::move(it->second.parentUrl);
            s_creatorUrlMap.erase(it);
        }
    }

    if (!url.isEmpty()) {
        objectTrack(url, true, Finalize, parentUrl);
        // Push onto stack so components created during finalize get us as parent
        tls_creationStack.push({ url, Finalize, parentUrl });
        bool result = s_objectCreatorFinalizeFn(self, interrupt);
        tls_creationStack.pop();
        objectTrack(url, false, Finalize, parentUrl);
        return result;
    }

    return s_objectCreatorFinalizeFn(self, interrupt);
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

void objectTrack(const QByteArray &url, bool startOfCreation, TrackType trackType,
                 const QByteArray &parentUrl)
{
    // File-scope state
    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    static int s_nestingLevel = 0;
    static int s_startCount = 0; // cumulative calls with startOfFunction=true
    static QStack<std::tuple<QByteArray, size_t>> s_creationStack;

    const size_t heapSize = measureHeapSize();
    QByteArray heapSizeDeltaStr;

    if (startOfCreation) {
        ++s_nestingLevel;
        ++s_startCount;
        s_creationStack.push({ url, heapSize });
    } else {
        if (s_creationStack.isEmpty()) {
            qWarning() << "QML-OBJECT-TRACK: unexpected end for component at" << url;
        } else {
            auto [urlBegin, heapSizeBegin] = s_creationStack.pop();
            if (urlBegin != url)
                qWarning() << "QML-OBJECT-TRACK: mismatched end for component:" << url << "expected:" << urlBegin;
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

    const char *typeStr = trackType == Create ? "create" : trackType == Finalize ? "finalize" : "delegate";

    std::fprintf(s_csvFile,
                 "%s,%s,%s,%zu,%s,%d,%d,%s\n",
                 QByteArray(s_nestingLevel, startOfCreation ? '>' : '<').constData(),
                 typeStr,
                 url.constData(),
                 heapSize,
                 heapSizeDeltaStr.constData(),
                 s_nestingLevel,
                 s_startCount,
                 parentUrl.constData());
    std::fflush(s_csvFile);

    if (!startOfCreation)
        --s_nestingLevel;
}
