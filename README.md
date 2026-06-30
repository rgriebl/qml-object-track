`LD_PRELOAD` library for QML runtimes that reports how much heap each QML
component instantiation consumes.

```
cmake -B build && cmake --build build
LD_PRELOAD=build/libqml-object-track.so ./your-qt-app
```

By default it prints a short report to **stderr** when the app exits: the peak
and current malloc heap, and a tree of the QML files that contributed the most
heap, rooted at the file loaded into the engine. Each line shows the size
**inclusive** of children; `self` is the file's own heap excluding children.
Only the biggest entries are listed per level; the rest are folded into an
`others` bucket. Delegates appear as their own node under the view that uses
them (`Type [delegate]`).

```
==================== QML object-track report (final) - pid 12345 ====================
  malloc in-use now : +2.3 MB
  peak malloc in-use: +38.3 MB
  QML files tracked : 24   (showing top 10 per level)
  QML trackdown:   (sizes are inclusive of children; 'self' excludes them)
  MyApp/Main.qml              +31.9 MB  x1     self +5.1 MB
    MyApp/HomeScreen.qml      +18.4 MB  x1     self +0.5 MB
      Button [delegate]       +12.0 MB  x1000  self +8.0 MB
        <base components>     +4.0 MB   x1000
    ... others (12 files)     +2.6 MB
================================================================================
```

The final report fires on a graceful Qt quit (`QCoreApplication::aboutToQuit`)
or, failing that, at `atexit` — whichever happens first, and only once. It shows
the **peak** state: the peak malloc heap and the tree as it was at (close to) the
high-water mark, not the post-unload state at exit. A process that runs forever
should use timer mode; those interval reports show the live state at each tick. A
hard `_exit()` / `SIGKILL` bypasses the final report.

## Modes

Selected with `QML_OBJECT_TRACK_MODE`:

| mode               | behaviour                                                            |
|--------------------|----------------------------------------------------------------------|
| `report` (default) | one report at exit                                                   |
| `timer`            | a report every `INTERVAL` seconds, plus the final report at exit     |
| `detailed`         | like `report`, but also writes the per-instantiation CSV (see below) |

## Environment variables

| variable                    | default  | meaning                                              |
|-----------------------------|----------|------------------------------------------------------|
| `QML_OBJECT_TRACK_MODE`     | `report` | `report` / `timer` / `detailed`                      |
| `QML_OBJECT_TRACK_ITEMS`    | `10`     | max children shown per tree level (rest → `others`)  |
| `QML_OBJECT_TRACK_INTERVAL` | `5`      | seconds between reports; setting it implies `timer`  |
| `QML_OBJECT_TRACK_DEPTH`    | `0`      | max tree depth, `0` = unlimited                      |
| `QML_OBJECT_TRACK_MIN_SIZE` | `0`      | only list items whose inclusive size ≥ this; the rest fold into `others`. Accepts a `K`/`M`/`G` suffix (e.g. `1M`) |
| `QML_OBJECT_TRACK_CSV`      | off      | `1` to also write the detailed CSV in any mode       |
| `QML_OBJECT_TRACK_SUBCOMPONENTS` | off | set to break a delegate's separately-instantiated base components out as a `<base components>` child with their size |
| `QML_OBJECT_TRACK_DEBUG`    | off      | set to print per-hook invocation counts (to diagnose hook coverage on a given Qt build) |

The detailed CSV is written to `/tmp/qml-object-track.<pid>.csv`, one row per
component creation/destruction (nesting, type, url, heap size, delta, parent).

Heap deltas are measured with `mallinfo2()`, so they need a glibc allocator —
under jemalloc `mallinfo2` reads 0 and the report has nothing to show.

The report is the tree under the application root (the first file created).
A component whose parent can't be determined — typically something built by
deferred execution (a Controls indicator/popup created after its owner's
creation bracket has already closed, with an anonymous parent context) — is
**left out** of the report rather than shown at the top level where it would
look like a root.

## Sub-component sizing (`QML_OBJECT_TRACK_SUBCOMPONENTS`)

When set, a delegate's base components are broken out as a `<base components>`
child holding their total heap, so you can see how much of a delegate is the
delegate itself versus the parts it pulls in.

**The split is along `QQmlObjectCreator` boundaries, not the QML object tree.**
Each QML component instantiated through its own `QQmlObjectCreator` is bracketed
(constructor → destructor); the first creator of a delegate build is the
delegate root (its size becomes `self`), and every later creator is summed into
`<base components>`. Anything built *without* a separate creator stays in `self`.

This matters because of how the breakdown looks for a `delegate: ComboBox {}`:

```
ComboBox [delegate]  +90.3 MB  x2000  self +35.5 MB
  <base components>  +54.8 MB  x2000
```

- `<base components>` holds the ComboBox parts instantiated through their own
  creator (e.g. its **popup**).
- The ComboBox's `contentItem` / `background` / `indicator` are created by
  **deferred execution** during the delegate root's completion — inside the
  *root* creator's bracket, with no creator of their own — so they land in
  `self`, not in `<base components>`. Wrapping the ComboBox in an `Item` does
  **not** move them out of `self` for the same reason.

So on a build where only the coarse hooks fire (typical desktop Qt), `self` is
"the delegate root plus everything built inline/deferred under it" and
`<base components>` is "separately-instantiated child components". Individual
names aren't reachable there, hence the placeholder.

Where the fine-grained `QQmlObjectCreator` hooks fire (e.g. the embedded
target), this approximation isn't needed: every component — deferred or not — is
tracked per context with its real file name and nests automatically, so the
ComboBox and each of its parts show up as named children with their own sizes.

This project is licensed under the terms of the MIT license.
