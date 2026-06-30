#pragma once

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#include <malloc.h>
#include <unistd.h>
#include <dlfcn.h>

#include <QObject>
#include <QFile>
#include <QByteArray>
#include <QGuiApplication>
#include <QWindow>
#include <QQuickWindow>
#include <QPixmapCache>
#include <QQmlEngine>
#include <QtQml/qqml.h>
#include <QtDebug>

class MemoryHelper : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Read once at startup from env so the QML auto-test harness can self-trigger.
    Q_PROPERTY(bool autoTest READ autoTest CONSTANT)
    // When set, the harness exits right after the baseline dump (for heaptrack diff).
    Q_PROPERTY(bool autoTestBaseline READ autoTestBaseline CONSTANT)
    // When set, the harness binds the screen load to a dedicated jemalloc arena.
    Q_PROPERTY(bool arenaExperiment READ arenaExperiment CONSTANT)

public:
    explicit MemoryHelper(QObject *parent = nullptr) : QObject(parent) {}

    bool autoTest() const { return qEnvironmentVariableIsSet("QML_AUTOTEST"); }
    bool autoTestBaseline() const { return qEnvironmentVariableIsSet("QML_AUTOTEST_BASELINE"); }
    bool arenaExperiment() const { return qEnvironmentVariableIsSet("QML_ARENA"); }

    Q_INVOKABLE void trimHeap()
    {
        ::malloc_trim(0);
    }

    // ---- jemalloc arena control (resolved at runtime; no-op without jemalloc) ----
    // mallctl signature: int mallctl(const char *name, void *old, size_t *oldlen,
    //                                void *new, size_t newlen)
    using mallctl_fn = int (*)(const char *, void *, size_t *, void *, size_t);
    static mallctl_fn jeMallctl()
    {
        static mallctl_fn fn = reinterpret_cast<mallctl_fn>(dlsym(RTLD_DEFAULT, "mallctl"));
        return fn;
    }
    Q_INVOKABLE bool jeActive() { return jeMallctl() != nullptr; }

    // Create a fresh arena, return its index (-1 on failure / no jemalloc).
    Q_INVOKABLE int jeCreateArena()
    {
        auto m = jeMallctl();
        if (!m) return -1;
        unsigned aid = 0;
        size_t sz = sizeof(aid);
        if (m("arenas.create", &aid, &sz, nullptr, 0) != 0)
            return -1;
        qInfo() << "jemalloc: created scratch arena" << aid;
        return int(aid);
    }

    // Bind the calling (main/GUI) thread's future allocations to arena `id`.
    Q_INVOKABLE void jeBindArena(int id)
    {
        auto m = jeMallctl();
        if (!m || id < 0) return;
        unsigned aid = unsigned(id);
        m("thread.arena", nullptr, nullptr, &aid, sizeof(aid));
    }

    // Bind the calling thread back to the default arena 0.
    Q_INVOKABLE void jeUnbindArena()
    {
        auto m = jeMallctl();
        if (!m) return;
        unsigned aid = 0;
        m("thread.arena", nullptr, nullptr, &aid, sizeof(aid));
    }

    // Reclaim freed memory to the OS. jemalloc: flush tcache + force-purge ALL
    // arenas (index 4096 == MALLCTL_ARENAS_ALL). glibc: malloc_trim.
    Q_INVOKABLE void reclaim()
    {
        if (auto m = jeMallctl()) {
            m("thread.tcache.flush", nullptr, nullptr, nullptr, 0);
            m("arena.4096.purge", nullptr, nullptr, nullptr, 0);
        } else {
            ::malloc_trim(0);
        }
    }

    // jemalloc's own accounting (much more precise than smaps for jemalloc runs):
    //   allocated -> live bytes;  resident -> physical RSS jemalloc holds;
    //   retained  -> virtual returned to OS.  Per arena: pactive (live pages),
    //   pdirty/pmuzzy (freed pages awaiting purge), resident (physical).
    //   After unload+purge, a low scratch-arena pactive/resident == clean reclaim;
    //   high pactive with little allocated == pinned by scattered survivors.
    Q_INVOKABLE void jeDumpStats(const QString &label, int scratch = -1)
    {
        auto m = jeMallctl();
        if (!m) return;
        uint64_t e = 1; size_t esz = sizeof(e);
        m("epoch", &e, &esz, &e, sizeof(e));            // refresh snapshot

        auto rd = [&](const char *name) -> long long {
            size_t v = 0, sz = sizeof(v);
            return m(name, &v, &sz, nullptr, 0) == 0 ? (long long)v : -1;
        };
        qInfo().noquote().nospace()
            << "\n--- JEMALLOC STATS [" << label << "] ---\n"
            << "  allocated : " << rd("stats.allocated") / 1024 << " kB  (live)\n"
            << "  active    : " << rd("stats.active")    / 1024 << " kB\n"
            << "  resident  : " << rd("stats.resident")  / 1024 << " kB  (jemalloc RSS)\n"
            << "  mapped    : " << rd("stats.mapped")    / 1024 << " kB\n"
            << "  retained  : " << rd("stats.retained")  / 1024 << " kB  (returned to OS)";

        const long ps = sysconf(_SC_PAGESIZE);
        auto dumpArena = [&](int a) {
            if (a < 0) return;
            auto ar = [&](const char *leaf) -> long long {
                QByteArray n = "stats.arenas." + QByteArray::number(a) + '.' + leaf;
                size_t v = 0, sz = sizeof(v);
                return m(n.constData(), &v, &sz, nullptr, 0) == 0 ? (long long)v : -1;
            };
            qInfo().noquote().nospace()
                << "  arena " << a << ": pactive " << ar("pactive") * ps / 1024 << " kB"
                << ", pdirty " << ar("pdirty") * ps / 1024 << " kB"
                << ", pmuzzy " << ar("pmuzzy") * ps / 1024 << " kB"
                << ", resident " << ar("resident") / 1024 << " kB";
        };
        dumpArena(0);
        dumpArena(scratch);
    }

    // Compact the per-engine QRecyclePools (binding-guard + Q_PROPERTY trigger)
    // via the patched QQmlEngine::trimMemory(). Frees their fully-unused pages
    // back to the allocator; a following reclaim() returns them to the OS.
    // Compiled out unless built against the patched Qt (OBJECTTRACK_HAVE_TRIMMEMORY).
    Q_INVOKABLE void trimEngine()
    {
#ifdef OBJECTTRACK_HAVE_TRIMMEMORY
        if (QQmlEngine *e = qmlEngine(this))
            e->trimMemory();
#else
        qInfo("trimEngine: built without OBJECTTRACK_HAVE_TRIMMEMORY (unpatched Qt) - no-op");
#endif
    }

    // Release Qt Quick render-side caches so their backing heap can be trimmed:
    //   - QPixmapCache: cached QPixmaps (icons, images)
    //   - QQuickWindow::releaseResources(): scene-graph glyph/distance-field/
    //     texture caches not currently in use (processed on the render thread,
    //     so a malloc_trim must follow a frame later to reclaim the freed heap).
    Q_INVOKABLE void releaseCaches()
    {
        QPixmapCache::clear();
        const QList<QWindow *> windows = QGuiApplication::allWindows();
        for (QWindow *w : windows) {
            if (auto *qw = qobject_cast<QQuickWindow *>(w))
                qw->releaseResources();
        }
    }

    // Hard-exit without running any destructors. Used by the heaptrack run so
    // that "leaked at exit" == the live allocation set at this exact moment
    // (engine/app teardown would otherwise free the survivors first).
    Q_INVOKABLE void exitNow()
    {
        fflush(nullptr);
        ::_exit(0);
    }

    // Prints the breakdown that tells fragmentation apart from live/external memory:
    //   in-use        -> live glibc allocations (cannot be freed; something holds them)
    //   free-in-arena -> freed but trapped inside the arena (== fragmentation)
    //   mmap          -> large glibc allocations served by mmap (released on free)
    //   RSS-glibc     -> the remainder lives outside glibc malloc (V4 JS heap, GPU, .so, etc.)
    Q_INVOKABLE void dumpStats(const QString &label)
    {
        struct mallinfo2 mi = ::mallinfo2();
        const long rss = rssKb();
        const long glibcKb = (mi.arena + mi.hblkhd) / 1024;

        qInfo().noquote().nospace()
            << "\n=== MEM [" << label << "] ===\n"
            << "  RSS            : " << rss << " kB\n"
            << "  arena (brk)    : " << mi.arena / 1024 << " kB\n"
            << "  mmap (hblkhd)  : " << mi.hblkhd / 1024 << " kB\n"
            << "  in-use         : " << mi.uordblks / 1024 << " kB   (live allocations)\n"
            << "  free-in-arena  : " << mi.fordblks / 1024 << " kB   (fragmentation)\n"
            << "  top-releasable : " << mi.keepcost / 1024 << " kB\n"
            << "  glibc total    : " << glibcKb << " kB\n"
            << "  RSS - glibc    : " << (rss - glibcKb) << " kB   (V4/GPU/.so/etc - outside malloc)";
    }

    // Categorize resident memory (RSS) by mapping type via /proc/self/smaps, so
    // we can separate glibc heap from GPU/render memory from shared libraries.
    // This is the only reliable way to see where real-rendering RSS actually goes.
    Q_INVOKABLE void dumpRssBreakdown(const QString &label)
    {
        QFile f(QStringLiteral("/proc/self/smaps"));
        if (!f.open(QIODevice::ReadOnly))
            return;

        long heapBrk = 0, anon = 0, gpu = 0, lib = 0, font = 0, other = 0, total = 0;
        QByteArray path;          // path of the current mapping
        bool headerSeen = false;

        const QList<QByteArray> lines = f.readAll().split('\n');
        for (const QByteArray &line : lines) {
            // Mapping header: "addr-addr perms off dev inode  path"
            if (line.size() > 0 && (line[0] >= '0' && line[0] <= '9'
                                    || line[0] >= 'a' && line[0] <= 'f')
                    && line.contains('-') && line.contains(' ')) {
                // 6th whitespace-delimited field onward is the path (may be empty)
                const QList<QByteArray> f6 = line.simplified().split(' ');
                path = (f6.size() >= 6) ? f6[5] : QByteArray();
                headerSeen = true;
                continue;
            }
            if (!headerSeen || !line.startsWith("Rss:"))
                continue;
            const long kb = line.mid(4).simplified().split(' ').value(0).toLong();
            total += kb;
            if (path.contains("/dri/") || path.contains("renderD") || path.contains("/card")
                    || path.contains("nvidia") || path.contains("mali") || path.contains("kgsl"))
                gpu += kb;
            else if (path.endsWith(".so") || path.contains(".so."))
                lib += kb;
            else if (path.endsWith(".ttf") || path.endsWith(".ttc") || path.endsWith(".otf")
                     || path.endsWith(".qpf2") || path.endsWith(".pfb"))
                font += kb;
            else if (path.startsWith("[heap"))
                heapBrk += kb;   // glibc main arena (brk); with ARENA_MAX=1 == all malloc
            else if (path.isEmpty() || path.startsWith("[anon") || path.startsWith("[stack"))
                anon += kb;      // mmap'd anon: V4 JS heap/JIT, glibc large allocs, etc.
            else
                other += kb;
        }

        qInfo().noquote().nospace()
            << "\n--- RSS BREAKDOWN [" << label << "] ---\n"
            << "  [heap] glibc   : " << heapBrk << " kB   (brk arena; ==malloc with ARENA_MAX=1)\n"
            << "  anon mmap (V4) : " << anon  << " kB   (V4 JS heap/JIT + large mmap)\n"
            << "  gpu (/dev/dri) : " << gpu   << " kB\n"
            << "  shared libs    : " << lib   << " kB\n"
            << "  fonts          : " << font  << " kB\n"
            << "  other (binary) : " << other << " kB\n"
            << "  TOTAL RSS      : " << total << " kB";
    }

    static long rssKb()
    {
        QFile f(QStringLiteral("/proc/self/statm"));
        if (!f.open(QIODevice::ReadOnly))
            return -1;
        const QList<QByteArray> parts = f.readAll().split(' ');
        if (parts.size() < 2)
            return -1;
        return parts[1].toLong() * (sysconf(_SC_PAGESIZE) / 1024);
    }

    // Walks every writable anonymous mapping (the malloc heap + arenas) via
    // /proc/self/pagemap and counts which 4 kB pages are actually resident.
    // Then re-buckets those resident pages into 16 kB and 64 kB groups: a
    // bucket counts as resident if it holds >=1 resident 4 kB page. This
    // projects the RSS that would be retained for the SAME live-object scatter
    // if the kernel used larger pages (the target aarch64 kernel uses 64 kB).
    Q_INVOKABLE void projectPageReclaim(const QString &label)
    {
        const long PAGE = sysconf(_SC_PAGESIZE); // 4096 on this host
        QFile maps(QStringLiteral("/proc/self/maps"));
        QFile pm(QStringLiteral("/proc/self/pagemap"));
        if (!maps.open(QIODevice::ReadOnly) || !pm.open(QIODevice::ReadOnly))
            return;

        quint64 resident4k = 0;
        // track which 16k / 64k super-pages contain >=1 resident 4k page
        quint64 buckets16 = 0, buckets64 = 0;

        const QList<QByteArray> lines = maps.readAll().split('\n');
        for (const QByteArray &line : lines) {
            // Only writable anonymous regions: "rw-p" with no pathname.
            const int perm = line.indexOf(' ');
            if (perm < 0 || !line.mid(perm + 1, 4).startsWith("rw"))
                continue;
            // anonymous = nothing after the inode column; skip file/[stack]/[vvar] etc.
            // a quick filter: keep [heap] and pure-anon (line ends right after inode)
            const bool isHeap = line.contains("[heap]");
            const bool isAnon = !line.contains('/') && !line.contains('[');
            if (!isHeap && !isAnon)
                continue;

            const int dash = line.indexOf('-');
            bool ok1 = false, ok2 = false;
            const quint64 start = line.left(dash).toULongLong(&ok1, 16);
            const quint64 end = line.mid(dash + 1, perm - dash - 1).toULongLong(&ok2, 16);
            if (!ok1 || !ok2 || end <= start)
                continue;

            qint64 prev16 = -1, prev64 = -1;
            for (quint64 addr = start; addr < end; addr += PAGE) {
                const quint64 idx = addr / PAGE;
                pm.seek(idx * 8);
                quint64 entry = 0;
                if (pm.read(reinterpret_cast<char *>(&entry), 8) != 8)
                    break;
                if (!(entry & (1ULL << 63)))   // present bit
                    continue;
                resident4k++;
                const qint64 b16 = addr / (16 * 1024);
                const qint64 b64 = addr / (64 * 1024);
                if (b16 != prev16) { buckets16++; prev16 = b16; }
                if (b64 != prev64) { buckets64++; prev64 = b64; }
            }
        }

        qInfo().noquote().nospace()
            << "\n--- PAGE-RECLAIM PROJECTION [" << label << "] ---\n"
            << "  resident anon  @ 4 kB pages : " << (resident4k * 4) << " kB  (actual on this host)\n"
            << "  would retain   @16 kB pages : " << (buckets16 * 16) << " kB\n"
            << "  would retain   @64 kB pages : " << (buckets64 * 64) << " kB  (target aarch64)\n"
            << "  fragmentation penalty 64k   : " << ((buckets64 * 64) - (resident4k * 4)) << " kB extra";
    }
};
