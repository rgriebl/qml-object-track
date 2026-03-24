#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <QtCore/QXmlStreamReader>
#include <QtCore/QDebug>
#include <QtCore/QUrl>

#include <dlfcn.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <mutex>


static void objectTrack(void *qmlComponentPtr, const QByteArray &url, bool startOfCreation);


typedef void *(*beginCreate_fn_t)(void *self, void *context);
typedef void *(*completeCreate_fn_t)(void *self);
/* QUrl QQmlComponent::url() const – Itanium ABI: hidden return pointer first */
typedef void  (*url_fn_t)(QUrl *ret, const void *self);


static beginCreate_fn_t    s_beginFn    = nullptr;
static completeCreate_fn_t s_completeFn = nullptr;
static url_fn_t            s_urlFn      = nullptr;


/* Resolve the real symbols exactly once. */
static void resolve_original(void)
{
    if (__builtin_expect(s_beginFn != nullptr, 1))
        return;

    *(void **)(&s_beginFn) = dlsym(
        RTLD_NEXT,
        "_ZN13QQmlComponent11beginCreateEP11QQmlContext"
    );

    if (!s_beginFn) {
        fprintf(stderr,
            "qml_hook: fatal - could not resolve "
            "QQmlComponent::beginCreate via RTLD_NEXT: %s\n",
            dlerror());
        abort();
    }

    *(void **)(&s_completeFn) = dlsym(
        RTLD_NEXT,
        "_ZN13QQmlComponent14completeCreateEv"
    );

    if (!s_completeFn) {
        fprintf(stderr,
            "qml_hook: fatal - could not resolve "
            "QQmlComponent::completeCreate via RTLD_NEXT: %s\n",
            dlerror());
        abort();
    }

    *(void **)(&s_urlFn) = dlsym(
        RTLD_NEXT,
        "_ZNK13QQmlComponent3urlEv"
    );

    if (!s_urlFn) {
        fprintf(stderr,
            "qml_hook: fatal - could not resolve "
            "QQmlComponent::url via RTLD_NEXT: %s\n",
            dlerror());
        abort();
    }
}

static QByteArray componentUrl(void *self)
{
    QUrl url;
    s_urlFn(&url, self);
    return url.toString().toUtf8();
}


/* --------------------------------------------------------------------------
 * The interposed function.
 *
 * The __asm__ label exports this C function under the C++ mangled name so
 * the dynamic linker satisfies references to QQmlComponent::beginCreate
 * with our implementation instead of Qt's.
 *
 * Signature in "C" terms (Itanium ABI: implicit this as first argument):
 *   void *hook(void *self, void *context)
 * -------------------------------------------------------------------------- */
__attribute__((visibility("default")))
void *qml_beginCreate_hook(void *self, void *context)
    __asm__("_ZN13QQmlComponent11beginCreateEP11QQmlContext");

void *qml_beginCreate_hook(void *self, void *context)
{
    resolve_original();

    objectTrack(self, componentUrl(self), true);

    return s_beginFn(self, context);
}

/* --------------------------------------------------------------------------
 * Hook for QQmlComponent::completeCreate()
 *
 * Mangled symbol: _ZN13QQmlComponent14completeCreateEv
 * Signature: void completeCreate(void *self)
 * -------------------------------------------------------------------------- */
__attribute__((visibility("default")))
void qml_completeCreate_hook(void *self)
    __asm__("_ZN13QQmlComponent14completeCreateEv");

void qml_completeCreate_hook(void *self)
{
    resolve_original();

    s_completeFn(self);

    objectTrack(self, componentUrl(self), false);
}

// File-scope state
static std::mutex  s_mutex;
static int         s_nestingLevel = 0;
static int         s_startCount   = 0;   // cumulative calls with startOfFunction=true
static FILE       *s_csvFile      = nullptr;
static QList<void *> s_creationStack;

/* --------------------------------------------------------------------------
 * Call malloc_info(3) into an in-memory buffer, parse the XML with
 * QXmlStreamReader, and return the total current heap size in bytes.
 * -------------------------------------------------------------------------- */
static size_t measureHeapSize()
{
    char  *buf  = nullptr;
    size_t size = 0;

    FILE *memf = open_memstream(&buf, &size);
    if (!memf)
        return 0;

    malloc_info(0, memf);
    fclose(memf);   // flushes and finalises buf

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

    qWarning() << "CURRENT HEAPSIZE:" << totalHeapSize;

    free(buf);
    return totalHeapSize;
}

void objectTrack(void *qmlComponentPtr, const QByteArray &url, bool startOfCreation)
{
    std::lock_guard<std::mutex> lock(s_mutex);

    if (startOfCreation) {
        ++s_nestingLevel;
        ++s_startCount;
        s_creationStack.push_back(qmlComponentPtr);
    } else {
        if (!s_creationStack.isEmpty() && s_creationStack.last() == qmlComponentPtr)
            s_creationStack.pop_back();
        else
            qWarning() << "Mismatched end of creation for component at" << url;
    }

    // Snapshot URL and heap size while holding the lock
    const size_t heapSize  = measureHeapSize();

    // Lazy-open the CSV file on the first write
    if (!s_csvFile) {
        char filename[64];
        std::snprintf(filename, sizeof(filename), "/tmp/qml-object-track.%d.csv", getpid());
        s_csvFile = std::fopen(filename, "w");
        if (!s_csvFile)
            qFatal("Qml-Object-Tracker: could not open %s for writing", filename);
    }

    if (s_csvFile) {
        const char bracket = startOfCreation ? '>' : '<';
        for (int i = 0; i < s_nestingLevel; ++i)
            std::fputc(bracket, s_csvFile);

        std::fprintf(s_csvFile, ",%s,%zu,%d,%d\n",
                     url.constData(),
                     heapSize,
                     s_nestingLevel,
                     s_startCount);
        std::fflush(s_csvFile);
    }

    if (!startOfCreation)
        --s_nestingLevel;
}
