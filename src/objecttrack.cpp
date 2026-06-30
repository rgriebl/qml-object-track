// Copyright (C) 2026 Robert Griebl
// SPDX-License-Identifier: MIT

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <QtCore/QDebug>
#include <QtCore/qobject.h>
#include <QtCore/QUrl>
#include <QtCore/QStack>
#include <QtCore/QAnyStringView>
#include <QtCore/QCoreApplication>

#include <dlfcn.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <mutex>
#include <unordered_map>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <cstring>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>

enum TrackType {
    Create = 0,
    Finalize,
    Delegate,
    Destroy,
    DelegateDestroy, // delegate object destroyed via QObject::destroyed signal
    ComponentCreate  // fallback: QQmlObjectCreator hooks bypassed (direct intra-SO calls)
};

static void objectTrack(const QByteArray &url, bool startOfCreation, TrackType=Create,
                        const QByteArray &parentUrl = QByteArray(),
                        size_t precomputedHeapSize = 0);
static size_t measureHeapSize();


typedef void *(*beginCreate_fn_t)(void *self, void *context);
typedef void *(*completeCreate_fn_t)(void *self);
typedef QUrl  (*url_fn_t)(const void *self);
typedef void *(*objectCreator_create_fn_t)(void *self, int subComponentIndex, void *parent,
                                           void *interrupt, int flags);
// QQmlComponent::create(QQmlIncubator &, QQmlContext *, QQmlContext *)
// Called via PLT from libQt6Quick.so — works even when QQmlObjectCreator hooks are bypassed
typedef void  (*componentCreate_incubated_fn_t)(void *self, void *incubator,
                                                void *context, void *forContext);
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
// QQmlContextData::emitDestruction — fires when component tree is torn down
typedef void  (*emitDestruction_fn_t)(void *self);
// QQmlComponent::creationContext() const — PLT call from libQt6QmlModels.so, fires before each delegate
typedef void *(*creationContext_fn_t)(const void *self);
// QQmlIncubator::QQmlIncubator(IncubationMode) — PLT call when delegate incubator is constructed
typedef void  (*incubatorCtor_fn_t)(void *self, int mode);
// QQmlIncubator::clear() — PLT call from libQt6QmlModels.so after delegate incubation completes
typedef void  (*incubatorClear_fn_t)(void *self);
// QQmlIncubator::object() const — returns the created QObject (non-null when status==Ready)
typedef QObject *(*incubatorObject_fn_t)(const void *self);
// qmlContext(QObject*) — free function, returns QQmlContext* for the object's innermost context
typedef void *(*qmlContext_fn_t)(const QObject *obj);
// QQmlContext::baseUrl() const — returns QUrl of the context's component file
typedef QUrl (*contextBaseUrl_fn_t)(const void *self);
// QQmlApplicationEngine::load(const QUrl &) — called by the application via PLT, so we can hook it
typedef void (*engineLoad_fn_t)(void *self, const QUrl &url);
// QQmlComponent::create(QQmlContext*) — synchronous non-incubated create, called from user code
// with QQmlEngine. QQmlApplicationEngine calls this intra-SO so the PLT hook won't double-fire.
typedef QObject *(*componentCreateSync_fn_t)(void *self, void *context);
// QQmlApplicationEngine::rootObjects() const — returns list of root QObjects created by load().
typedef QList<QObject*> (*rootObjects_fn_t)(const void *self);
// QQmlApplicationEngine::loadFromModule(QAnyStringView, QAnyStringView) — the modern entry point
// (loadFromModule does NOT call load(QUrl), so the root QML file is otherwise untracked).
typedef void (*loadFromModule_fn_t)(void *self, QAnyStringView uri, QAnyStringView typeName);


static beginCreate_fn_t             s_beginFn                = nullptr;
static completeCreate_fn_t          s_completeFn             = nullptr;
static url_fn_t                     s_urlFn                  = nullptr;
static url_fn_t                     s_contextUrlFn           = nullptr;
static objectCreator_create_fn_t    s_objectCreatorCreateFn  = nullptr;
static initFromType_fn_t            s_initFromTypeFn         = nullptr;
static objectCreator_finalize_fn_t      s_objectCreatorFinalizeFn  = nullptr;
static objectCreator_ctor_fn_t          s_objectCreatorCtorFn      = nullptr;
static emitDestruction_fn_t             s_emitDestructionFn        = nullptr;
static componentCreate_incubated_fn_t   s_componentCreateFn        = nullptr;
static creationContext_fn_t             s_creationContextFn        = nullptr;
static incubatorCtor_fn_t               s_incubatorCtorFn          = nullptr;
static incubatorClear_fn_t              s_incubatorClearFn         = nullptr;
static incubatorObject_fn_t             s_incubatorObjectFn        = nullptr;
static qmlContext_fn_t                  s_qmlContextFn             = nullptr;
static contextBaseUrl_fn_t              s_contextBaseUrlFn         = nullptr;
static engineLoad_fn_t                  s_engineLoadFn             = nullptr;
static componentCreateSync_fn_t         s_componentCreateSyncFn    = nullptr;
static rootObjects_fn_t                 s_rootObjectsFn            = nullptr;
static loadFromModule_fn_t              s_loadFromModuleFn         = nullptr;

// QQmlObjectCreator destructor — the bracket end for per-component sizing.
typedef void (*objectCreatorDtor_fn_t)(void *self);
static objectCreatorDtor_fn_t           s_objectCreatorDtorFn      = nullptr;

// Diagnostics: per-hook invocation counters (printed at report time when the
// QML_OBJECT_TRACK_DEBUG env var is set). Used to see which creation/teardown
// hooks are actually reachable on a given Qt build.
enum HookId {
    H_initFromType, H_objCreate, H_objCtor, H_objDtor, H_finalize, H_emitDestroy,
    H_creationCtx, H_incubatorClear, H_componentCreate, H_componentSync,
    H_loadModule, H_COUNT
};
static std::atomic<long> s_hookCounts[H_COUNT];
static const char *s_hookNames[H_COUNT] = {
    "initFromType", "objectCreator::create", "objectCreator::ctor",
    "objectCreator::dtor", "objectCreator::finalize", "emitDestruction",
    "creationContext", "incubator::clear", "component::create(incubator)",
    "component::create(ctx)", "loadFromModule"
};

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
// Set by initFromType hook; cleared at entry of the QQmlComponent::create(incubator) hook
// so the component-level hook can detect if fine-grained tracking fired
static thread_local bool tls_initFromTypeFired = false;

// Delegate tracking for Qt builds where QQmlObjectCreator PLT hooks are bypassed:
// creationContext() fires before each delegate → records URL + heap snapshot.
// QQmlIncubatorC2 constructor fires next → moves the snapshot into a per-incubator map.
// QQmlComponent::create(incubator) fires for Loaders (not delegates) → erases the map entry.
// QQmlIncubator::clear() fires after delegate creation → consumes the map entry.
struct DelegateTrack { QByteArray url; QByteArray parentUrl; size_t heapBefore; bool initClearFired = false; };
// Depth of non-empty-URL component contexts currently being destroyed on this thread.
// Used to distinguish the engine root context (depth==0, also empty URL) from per-delegate
// wrapper contexts (empty URL, but nested inside a real component context at depth>0).
static thread_local int  tls_contextDestroyDepth = 0;
// Set while recursing inside a per-delegate wrapper context so that child style contexts
// (e.g. Fusion Button.qml) are suppressed. Saved/restored on each hook entry.
static thread_local bool tls_inDelegateContextDestruction = false;
// Saved by qml_incubatorClear_hook when the Loader's incubator completes (not a delegate model).
// The Loader calls clear() inside statusChanged(Ready) which nulls the incubator result before
// qml_component_create_hook can read it, so we save the pointer here for pickup after create().
static thread_local QObject *tls_loaderCompletedObject = nullptr;
static thread_local QByteArray tls_pendingDelegateUrl;
static thread_local QByteArray tls_pendingDelegateParent;
static thread_local size_t tls_pendingDelegateHeap = 0;
static thread_local std::unordered_map<void*, DelegateTrack> tls_delegateByIncubator;

