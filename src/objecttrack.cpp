// Copyright (C) 2026 Robert Griebl
// SPDX-License-Identifier: MIT

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <QtCore/QDebug>
#include <QtCore/qobject.h>
#include <QtCore/QUrl>
#include <QtCore/QStack>

#include <dlfcn.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

enum TrackType {
    Create = 0,
    Finalize,
    Delegate,
    Destroy,
    ComponentCreate // fallback: QQmlObjectCreator hooks bypassed (direct intra-SO calls)
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
struct DelegateTrack { QByteArray url; size_t heapBefore; bool initClearFired = false; };
// URLs ever seen at creation time. emitDestruction is only reported for URLs in this set,
// which naturally suppresses style contexts (e.g. Fusion Button.qml) that are never tracked
// at creation because component_create_hook skips them when a delegate incubation is active.
static std::unordered_set<std::string> s_trackedUrls;
static thread_local QByteArray tls_pendingDelegateUrl;
static thread_local size_t tls_pendingDelegateHeap = 0;
static thread_local std::unordered_map<void*, DelegateTrack> tls_delegateByIncubator;

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
    // Signal that fine-grained tracking fired, so the component-level fallback hook
    // knows to skip its coarse-grained tracking.
    tls_initFromTypeFired = true;
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

// Hook for QQmlContextData::emitDestruction — fires when component tree is torn down
[[gnu::visibility("default")]] void qml_emitDestruction_hook(void *self)
    __asm__("_ZN15QQmlContextData15emitDestructionEv");

void qml_emitDestruction_hook(void *self)
{
    resolveFunctions();

    QByteArray url = contextUrl(self);

    // Per-delegate wrapper contexts have empty url() (created via createRefCounted,
    // no compilation unit). Their destruction is already reported via QObject::destroyed.
    // Just recurse so child contexts can be evaluated by their own hook invocations.
    if (url.isEmpty()) {
        s_emitDestructionFn(self);
        return;
    }

    // Only report destruction for URLs we actually tracked at creation. URLs never
    // seen at creation (e.g. Fusion style components skipped in component_create_hook)
    // have count == 0 and would be spurious — skip them.
    if (s_trackedUrls.count(url.toStdString()) == 0) {
        s_emitDestructionFn(self);
        return;
    }

    objectTrack(url, true, Destroy);
    s_emitDestructionFn(self);
    objectTrack(url, false, Destroy);
}


// Hook for QQmlComponent::creationContext() const
// PLT call from libQt6QmlModels.so — fires exactly once per delegate before incubation begins.
// Records the delegate component URL and heap snapshot into a TLS "pending" slot.
[[gnu::visibility("default")]] void *qml_creationContext_hook(const void *self)
    __asm__("_ZNK13QQmlComponent15creationContextEv");

void *qml_creationContext_hook(const void *self)
{
    resolveFunctions();
    void *result = s_creationContextFn(self);
    QByteArray url = componentUrl(const_cast<void *>(self));
    if (!url.isEmpty()) {
        tls_pendingDelegateUrl   = url;
        tls_pendingDelegateHeap  = measureHeapSize();
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
        tls_delegateByIncubator[self] = { tls_pendingDelegateUrl, tls_pendingDelegateHeap };
        tls_pendingDelegateUrl.clear();
        tls_pendingDelegateHeap = 0;
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

    s_incubatorClearFn(self);

    // Skip if fine-grained QQmlObjectCreator hooks already tracked this delegate
    // (tls_initFromTypeFired is set by initFromType_hook in PLT-complete Qt builds like ARM/Yocto).
    if (hasDelegateInfo && obj != nullptr && !tls_initFromTypeFired) {
        const QByteArray &url = !objectUrl.isEmpty() ? objectUrl : info.url;
        if (!url.isEmpty()) {
            size_t heapAfter = measureHeapSize();
            objectTrack(url, true,  Delegate, {}, info.heapBefore);
            objectTrack(url, false, Delegate, {}, heapAfter);

            // The destroyed() signal fires from inside QObject::~QObject() before the
            // object's memory is returned to the allocator, so measureHeapSize() at that
            // point still includes the object. To produce a meaningful negative delta, we
            // fake the "before" heap as (heapNow + creationDelta), making the delta equal
            // to -creationDelta without inventing any allocation numbers.
            QByteArray capturedUrl = url;
            long long capturedDelta = static_cast<long long>(heapAfter)
                                    - static_cast<long long>(info.heapBefore);
            QObject::connect(obj, &QObject::destroyed, [capturedUrl, capturedDelta](QObject *) {
                size_t heapNow = measureHeapSize();
                size_t heapStart = static_cast<size_t>(static_cast<long long>(heapNow) + capturedDelta);
                objectTrack(capturedUrl, true,  Destroy, {}, heapStart);
                objectTrack(capturedUrl, false, Destroy, {}, heapNow);
            });
        }
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
    if (!s_componentCreateFn)
        return;

    // QQmlComponent::create(incubator) is called by Loader — NOT by delegate models.
    // Delegate models call incubateObject() (virtual, not PLT) instead. Erase any map entry
    // that was tentatively created by qml_incubatorCtor_hook for this Loader incubator.
    tls_delegateByIncubator.erase(incubator);
    tls_pendingDelegateUrl.clear();

    // If a delegate incubation is in progress (tls_delegateByIncubator is non-empty), this
    // create() call is for a style sub-object (e.g. Fusion Button.qml) loaded during delegate
    // construction — not a user-visible component. Skip tracking: the delegate's destroyed()
    // signal already handles the one-per-delegate accounting, and emitDestruction for the style
    // context is suppressed by tls_suppressEmitDestruction / tls_pendingDelegateDestroys.
    if (!tls_delegateByIncubator.empty()) {
        s_componentCreateFn(self, incubator, context, forContext);
        return;
    }

    QByteArray url = componentUrl(self);

    // Reset flag so we can detect if initFromType fires during this call
    tls_initFromTypeFired = false;
    size_t heapBefore = measureHeapSize();

    s_componentCreateFn(self, incubator, context, forContext);

    // Only track here if the fine-grained QQmlObjectCreator hooks did NOT fire.
    // If they fired, they already captured the creation with better granularity.
    if (!tls_initFromTypeFired && !url.isEmpty()) {
        size_t heapAfter = measureHeapSize();
        objectTrack(url, true,  ComponentCreate, {}, heapBefore);
        objectTrack(url, false, ComponentCreate, {}, heapAfter);
    }
}


// ── Heap measurement ────────────────────────────────────────────────────

static size_t measureHeapSize()
{
    struct mallinfo2 info = ::mallinfo2();
    return static_cast<size_t>(info.uordblks);
}

void objectTrack(const QByteArray &url, bool startOfCreation, TrackType trackType,
                 const QByteArray &parentUrl, size_t precomputedHeapSize)
{
    // File-scope state
    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    static int s_nestingLevel = 0;
    static int s_startCount = 0; // cumulative calls with startOfFunction=true
    static QStack<std::tuple<QByteArray, size_t>> s_creationStack;

    const size_t heapSize = (precomputedHeapSize != 0) ? precomputedHeapSize : measureHeapSize();
    QByteArray heapSizeDeltaStr;

    if (startOfCreation) {
        ++s_nestingLevel;
        ++s_startCount;
        s_creationStack.push({ url, heapSize });
        if (trackType != Destroy)
            s_trackedUrls.insert(url.toStdString());
    } else {
        if (s_creationStack.isEmpty()) {
            qWarning() << "QML-OBJECT-TRACK: unexpected end for component at" << url;
        } else {
            auto [urlBegin, heapSizeBegin] = s_creationStack.pop();
            if (urlBegin != url)
                qWarning() << "QML-OBJECT-TRACK: mismatched end for component:" << url << "expected:" << urlBegin;
            else {
                long long delta = static_cast<long long>(heapSize)
                                - static_cast<long long>(heapSizeBegin);
                heapSizeDeltaStr = QByteArray::number(delta);
            }
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

    const char *typeStr = trackType == Create          ? "create"
                        : trackType == Finalize        ? "finalize"
                        : trackType == Delegate        ? "delegate"
                        : trackType == ComponentCreate ? "component"
                        : "destroy";

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