// Sub-component (delegate base-component) sizing. Only active when
// QML_OBJECT_TRACK_SUBCOMPONENTS is set. While a delegate is being built we
// bracket every nested QQmlObjectCreator (ctor..dtor) so the delegate's base
// components show up as its children with their own heap sizes. Names come from
// the parent context URL when available, otherwise a placeholder.
struct SubFrame { QByteArray parentUrl; size_t heapBegin; long long childrenDelta; bool isRoot; };
static thread_local bool tls_inDelegateBuild = false;
static thread_local bool tls_delegateRootSeen = false;
static thread_local QStack<SubFrame> tls_subStack;
static thread_local std::vector<std::pair<QByteArray, long long>> tls_subComponents;
static thread_local long long tls_delegateSubHeap = 0;

static bool subComponentsEnabled()
{
    static const bool v = (::getenv("QML_OBJECT_TRACK_SUBCOMPONENTS") != nullptr);
    return v;
}

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

    // QQmlContextData::emitDestruction() — fires when component tree is torn down
    *(void **) (&s_emitDestructionFn) = dlsym(RTLD_NEXT,
        "_ZN15QQmlContextData15emitDestructionEv");
    if (!s_emitDestructionFn)
        qFatal("QML-OBJECT-TRACK: could not resolve QQmlContextData::emitDestruction: %s", dlerror());

    // QQmlComponent::create(QQmlIncubator &, QQmlContext *, QQmlContext *)
    // Fallback for Qt builds where QQmlObjectCreator hooks are bypassed (direct intra-SO calls).
    // Called via PLT from libQt6Quick.so so it IS intercepted via LD_PRELOAD.
    *(void **) (&s_componentCreateFn) = dlsym(RTLD_NEXT,
        "_ZN13QQmlComponent6createER13QQmlIncubatorP11QQmlContextS3_");
    if (!s_componentCreateFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlComponent::create(incubator) — component-level fallback disabled");

    // Delegate tracking hooks: creationContext() + QQmlIncubatorC2 + clear() form a bracket
    // around each delegate creation when QQmlObjectCreator PLT hooks are bypassed.
    *(void **) (&s_creationContextFn) = dlsym(RTLD_NEXT, "_ZNK13QQmlComponent15creationContextEv");
    if (!s_creationContextFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlComponent::creationContext() — delegate tracking disabled");

    *(void **) (&s_incubatorCtorFn) = dlsym(RTLD_NEXT, "_ZN13QQmlIncubatorC2ENS_14IncubationModeE");
    if (!s_incubatorCtorFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlIncubator::QQmlIncubator(IncubationMode) — delegate tracking disabled");

    *(void **) (&s_incubatorClearFn) = dlsym(RTLD_NEXT, "_ZN13QQmlIncubator5clearEv");
    if (!s_incubatorClearFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlIncubator::clear() — delegate tracking disabled");

    // Used in incubatorClear_hook to get the delegate URL and connect the destroyed() signal
    *(void **) (&s_incubatorObjectFn) = dlsym(RTLD_NEXT, "_ZNK13QQmlIncubator6objectEv");
    *(void **) (&s_qmlContextFn)      = dlsym(RTLD_NEXT, "_Z10qmlContextPK7QObject");
    *(void **) (&s_contextBaseUrlFn)  = dlsym(RTLD_NEXT, "_ZNK11QQmlContext7baseUrlEv");

    // QQmlApplicationEngine::load(const QUrl &) — app binary → libQt6Qml.so PLT call
    *(void **) (&s_engineLoadFn) = dlsym(RTLD_NEXT, "_ZN21QQmlApplicationEngine4loadERK4QUrl");
    if (!s_engineLoadFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlApplicationEngine::load(QUrl) — root QML file will not be tracked");

    // QQmlComponent::create(QQmlContext*) — synchronous create called from user code with QQmlEngine.
    // QQmlApplicationEngine calls this intra-SO, so this hook only fires from user-written call sites.
    *(void **) (&s_componentCreateSyncFn) = dlsym(RTLD_NEXT, "_ZN13QQmlComponent6createEP11QQmlContext");
    if (!s_componentCreateSyncFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlComponent::create(QQmlContext*) — QQmlEngine usage will not be tracked");

    // QQmlApplicationEngine::rootObjects() const — used after load() to get new root objects
    *(void **) (&s_rootObjectsFn) = dlsym(RTLD_NEXT, "_ZNK21QQmlApplicationEngine11rootObjectsEv");
    if (!s_rootObjectsFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlApplicationEngine::rootObjects() — main.qml destruction will not be tracked");

    // QQmlApplicationEngine::loadFromModule(QAnyStringView, QAnyStringView)
    *(void **) (&s_loadFromModuleFn) = dlsym(RTLD_NEXT,
        "_ZN21QQmlApplicationEngine14loadFromModuleE14QAnyStringViewS0_");
    if (!s_loadFromModuleFn)
        qWarning("QML-OBJECT-TRACK: could not resolve QQmlApplicationEngine::loadFromModule() — root QML module will not be tracked");

    // QQmlObjectCreator destructor — bracket end for sub-component sizing.
    *(void **) (&s_objectCreatorDtorFn) = dlsym(RTLD_NEXT, "_ZN17QQmlObjectCreatorD1Ev");
    if (!s_objectCreatorDtorFn)
        qWarning("QML-OBJECT-TRACK: could not resolve ~QQmlObjectCreator — sub-component sizing disabled");

    qInfo() << "QML-OBJECT-TRACK: successfully loaded";
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

// Hook for QQmlApplicationEngine::load(const QUrl &) — tracks the root QML file.
// The application binary calls this via PLT, so LD_PRELOAD interposition works.
// QQmlObjectCreator is called internally within libQt6Qml.so (no PLT), so we can
// only measure the full heap bracket around the entire engine load here.
[[gnu::visibility("default")]] void qml_engineLoad_hook(void *self, const QUrl &url)
    __asm__("_ZN21QQmlApplicationEngine4loadERK4QUrl");

void qml_engineLoad_hook(void *self, const QUrl &url)
{
    resolveFunctions();
    if (!s_engineLoadFn)
        return;

    QByteArray urlStr = url.toString().toUtf8();
    size_t rootCountBefore = s_rootObjectsFn ? (size_t)s_rootObjectsFn(self).size() : 0;
    size_t heapBefore = measureHeapSize();
    objectTrack(urlStr, true, Create, {}, heapBefore);
    s_engineLoadFn(self, url);
    size_t heapAfter = measureHeapSize();
    objectTrack(urlStr, false, Create, {}, heapAfter);

    if (s_rootObjectsFn) {
        QList<QObject*> roots = s_rootObjectsFn(self);
        long long creationDelta = static_cast<long long>(heapAfter) - static_cast<long long>(heapBefore);
        for (int i = static_cast<int>(rootCountBefore); i < roots.size(); ++i) {
            if (QObject *obj = roots[i]) {
                QObject::connect(obj, &QObject::destroyed, [urlStr, creationDelta](QObject *) {
                    size_t heapNow = measureHeapSize();
                    size_t heapStart = static_cast<size_t>(
                        static_cast<long long>(heapNow) + creationDelta);
                    objectTrack(urlStr, true,  Destroy, {}, heapStart);
                    objectTrack(urlStr, false, Destroy, {}, heapNow);
                });
            }
        }
    }
}

// Hook for QQmlApplicationEngine::loadFromModule(QAnyStringView, QAnyStringView).
// loadFromModule() does not call load(QUrl), so without this the root QML file
// (e.g. Main.qml) is never tracked. The application calls this via PLT, so the
// LD_PRELOAD interposition works. On builds where the fine-grained
// QQmlObjectCreator hooks fire during the load (tls_initFromTypeFired), they
// already captured the root with nesting, so we suppress our coarse tracking.
[[gnu::visibility("default")]] void qml_loadFromModule_hook(void *self, QAnyStringView uri,
                                                            QAnyStringView typeName)
    __asm__("_ZN21QQmlApplicationEngine14loadFromModuleE14QAnyStringViewS0_");

void qml_loadFromModule_hook(void *self, QAnyStringView uri, QAnyStringView typeName)
{
    resolveFunctions();
    ++s_hookCounts[H_loadModule];
    if (!s_loadFromModuleFn) {
        qFatal("QML-OBJECT-TRACK: loadFromModule hook with no resolved function");
        return;
    }

    size_t rootCountBefore = s_rootObjectsFn ? (size_t)s_rootObjectsFn(self).size() : 0;
    tls_initFromTypeFired = false;
    size_t heapBefore = measureHeapSize();
    s_loadFromModuleFn(self, uri, typeName);
    size_t heapAfter = measureHeapSize();

    // If the fine-grained hooks fired during the load, they already tracked the
    // root component (and its synchronous children) with proper nesting.
    if (tls_initFromTypeFired || !s_rootObjectsFn)
        return;

    QList<QObject*> roots = s_rootObjectsFn(self);
    QObject *mainObj = nullptr;
    for (int i = (int)rootCountBefore; i < roots.size(); ++i) {
        if (roots[i]) { mainObj = roots[i]; break; }
    }
    if (!mainObj)
        return;

    // The root file URL comes from the created object's context base URL.
    QByteArray url;
    if (s_qmlContextFn && s_contextBaseUrlFn) {
        if (void *ctx = s_qmlContextFn(mainObj))
            url = s_contextBaseUrlFn(ctx).toString().toUtf8();
    }
    if (url.isEmpty())
        return;

    objectTrack(url, true,  Create, {}, heapBefore);
    objectTrack(url, false, Create, {}, heapAfter);

    long long creationDelta = (long long)heapAfter - (long long)heapBefore;
    QObject::connect(mainObj, &QObject::destroyed, [url, creationDelta](QObject *) {
        size_t heapNow = measureHeapSize();
        size_t heapStart = (size_t)((long long)heapNow + creationDelta);
        objectTrack(url, true,  Destroy, {}, heapStart);
        objectTrack(url, false, Destroy, {}, heapNow);
    });
}

// Hook for QQmlComponent::create(QQmlContext*) — synchronous non-incubated create.
// Fires when user code calls component.create() directly with a QQmlEngine.
// When QQmlApplicationEngine is used, this same function is called intra-SO so the
// PLT hook does not fire — no double-tracking with qml_engineLoad_hook.
[[gnu::visibility("default")]] QObject *qml_componentCreateSync_hook(void *self, void *context)
    __asm__("_ZN13QQmlComponent6createEP11QQmlContext");

QObject *qml_componentCreateSync_hook(void *self, void *context)
{
    resolveFunctions();
    ++s_hookCounts[H_componentSync];
    if (!s_componentCreateSyncFn)
        return nullptr;

    QByteArray url = componentUrl(self);
    size_t heapBefore = measureHeapSize();
    objectTrack(url, true, Create, {}, heapBefore);
    QObject *result = s_componentCreateSyncFn(self, context);
    size_t heapAfter = measureHeapSize();
    objectTrack(url, false, Create, {}, heapAfter);

    if (result && !url.isEmpty()) {
        long long creationDelta = static_cast<long long>(heapAfter) - static_cast<long long>(heapBefore);
        QObject::connect(result, &QObject::destroyed, [url, creationDelta](QObject *) {
            size_t heapNow = measureHeapSize();
            size_t heapStart = static_cast<size_t>(
                static_cast<long long>(heapNow) + creationDelta);
            objectTrack(url, true,  Destroy, {}, heapStart);
            objectTrack(url, false, Destroy, {}, heapNow);
        });
    }
    return result;
}

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
    ++s_hookCounts[H_objCtor];
    s_objectCreatorCtorFn(self, parentContext, compilationUnit,
                          creationContext, inlineComponentName, incubator);

    // Extract parent component URL from parentContext
    QByteArray parentUrl;
    void *contextDataPtr = *(void **)parentContext;
    if (contextDataPtr)
        parentUrl = contextUrl(contextDataPtr);

    // Deferred creations (Controls indicator/contentItem/background, popups) are
    // built after their owner's bracket has closed, with an anonymous parent
    // context that has no URL — which would orphan them to the report root. Fall
    // back to the creationContext (the context the component was defined in) so
    // they're attributed to their owning file instead.
    if (parentUrl.isEmpty() && creationContext) {
        void *ccPtr = *(void **)creationContext;
        if (ccPtr)
            parentUrl = contextUrl(ccPtr);
    }

    // While a delegate is being built, open a heap bracket for this creator so
    // its (and its nested children's) size can be attributed under the delegate.
    // The outermost creator is the delegate root itself, so it is flagged isRoot
    // and not reported as a base component.
    if (subComponentsEnabled() && tls_inDelegateBuild) {
        // The first creator of a delegate build is the delegate root itself;
        // every later creator (nested or sequential) is a base component.
        bool isRoot = !tls_delegateRootSeen;
        tls_delegateRootSeen = true;
        tls_subStack.push({ isRoot ? QByteArray() : parentUrl, measureHeapSize(), 0, isRoot });
    }

    std::lock_guard<std::mutex> lock(s_creatorInfoMutex);
    s_creatorInfoMap[self] = { incubator != nullptr, std::move(parentUrl) };
}

// Hook for ~QQmlObjectCreator — closes the heap bracket opened in the ctor hook
// and records each nested (non-root) creator as a base component of the delegate.
[[gnu::visibility("default")]] void qml_objectCreator_dtor_hook(void *self)
    __asm__("_ZN17QQmlObjectCreatorD1Ev");

void qml_objectCreator_dtor_hook(void *self)
{
    resolveFunctions();
    ++s_hookCounts[H_objDtor];

    if (subComponentsEnabled() && tls_inDelegateBuild && !tls_subStack.isEmpty()) {
        // Measure before the real dtor: the object is already built; the dtor
        // only frees the creator's transient bookkeeping.
        size_t heapNow = measureHeapSize();
        SubFrame f = tls_subStack.pop();
        long long bracketDelta = static_cast<long long>(heapNow) - static_cast<long long>(f.heapBegin);
        long long selfDelta = bracketDelta - f.childrenDelta;
        if (!tls_subStack.isEmpty())
            tls_subStack.top().childrenDelta += bracketDelta;
        if (!f.isRoot) {
            tls_subComponents.push_back({ f.parentUrl, selfDelta });
            tls_delegateSubHeap += selfDelta;
        }
    }

    if (s_objectCreatorDtorFn)
        s_objectCreatorDtorFn(self);
}

// Hook for QQmlContextData::initFromTypeCompilationUnit — called early inside create(),
// before createInstance() does the actual allocations. We capture URL + start heap here.
[[gnu::visibility("default")]] void qml_initFromType_hook(
    void *self, const void *unit, int subComponentIndex)
    __asm__("_ZN15QQmlContextData27initFromTypeCompilationUnitERK14QQmlRefPointerIN3QV425ExecutableCompilationUnitEEi");

void qml_initFromType_hook(void *self, const void *unit, int subComponentIndex)
{
    resolveFunctions();
    ++s_hookCounts[H_initFromType];
    s_initFromTypeFn(self, unit, subComponentIndex);
    // Signal that fine-grained tracking fired, so the component-level fallback hook
    // knows to skip its coarse-grained tracking.
    tls_initFromTypeFired = true;
    // After the real call, this QQmlContextData has the URL set — start tracking here
    QByteArray url = contextUrl(self);
    // subComponentIndex >= 0 means an inline sub-component: an inline delegate or
    // inline Component defined *inside* this file. It shares the file's URL and is
    // part of that file's instance, NOT a separate instance of it. incubator != null
    // (tls_currentIsDelegate) with subComponentIndex < 0 is a real (separate-file)
    // delegate, which IS a genuine instance.
    const bool inlineSub = (subComponentIndex >= 0);
    TrackType type = (tls_currentIsDelegate || inlineSub) ? Delegate : Create;

    // Determine parent: if we're nested inside another creation, the stack top is the parent.
    // Only fall back to tls_currentParentUrl (from QQmlObjectCreator constructor) for top-level.
    QByteArray parentUrl;
    if (inlineSub) {
        // Mark internal (parent == url): adds heap to the file without inventing a
        // new instance or an edge to whatever ancestor happens to be on the stack
        // (which, for deferred-built inline delegates, is often the wrong one). Its
        // own children still nest under the file, because we push the file url below.
        parentUrl = url;
    } else if (!tls_creationStack.isEmpty()) {
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
    ++s_hookCounts[H_objCreate];

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
    ++s_hookCounts[H_finalize];

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

// Hook for QQmlContextData::emitDestruction — fires when component tree is torn down
[[gnu::visibility("default")]] void qml_emitDestruction_hook(void *self)
    __asm__("_ZN15QQmlContextData15emitDestructionEv");

void qml_emitDestruction_hook(void *self)
{
    resolveFunctions();
    ++s_hookCounts[H_emitDestroy];

    QByteArray url = contextUrl(self);

    if (url.isEmpty()) {
        if (tls_contextDestroyDepth > 0) {
            // Empty URL inside a component context tree → per-delegate wrapper context.
            // Suppress all its children (Fusion style components etc.) for the duration.
            bool prev = tls_inDelegateContextDestruction;
            tls_inDelegateContextDestruction = true;
            s_emitDestructionFn(self);
            tls_inDelegateContextDestruction = prev;
        } else {
            // Empty URL at depth 0 → engine root context. Just recurse without suppressing.
            s_emitDestructionFn(self);
        }
        return;
    }

    if (tls_inDelegateContextDestruction) {
        // Child of a per-delegate wrapper context (e.g. Fusion Button.qml). Suppress.
        s_emitDestructionFn(self);
        return;
    }

    ++tls_contextDestroyDepth;
    objectTrack(url, true, Destroy);
    s_emitDestructionFn(self);
    objectTrack(url, false, Destroy);
    --tls_contextDestroyDepth;
}


// Hook for QQmlComponent::creationContext() const
// PLT call from libQt6QmlModels.so — fires exactly once per delegate before incubation begins.
// Records the delegate component URL and heap snapshot into a TLS "pending" slot.
[[gnu::visibility("default")]] void *qml_creationContext_hook(const void *self)
    __asm__("_ZNK13QQmlComponent15creationContextEv");

void *qml_creationContext_hook(const void *self)
{
    resolveFunctions();
    ++s_hookCounts[H_creationCtx];
    void *result = s_creationContextFn(self);
    QByteArray url = componentUrl(const_cast<void *>(self));
    if (!url.isEmpty()) {
        tls_pendingDelegateUrl   = url;
        tls_pendingDelegateHeap  = measureHeapSize();
        // The creation context is where the delegate is *used* (e.g. the
        // ListView's file). Record it so the report can nest the delegate
        // under its host file instead of listing it as a top-level entry.
        tls_pendingDelegateParent.clear();
        if (result && s_contextBaseUrlFn)
            tls_pendingDelegateParent = s_contextBaseUrlFn(result).toString().toUtf8();
    }
    return result;
}

// Hook for QQmlIncubator::QQmlIncubator(IncubationMode) — the base constructor.
// PLT call from both libQt6QmlModels.so (delegate tasks) and libQt6Quick.so (Loader).
// Moves the pending URL/heap snapshot into a per-incubator map so clear() can consume it.
// Also resets tls_initFromTypeFired so we can detect whether fine-grained hooks fire during
// the upcoming incubation (used to suppress double-counting on ARM/Yocto builds).
[[gnu::visibility("default")]] void qml_incubatorCtor_hook(void *self, int mode)
    __asm__("_ZN13QQmlIncubatorC2ENS_14IncubationModeE");

void qml_incubatorCtor_hook(void *self, int mode)
{
    resolveFunctions();
    s_incubatorCtorFn(self, mode);
    tls_initFromTypeFired = false;
    if (!tls_pendingDelegateUrl.isEmpty()) {
        tls_delegateByIncubator[self] = { tls_pendingDelegateUrl, tls_pendingDelegateParent,
                                          tls_pendingDelegateHeap };
        tls_pendingDelegateUrl.clear();
        tls_pendingDelegateParent.clear();
        tls_pendingDelegateHeap = 0;
        if (subComponentsEnabled()) {
            tls_inDelegateBuild = true;
            tls_delegateRootSeen = false;
            tls_subStack.clear();
            tls_subComponents.clear();
            tls_delegateSubHeap = 0;
        }
    }
}

// Hook for QQmlIncubator::clear() — PLT call from libQt6QmlModels.so and libQt6Quick.so.
// For delegate incubators that completed successfully:
//   - consumes the per-incubator map entry set up by qml_incubatorCtor_hook
//   - measures heap AFTER clear() to capture the delegate's persistent footprint
//   - only fires when fine-grained QQmlObjectCreator hooks did NOT already track this delegate
[[gnu::visibility("default")]] void qml_incubatorClear_hook(void *self)
    __asm__("_ZN13QQmlIncubator5clearEv");

void qml_incubatorClear_hook(void *self)
{
    resolveFunctions();
    ++s_hookCounts[H_incubatorClear];
    auto it = tls_delegateByIncubator.find(self);
    bool hasDelegateInfo = (it != tls_delegateByIncubator.end());

    // QQmlDelegateModel calls clear() immediately after constructing the incubator (before
    // incubation) to initialise it — at that point status is Null and object() returns null.
    // The real completion clear fires later with status=Ready and object() non-null.
    // Use object()!=null as the signal that incubation actually completed.
    QObject *obj = (hasDelegateInfo && !tls_initFromTypeFired && s_incubatorObjectFn)
                   ? s_incubatorObjectFn(self) : nullptr;

    if (hasDelegateInfo && obj == nullptr && !it->second.initClearFired) {
        // Premature init clear (status=Null, real clear() is a no-op).
        // Refresh heapBefore so the delta covers actual delegate allocation, not QQDMIncubationTask.
        it->second.initClearFired = true;
        it->second.heapBefore = measureHeapSize();
        if (subComponentsEnabled()) {   // real build starts now -> discard anything collected so far
            tls_delegateRootSeen = false;
            tls_subStack.clear();
            tls_subComponents.clear();
            tls_delegateSubHeap = 0;
        }
        s_incubatorClearFn(self);
        return;
    }

    DelegateTrack info;
    if (hasDelegateInfo) {
        info = it->second;
        tls_delegateByIncubator.erase(it);
    }

    // Get the delegate component URL from the created object's context BEFORE clear() nullifies
    // d->result. qmlContext(obj)->baseUrl() is the URL of the component file that defines the
    // delegate (e.g. listview.qml), which is what we want to report for both creation and
    // destruction. The per-delegate QQmlContextData has an empty url() (no compilation unit),
    // so we can't rely on emitDestruction to report the right URL; instead we connect to the
    // QObject::destroyed signal so the correct URL is used at destruction time too.
    QByteArray objectUrl;
    if (obj && s_qmlContextFn && s_contextBaseUrlFn) {
        if (void *ctx = s_qmlContextFn(obj))
            objectUrl = s_contextBaseUrlFn(ctx).toString().toUtf8();
    }

    // For Loader-created components (not delegate model): the Loader calls clear() from inside
    // statusChanged(Ready) — which fires during QQmlComponent::create(incubator) — so by the
    // time qml_component_create_hook reads the incubator after create() returns, d->result is
    // already null. Capture the object here while it is still set, and stash it in a TLS slot
    // for qml_component_create_hook to pick up and connect destroyed() with the correct delta.
    if (!hasDelegateInfo && s_incubatorObjectFn) {
        if (QObject *loaderObj = s_incubatorObjectFn(self))
            tls_loaderCompletedObject = loaderObj;
    }

    s_incubatorClearFn(self);

    // Skip if fine-grained QQmlObjectCreator hooks already tracked this delegate
    // (tls_initFromTypeFired is set by initFromType_hook in PLT-complete Qt builds like ARM/Yocto).
    if (hasDelegateInfo && obj != nullptr && !tls_initFromTypeFired) {
        // host = the file the delegate is used in (its creation context).
        // node = the delegate's own file. For an inline delegate the two are the
        // same, so we synthesise a node from the delegate's root type ("Button
        // [delegate]") and hang it under the host, instead of folding it into the
        // host's own number.
        QByteArray host = info.parentUrl;
        QByteArray node = !objectUrl.isEmpty() ? objectUrl : info.url;
        if (host.isEmpty())
            host = node;
        if (node == host) {
            QByteArray tn = obj->metaObject()->className();
            int q = tn.indexOf("_QMLTYPE_");
            if (q >= 0)
                tn = tn.left(q);
            if (tn.startsWith("QQuick"))
                tn = tn.mid(6);
            node = (tn.isEmpty() ? QByteArray("delegate") : tn) + " [delegate]";
        }
        if (!node.isEmpty()) {
            QByteArray delegateParent = (host != node) ? host : QByteArray();
            size_t heapAfter = measureHeapSize();

            // With sub-component sizing on, the delegate's own number excludes its
            // base components (shown as children); otherwise it is the full bracket.
            long long bracket = static_cast<long long>(heapAfter) - static_cast<long long>(info.heapBefore);
            long long delegateSelf = subComponentsEnabled() ? (bracket - tls_delegateSubHeap) : bracket;
            objectTrack(node, true,  Delegate, delegateParent, info.heapBefore);
            objectTrack(node, false, Delegate, delegateParent,
                        static_cast<size_t>(static_cast<long long>(info.heapBefore) + delegateSelf));

            // Emit the delegate's base components as a single child holding their
            // total heap. Their individual file names aren't reachable on builds
            // where the fine-grained hooks don't fire (the parent context URL just
            // points back at the host, which would make a cycle), so a per-delegate
            // placeholder key is used instead.
            if (subComponentsEnabled() && tls_delegateSubHeap != 0) {
                QByteArray baseKey = node + "  <base components>";
                objectTrack(baseKey, true,  Delegate, node, info.heapBefore);
                objectTrack(baseKey, false, Delegate, node,
                            static_cast<size_t>(static_cast<long long>(info.heapBefore) + tls_delegateSubHeap));
            }

            // The destroyed() signal fires from inside QObject::~QObject() before the
            // object's memory is returned to the allocator, so measureHeapSize() at that
            // point still includes the object. To produce a meaningful negative delta, we
            // fake the "before" heap as (heapNow + creationDelta), making the delta equal
            // to -creationDelta without inventing any allocation numbers.
            QByteArray capturedUrl = node;
            long long capturedDelta = static_cast<long long>(heapAfter)
                                    - static_cast<long long>(info.heapBefore);
            QObject::connect(obj, &QObject::destroyed, [capturedUrl, capturedDelta](QObject *) {
                size_t heapNow = measureHeapSize();
                size_t heapStart = static_cast<size_t>(static_cast<long long>(heapNow) + capturedDelta);
                objectTrack(capturedUrl, true,  DelegateDestroy, {}, heapStart);
                objectTrack(capturedUrl, false, DelegateDestroy, {}, heapNow);
            });
        }
    }

    // Delegate build finished — stop collecting sub-components.
    if (subComponentsEnabled()) {
        tls_inDelegateBuild = false;
        tls_delegateRootSeen = false;
        tls_subStack.clear();
        tls_subComponents.clear();
        tls_delegateSubHeap = 0;
    }
}


// Hook for QQmlComponent::create(QQmlIncubator &, QQmlContext *, QQmlContext *)
// Fallback tracking used when QQmlObjectCreator hooks are bypassed by direct intra-SO calls
// (observed in Qt installer builds that omit PLT indirection for intra-library calls).
// libQt6Quick.so calls this via PLT, so LD_PRELOAD interposition works here.
// On ARM target builds where QQmlObjectCreator hooks DO fire, tls_initFromTypeFired
// suppresses duplicate tracking from this hook.
[[gnu::visibility("default")]] void qml_component_create_hook(
    void *self, void *incubator, void *context, void *forContext)
    __asm__("_ZN13QQmlComponent6createER13QQmlIncubatorP11QQmlContextS3_");

void qml_component_create_hook(void *self, void *incubator, void *context, void *forContext)
{
    resolveFunctions();
    ++s_hookCounts[H_componentCreate];
    if (!s_componentCreateFn)
        return;

    // QQmlComponent::create(incubator) is called by Loader — NOT by delegate models.
    // Delegate models call incubateObject() (virtual, not PLT) instead. Erase any map entry
    // that was tentatively created by qml_incubatorCtor_hook for this Loader incubator.
    tls_delegateByIncubator.erase(incubator);
    tls_pendingDelegateUrl.clear();
    tls_pendingDelegateParent.clear();

    // If a delegate incubation is in progress (tls_delegateByIncubator is non-empty), this
    // create() call is for a style sub-object (e.g. Fusion Button.qml) loaded during delegate
    // construction — not a user-visible component. Skip tracking: the delegate's destroyed()
    // signal already handles the one-per-delegate accounting, and emitDestruction for the style
    // context is suppressed by tls_suppressEmitDestruction / tls_pendingDelegateDestroys.
    if (!tls_delegateByIncubator.empty()) {
        s_componentCreateFn(self, incubator, context, forContext);
        tls_loaderCompletedObject = nullptr; // discard any sub-object captured by incubatorClear
        return;
    }

    QByteArray url = componentUrl(self);

    // The host file (the file that contains the Loader) is the base URL of the
    // context the component is created in — used as the tree parent so the
    // loaded file nests under its host (e.g. listview.qml under Main.qml).
    QByteArray hostUrl;
    if (context && s_contextBaseUrlFn)
        hostUrl = s_contextBaseUrlFn(context).toString().toUtf8();
    if (hostUrl == url)
        hostUrl.clear();

    // Reset flag so we can detect if initFromType fires during this call
    tls_initFromTypeFired = false;
    tls_loaderCompletedObject = nullptr;
    size_t heapBefore = measureHeapSize();

    s_componentCreateFn(self, incubator, context, forContext);

    // Pick up the root object saved by qml_incubatorClear_hook. The Loader calls
    // incubator->clear() inside statusChanged(Ready) — which fires during create() — so
    // d->result is already null here. The hook captured the pointer before clear() ran.
    QObject *completedObj = tls_loaderCompletedObject;
    tls_loaderCompletedObject = nullptr;

    size_t heapAfter = measureHeapSize();

    // Only track here if the fine-grained QQmlObjectCreator hooks did NOT fire.
    // If they fired, they already captured the creation with better granularity.
    if (!tls_initFromTypeFired && !url.isEmpty()) {
        objectTrack(url, true,  ComponentCreate, hostUrl, heapBefore);
        objectTrack(url, false, ComponentCreate, hostUrl, heapAfter);
    }

    // Connect destroyed() on the root object to track component teardown (Loader unload / shutdown).
    if (completedObj && !url.isEmpty()) {
        long long creationDelta = static_cast<long long>(heapAfter) - static_cast<long long>(heapBefore);
        QObject::connect(completedObj, &QObject::destroyed, [url, creationDelta](QObject *) {
            size_t heapNow = measureHeapSize();
            size_t heapStart = static_cast<size_t>(
                static_cast<long long>(heapNow) + creationDelta);
            objectTrack(url, true,  Destroy, {}, heapStart);
            objectTrack(url, false, Destroy, {}, heapNow);
        });
    }
}


// ── Heap measurement ────────────────────────────────────────────────────

static size_t measureHeapSize()
{
    struct mallinfo2 info = ::mallinfo2();
    return static_cast<size_t>(info.uordblks);
}

// ── Reporting & aggregation ─────────────────────────────────────────────
//
// By default the tracker prints a short heap-influence report to stderr when
// the process exits: a tree of the QML files that contributed the most heap,
// capping the breadth per level and bucketing the rest into an "others" entry.
//
// Environment variables:
//   QML_OBJECT_TRACK_MODE     report (default) | timer | detailed
//   QML_OBJECT_TRACK_ITEMS    max children shown per tree level   (default 10)
//   QML_OBJECT_TRACK_INTERVAL seconds between reports (implies timer mode)
//   QML_OBJECT_TRACK_DEPTH    max tree depth, 0 = unlimited       (default 0)
//   QML_OBJECT_TRACK_CSV      1 to also write the detailed CSV in any mode
//
//   report   : one report at exit.
//   timer    : a report every INTERVAL seconds, plus the final report at exit.
//   detailed : like report, but also writes /tmp/qml-object-track.<pid>.csv
//              (the per-instantiation log — the original behaviour).

namespace {

struct Config {
    enum Mode { Report, Timer, Detailed };
    Mode mode = Report;
    int  items = 10;       // max children shown per tree level (rest -> "others")
    int  intervalSec = 5;  // timer-mode period
    int  maxDepth = 0;     // 0 == unlimited
    long long minSize = 0; // only list items whose inclusive size >= this (rest -> "others")
    bool csv = false;      // also write the detailed CSV
    bool timer = false;    // run the periodic reporter
};

const Config &config()
{
    static const Config cfg = [] {
        Config c;
        if (const char *m = ::getenv("QML_OBJECT_TRACK_MODE")) {
            if (!std::strcmp(m, "timer"))         { c.mode = Config::Timer;    c.timer = true; }
            else if (!std::strcmp(m, "detailed")) { c.mode = Config::Detailed; c.csv = true; }
        }
        if (const char *v = ::getenv("QML_OBJECT_TRACK_ITEMS")) {
            int n = ::atoi(v);
            if (n > 0)
                c.items = n;
        }
        if (const char *v = ::getenv("QML_OBJECT_TRACK_INTERVAL")) {
            int n = ::atoi(v);
            if (n > 0) {
                c.intervalSec = n;
                c.timer = true;
            }
        }
        if (const char *v = ::getenv("QML_OBJECT_TRACK_DEPTH")) {
            int n = ::atoi(v);
            if (n >= 0)
                c.maxDepth = n;
        }
        if (const char *v = ::getenv("QML_OBJECT_TRACK_MIN_SIZE")) {
            char *end = nullptr;
            long long n = ::strtoll(v, &end, 10);
            if (n > 0) {
                if (end && (*end == 'k' || *end == 'K')) n *= 1024;
                else if (end && (*end == 'm' || *end == 'M')) n *= 1024LL * 1024;
                else if (end && (*end == 'g' || *end == 'G')) n *= 1024LL * 1024 * 1024;
                c.minSize = n;
            }
        }
        if (const char *v = ::getenv("QML_OBJECT_TRACK_CSV")) {
            if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y')
                c.csv = true;
        }
        return c;
    }();
    return cfg;
}

struct NodeAgg {
    long long createDelta = 0;   // SELF (exclusive) heap added by this file, not its children
    long long destroyDelta = 0;  // SELF heap freed during destruction (usually <= 0)
    int createCount = 0;
    int destroyCount = 0;
};

using NodeMap = std::map<QByteArray, NodeAgg>;                       // url -> aggregate
using EdgeMap = std::map<QByteArray, std::map<QByteArray, NodeAgg>>; // parentUrl -> childUrl -> aggregate

// One open creation/destruction bracket. childrenDelta accumulates the
// (inclusive) deltas of nested brackets so that self = bracket - childrenDelta.
struct Frame {
    QByteArray url;
    size_t heapBegin = 0;
    long long childrenDelta = 0;
};

// All mutable tracker state lives in one heap-allocated, never-destroyed struct.
// This keeps the at-exit report safe regardless of the order in which the C++
// runtime tears down namespace-scope objects vs. runs atexit() handlers (the
// report handler is registered from a library constructor, which can run before
// namespace-scope objects are dynamically initialised).
struct State {
    std::mutex mutex;                                           // guards everything below
    std::map<QByteArray, NodeAgg> nodes;                       // url -> aggregate (self)
    std::map<QByteArray, std::map<QByteArray, NodeAgg>> edges;  // parentUrl -> childUrl -> aggregate
    QByteArray rootUrl;                                        // first file created = the tree root
    size_t peakHeap = 0;
    // Snapshot of nodes/edges at (close to) the peak heap, so the final report can
    // show the tree as it was at the high-water mark rather than the post-unload state.
    NodeMap peakNodes;
    EdgeMap peakEdges;
    size_t peakSnapshotHeap = 0;
    int nestingLevel = 0;
    int startCount = 0;
    QStack<Frame> trackStack;
    FILE *csvFile = nullptr;
};

State &state()
{
    static State *s = new State();  // intentionally leaked: never destroyed
    return *s;
}

QByteArray fmtBytes(long long b)
{
    const char *sign = (b < 0) ? "-" : "+";
    double a = (b < 0) ? -double(b) : double(b);
    char buf[48];
    if (a < 1024.0)
        std::snprintf(buf, sizeof(buf), "%s%.0f B", sign, a);
    else if (a < 1024.0 * 1024.0)
        std::snprintf(buf, sizeof(buf), "%s%.1f KB", sign, a / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%s%.1f MB", sign, a / (1024.0 * 1024.0));
    return QByteArray(buf);
}

// Compact form: drop the URL scheme and keep the last two path segments so
// common basenames (Button.qml) stay distinguishable.
QByteArray shortUrl(const QByteArray &url)
{
    if (url.contains("<base components>"))   // namespaced key -> show the plain label
        return QByteArray("<base components>");
    QByteArray u = url;
    int scheme = u.indexOf("://");
    if (scheme >= 0)
        u = u.mid(scheme + 3);
    else if (u.startsWith("qrc:"))
        u = u.mid(4);
    if (u.isEmpty())
        return QByteArray("<unknown>");
    int slash = u.lastIndexOf('/');
    if (slash > 0) {
        int prev = u.lastIndexOf('/', slash - 1);
        if (prev >= 0)
            return u.mid(prev + 1);
    }
    return (slash >= 0) ? u.mid(slash + 1) : u;
}

// Called with state().mutex held.
void accumulate(State &st, const QByteArray &url, const QByteArray &parentUrl,
                TrackType type, long long delta)
{
    const bool isDestroy = (type == Destroy || type == DelegateDestroy);
    NodeAgg &n = st.nodes[url];
    if (isDestroy) {
        n.destroyDelta += delta;
        ++n.destroyCount;
    } else {
        // A creation bracket can't have freed net memory — a negative delta means
        // unrelated frees happened to be captured in the (global mallinfo2) bracket
        // window, common for tiny/shared objects (token singletons etc.). Don't
        // attribute that to the creation; clamp it to zero.
        if (delta < 0)
            delta = 0;
        // Count instances, not brackets. A single component instance produces
        // several brackets: the create pass, a later finalize pass (bindings /
        // componentComplete), and one bracket per inline sub-object — the latter
        // carry the file's own url as their parent. Only the create/delegate/
        // component pass with an external parent is a genuinely new instance.
        const bool isInstance = (type != Finalize) && (parentUrl != url);
        n.createDelta += delta;
        if (isInstance)
            ++n.createCount;
        if (!parentUrl.isEmpty() && parentUrl != url) {
            NodeAgg &e = st.edges[parentUrl][url];
            e.createDelta += delta;
            if (isInstance)
                ++e.createCount;
        }
    }
}

// Inclusive subtree totals: a node's own self plus all of its descendants'.
struct Incl { long long create = 0; long long destroy = 0; };

Incl inclusiveOf(const NodeMap &nodes, const EdgeMap &edges,
                 const QByteArray &url, std::set<QByteArray> &visited)
{
    Incl r;
    auto nit = nodes.find(url);
    if (nit != nodes.end()) {
        r.create = nit->second.createDelta;
        r.destroy = nit->second.destroyDelta;
    }
    if (!visited.insert(url).second)   // already on this path -> stop (cycle guard)
        return r;
    auto eit = edges.find(url);
    if (eit != edges.end()) {
        for (const auto &c : eit->second) {
            Incl ci = inclusiveOf(nodes, edges, c.first, visited);
            r.create += ci.create;
            r.destroy += ci.destroy;
        }
    }
    visited.erase(url);
    return r;
}

struct ChildRef { QByteArray url; int count; };

// Gather the children of a node as ChildRefs, skipping destroy-only artifacts
// (files seen only at teardown, e.g. base components on the coarse-hook path).
std::vector<ChildRef> childrenOf(const NodeMap &nodes, const EdgeMap &edges, const QByteArray &url)
{
    std::vector<ChildRef> kids;
    auto eit = edges.find(url);
    if (eit != edges.end()) {
        for (const auto &c : eit->second) {
            auto nit = nodes.find(c.first);
            if (nit != nodes.end() && nit->second.createCount > 0)
                kids.push_back({ c.first, c.second.createCount });
        }
    }
    return kids;
}

void printChildren(FILE *out, const NodeMap &nodes, const EdgeMap &edges,
                   const std::vector<ChildRef> &children,
                   int indent, int depth, std::set<QByteArray> &path)
{
    struct Row { QByteArray url; int count; Incl incl; long long self; };
    std::vector<Row> rows;
    rows.reserve(children.size());
    for (const auto &c : children) {
        std::set<QByteArray> vis;
        Incl in = inclusiveOf(nodes, edges, c.url, vis);
        long long self = 0;
        auto nit = nodes.find(c.url);
        if (nit != nodes.end())
            self = nit->second.createDelta;
        rows.push_back({ c.url, c.count, in, self });
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        return a.incl.create > b.incl.create;
    });

    const int cap = config().items;
    const long long minSize = config().minSize;
    long long othersIncl = 0;
    int othersCount = 0;
    int shown = 0;
    for (const auto &row : rows) {
        // Fold into "others" once past the per-level item cap, or below the
        // configured minimum size (rows are sorted big-first, so this keeps the
        // largest contributors and buckets the long tail of small ones).
        if (shown >= cap || (minSize > 0 && row.incl.create < minSize)) {
            othersIncl += row.incl.create;
            ++othersCount;
            continue;
        }
        ++shown;

        QByteArray line(indent * 2, ' ');
        line += shortUrl(row.url);
        line += "  " + fmtBytes(row.incl.create);
        line += "  x" + QByteArray::number(row.count);
        if (row.self != row.incl.create)
            line += "  self " + fmtBytes(row.self);
        std::fprintf(out, "%s\n", line.constData());

        const bool depthOk = (config().maxDepth == 0) || (depth < config().maxDepth);
        if (depthOk && path.find(row.url) == path.end()) {
            std::vector<ChildRef> kids = childrenOf(nodes, edges, row.url);
            if (!kids.empty()) {
                path.insert(row.url);
                printChildren(out, nodes, edges, kids, indent + 1, depth + 1, path);
                path.erase(row.url);
            }
        }
    }
    if (othersCount > 0) {
        QByteArray pad(indent * 2, ' ');
        std::fprintf(out, "%s... others (%d files)  %s\n",
                     pad.constData(), othersCount, fmtBytes(othersIncl).constData());
    }
}

// usePeak: render the tree/heap from the peak-heap snapshot (the final report)
// rather than the current, post-unload state (the interval reports).
void printReport(const char *reason, bool usePeak)
{
    State &st = state();
    std::lock_guard<std::mutex> lock(st.mutex);

    const size_t now = measureHeapSize();
    if (now > st.peakHeap)
        st.peakHeap = now;

    // Pick the snapshot to render. Fall back to the live maps if we never took a
    // peak snapshot (e.g. heap never grew past the threshold).
    const bool havePeak = usePeak && !st.peakNodes.empty();
    const NodeMap &nodes = havePeak ? st.peakNodes : st.nodes;
    const EdgeMap &edges = havePeak ? st.peakEdges : st.edges;

    FILE *out = stderr;
    std::fprintf(out, "\n==================== QML object-track report (%s%s) - pid %d ====================\n",
                 reason, havePeak ? ", at peak" : "", (int)getpid());
    if (havePeak) {
        std::fprintf(out, "  peak malloc in-use : %s   (current %s)\n",
                     fmtBytes((long long)st.peakHeap).constData(), fmtBytes((long long)now).constData());
    } else {
        std::fprintf(out, "  malloc in-use now  : %s\n", fmtBytes((long long)now).constData());
        std::fprintf(out, "  peak malloc in-use : %s\n", fmtBytes((long long)st.peakHeap).constData());
    }
    size_t createdFiles = 0;
    for (const auto &ne : nodes)
        if (ne.second.createCount > 0)
            ++createdFiles;
    std::fprintf(out, "  QML files tracked  : %zu   (showing top %d per level)\n",
                 createdFiles, config().items);

    if (::getenv("QML_OBJECT_TRACK_DEBUG")) {
        std::fprintf(out, "  [debug] hook invocations:\n");
        for (int i = 0; i < H_COUNT; ++i)
            std::fprintf(out, "    %-30s %ld\n", s_hookNames[i], s_hookCounts[i].load());
    }

    if (createdFiles == 0) {
        std::fprintf(out, "  (no QML component allocations recorded - note that jemalloc makes\n"
                          "   mallinfo2 read 0, so the heap deltas need a glibc allocator)\n");
        std::fprintf(out, "================================================================================\n\n");
        std::fflush(out);
        return;
    }

    // The report is the tree under the application root (the first file created).
    // Any other node with no incoming edge is a creation whose parent we couldn't
    // determine (a deferred orphan, e.g. a Controls indicator/popup built after its
    // owner's bracket closed). Rather than show those at the top level where they'd
    // masquerade as roots, we leave them out entirely.
    std::vector<ChildRef> roots;
    auto rit = nodes.find(st.rootUrl);
    if (rit != nodes.end() && rit->second.createCount > 0) {
        roots.push_back({ st.rootUrl, rit->second.createCount });
    } else {
        // Fallback (no recorded root): show every created node with no incoming edge.
        std::set<QByteArray> childUrls;
        for (const auto &pe : edges)
            for (const auto &ce : pe.second)
                childUrls.insert(ce.first);
        for (const auto &ne : nodes)
            if (ne.second.createCount > 0 && childUrls.find(ne.first) == childUrls.end())
                roots.push_back({ ne.first, ne.second.createCount });
    }

    std::fprintf(out, "  QML trackdown%s:\n\n", havePeak ? " (at peak heap)" : "");
    std::set<QByteArray> path;
    printChildren(out, nodes, edges, roots, 1, 0, path);
    std::fprintf(out, "================================================================================\n\n");
    std::fflush(out);
}

// ── Periodic (timer-mode) reporter ──────────────────────────────────────

std::thread s_timerThread;
std::mutex s_timerMutex;
std::condition_variable s_timerCv;
bool s_timerStop = false;
std::atomic<bool> s_timerStarted{false};

void stopTimerThread()
{
    if (!s_timerStarted.load())
        return;
    {
        std::lock_guard<std::mutex> lk(s_timerMutex);
        s_timerStop = true;
    }
    s_timerCv.notify_all();
    if (s_timerThread.joinable())
        s_timerThread.join();
}

// The final (peak) report, emitted at most once — both QCoreApplication::aboutToQuit
// and atexit() try to fire it, whichever happens first wins.
std::atomic<bool> s_finalReported{false};
void printFinalReport(const char *reason)
{
    bool expected = false;
    if (!s_finalReported.compare_exchange_strong(expected, true))
        return;
    stopTimerThread();
    printReport(reason, /*usePeak=*/true);
}

void ensureReportingInit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        ::atexit([] { printFinalReport("final"); });
        if (config().timer) {
            s_timerStarted.store(true);
            s_timerThread = std::thread([] {
                std::unique_lock<std::mutex> lk(s_timerMutex);
                while (!s_timerStop) {
                    if (s_timerCv.wait_for(lk, std::chrono::seconds(config().intervalSec),
                                           [] { return s_timerStop; }))
                        break;
                    lk.unlock();
                    printReport("interval", /*usePeak=*/false);   // interval reports show live state
                    lk.lock();
                }
            });
        }
    });
}

// Connect to QCoreApplication::aboutToQuit so the final report also fires on a
// graceful Qt quit (more reliable than atexit alone, e.g. under an app manager).
// Connected lazily once qApp exists; fires the report at most once (see above).
void ensureQuitHook()
{
    static std::atomic<bool> connected{false};
    if (connected.load(std::memory_order_acquire))
        return;
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return;
    bool expected = false;
    if (!connected.compare_exchange_strong(expected, true))
        return;
    QObject::connect(app, &QCoreApplication::aboutToQuit, [] { printFinalReport("aboutToQuit"); });
}

} // namespace

// Register the at-exit report (and start the timer thread, if requested) at
// library-load time, so a report is still produced when the app never
// instantiates a tracked QML component (e.g. a long-running idle process).
__attribute__((constructor))
static void qmlObjectTrackInit()
{
    ensureReportingInit();
}

void objectTrack(const QByteArray &url, bool startOfCreation, TrackType trackType,
                 const QByteArray &parentUrl, size_t precomputedHeapSize)
{
    ensureReportingInit();
    ensureQuitHook();   // connect aboutToQuit once qApp exists (no-op until then)
    State &st = state();
    std::lock_guard<std::mutex> lock(st.mutex);

    const size_t heapSize = (precomputedHeapSize != 0) ? precomputedHeapSize : measureHeapSize();
    const bool newPeak = heapSize > st.peakHeap;
    if (newPeak)
        st.peakHeap = heapSize;

    QByteArray heapSizeDeltaStr;
    bool haveDelta = false;
    long long selfDelta = 0;

    if (startOfCreation) {
        ++st.nestingLevel;
        ++st.startCount;
        if (st.startCount == 1)
            st.rootUrl = url;   // the first file created is the application/tree root
        st.trackStack.push({ url, heapSize, 0 });
    } else {
        if (st.trackStack.isEmpty()) {
            qWarning() << "QML-OBJECT-TRACK: unexpected end for component at" << url;
        } else {
            Frame f = st.trackStack.pop();
            if (f.url != url)
                qWarning() << "QML-OBJECT-TRACK: mismatched end for component:" << url << "expected:" << f.url;
            else {
                // bracketDelta is inclusive (this file + everything created within
                // its bracket); selfDelta subtracts the nested children so each
                // file is counted once. The whole bracket is folded into the
                // enclosing frame so its parent's self excludes us in turn.
                long long bracketDelta = static_cast<long long>(heapSize)
                                       - static_cast<long long>(f.heapBegin);
                selfDelta = bracketDelta - f.childrenDelta;
                heapSizeDeltaStr = QByteArray::number(bracketDelta);
                haveDelta = true;
                if (!st.trackStack.isEmpty())
                    st.trackStack.top().childrenDelta += bracketDelta;
            }
        }
    }

    if (haveDelta)
        accumulate(st, url, parentUrl, trackType, selfDelta);

    // Snapshot the tree near each new heap high-water mark so the final report can
    // show the peak state. Throttled by a byte threshold so the copy doesn't run on
    // every allocation during ramp-up; the last snapshot lands within the threshold
    // of the true peak (the header still reports the exact peak value).
    if (newPeak) {
        constexpr size_t kPeakSnapshotThreshold = 1u << 20;   // 1 MB
        if (heapSize - st.peakSnapshotHeap >= kPeakSnapshotThreshold) {
            st.peakNodes = st.nodes;
            st.peakEdges = st.edges;
            st.peakSnapshotHeap = heapSize;
        }
    }

    // The detailed CSV is only written in detailed mode (or with CSV=1).
    if (config().csv) {
        if (!st.csvFile) {
            char filename[64];
            std::snprintf(filename, sizeof(filename), "/tmp/qml-object-track.%d.csv", getpid());
            st.csvFile = std::fopen(filename, "w");
            if (st.csvFile)
                qWarning() << "QML-OBJECT-TRACK: logging into" << filename;
            else
                qWarning() << "QML-OBJECT-TRACK: could not open" << filename << "for writing";
        }
        if (st.csvFile) {
            const char *typeStr = trackType == Create          ? "create"
                                : trackType == Finalize        ? "finalize"
                                : trackType == Delegate        ? "delegate"
                                : trackType == Destroy         ? "destroy"
                                : trackType == DelegateDestroy ? "delegate-destroy"
                                : "component";

            std::fprintf(st.csvFile,
                         "%s,%s,%s,%zu,%s,%d,%d,%s\n",
                         QByteArray(st.nestingLevel, startOfCreation ? '>' : '<').constData(),
                         typeStr,
                         url.constData(),
                         heapSize,
                         heapSizeDeltaStr.constData(),
                         st.nestingLevel,
                         st.startCount,
                         parentUrl.constData());
            std::fflush(st.csvFile);
        }
    }

    if (!startOfCreation)
        --st.nestingLevel;
}
