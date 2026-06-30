import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import ObjectTrackTest

ApplicationWindow {
    visible: true
    width: 640
    height: 480
    title: qsTr("Hello World")

    TabBar {
        id: bar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        TabButton {
            text: "List View"
        }

        TabButton {
            text: "Repeater"
        }
    }

    // ----------------------------------------------------------------------
    // Headless auto-test harness. Enabled with QML_AUTOTEST=1 in the env.
    // Drives the heavy ComboBox repeater (loader2) through repeated
    // load / unload / gc+trim cycles and dumps the memory breakdown at each
    // phase so we can see what the retained memory actually is.
    // ----------------------------------------------------------------------
    property int testStep: 0
    property int scratchArena: -1   // jemalloc arena bound around the screen load
    property bool warmedUp: false

    // Let the window fully map/compose and the GL/Mesa render path allocate its
    // presentation buffers (~40 MB of anon that only appears once the window is
    // actually presented on the active desktop) BEFORE measuring baseline, so
    // "baseline" reflects the real settled idle RSS rather than a half-rendered
    // window. Without this the baseline is under-reported and every offset is
    // inflated by the not-yet-allocated render memory.
    Timer {
        id: warmUpTimer
        interval: 4000
        repeat: false
        running: MemoryHelper.autoTest
        onTriggered: warmedUp = true
    }

    Timer {
        id: autoTestTimer
        interval: 1500
        repeat: true
        running: MemoryHelper.autoTest && warmedUp
        onTriggered: runTestStep()
    }

    // Pages the ListView top-to-bottom repeatedly so delegates are materialized
    // and destroyed (those scrolling out beyond cacheBuffer) -> the create/destroy
    // churn that fragments the heap. Multiple passes approximate heavy manual
    // scrolling. Resumes the main sequence when all passes are done.
    property int scrollPass: 0
    readonly property int scrollPasses: 3

    Timer {
        id: scrollTimer
        interval: 20
        repeat: true
        onTriggered: {
            var lv = loader.item ? loader.item.listView : null
            if (!lv || lv.contentHeight <= lv.height) {
                scrollTimer.stop()
                autoTestTimer.start()
                return
            }
            if (lv.contentY + lv.height >= lv.contentHeight - 1) {
                scrollPass++
                if (scrollPass >= scrollPasses) {
                    scrollTimer.stop()
                    autoTestTimer.start()   // resume main sequence
                    return
                }
                lv.contentY = 0             // next pass from the top
                return
            }
            lv.contentY = Math.min(lv.contentY + lv.height, lv.contentHeight - lv.height)
        }
    }

    // Manual "Deep Trim": releaseCaches() schedules scene-graph release on the
    // render thread, so wait a frame before gc + malloc_trim, then report.
    Timer {
        id: deepTrimTimer
        interval: 300
        onTriggered: {
            gc()
            MemoryHelper.reclaim()
            MemoryHelper.dumpStats("after deep trim")
            MemoryHelper.dumpRssBreakdown("after deep trim")
            MemoryHelper.jeDumpStats("after deep trim", scratchArena)
        }
    }

    function runTestStep() {
        switch (testStep) {
        case 0:
            bar.currentIndex = 0          // show the list tab so it lays out
            MemoryHelper.dumpStats("baseline")
            MemoryHelper.dumpRssBreakdown("baseline")
            MemoryHelper.jeDumpStats("baseline", scratchArena)
            if (MemoryHelper.arenaExperiment && MemoryHelper.jeActive())
                scratchArena = MemoryHelper.jeCreateArena()
            if (MemoryHelper.autoTestBaseline)
                MemoryHelper.exitNow()    // heaptrack diff baseline: infra only
            break
        case 1:
            bar.currentIndex = 0          // list tab visible
            if (scratchArena >= 0)
                MemoryHelper.jeBindArena(scratchArena)  // route the load into scratch arena
            loader.active = true          // load listview.qml -> renders
            break
        case 2:
            autoTestTimer.stop()          // pause; scrollTimer resumes us at the end
            scrollTimer.start()           // scroll the (visible) list -> renders delegates
            break
        case 3:
            MemoryHelper.dumpStats("list loaded + scrolled")
            break
        case 4:
            bar.currentIndex = 1          // SWITCH to repeater tab so it actually renders
            loader2.active = true         // load repeater.qml -> renders 1000 ComboBox
            break
        case 5:
            // give the repeater a tick to render before measuring the peak
            break
        case 6:
            MemoryHelper.dumpStats("both loaded + rendered")
            MemoryHelper.dumpRssBreakdown("both loaded + rendered")
            MemoryHelper.jeDumpStats("both loaded + rendered", scratchArena)
            MemoryHelper.projectPageReclaim("both loaded + rendered")
            if (scratchArena >= 0)
                MemoryHelper.jeUnbindArena()   // cleanup allocations go to arena 0
            break
        case 7:
            loader.active = false         // unload both
            loader2.active = false
            break
        case 8:
            gc()
            MemoryHelper.reclaim()        // malloc_trim (glibc) or purge-all (jemalloc)
            break
        case 9:
            MemoryHelper.dumpStats("unloaded+gc+trim")
            MemoryHelper.dumpRssBreakdown("unloaded+gc+trim")
            MemoryHelper.jeDumpStats("unloaded+gc+reclaim", scratchArena)
            MemoryHelper.projectPageReclaim("unloaded+gc+trim")
            break
        case 10:
            MemoryHelper.releaseCaches()  // QPixmapCache + scene-graph resources
            MemoryHelper.trimEngine()     // compact QRecyclePools (patched Qt)
            gc()                          // render thread processes release next frame
            break
        case 11:
            MemoryHelper.reclaim()        // reclaim what releaseCaches freed
            break
        case 12:
            MemoryHelper.dumpStats("unloaded+DEEP trim")
            MemoryHelper.dumpRssBreakdown("unloaded+DEEP trim")
            MemoryHelper.jeDumpStats("unloaded+DEEP trim", scratchArena)
            MemoryHelper.projectPageReclaim("unloaded+DEEP trim")
            MemoryHelper.exitNow()   // freeze live set here for heaptrack (no teardown)
            return
        }
        testStep++
    }

    StackLayout {
        anchors.top: bar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        currentIndex: bar.currentIndex

            Rectangle {
                color: "lightblue"
                Loader {
                    id: loader
                    active: false
                    anchors.fill: parent
                    source: "listview.qml"
                    asynchronous: false
                }

                Row {
                    anchors.centerIn: parent

                    Button {
                        text: loader.active ? "Unload" : "Load"
                        onClicked: {
                            if (loader.active) {
                                loader.active = false
                            } else {
                                loader.active = true
                            }
                        }
                    }

                    Button {
                        text: "Force GC + Trim"
                        onClicked: {
                            gc()
                            MemoryHelper.reclaim()   // malloc_trim (glibc) / purge (jemalloc)
                        }
                    }

                    Button {
                        text: "Deep Trim"
                        onClicked: {
                            MemoryHelper.releaseCaches()
                            MemoryHelper.trimEngine()
                            gc()
                            deepTrimTimer.restart()   // trims + dumps after a frame
                        }
                    }

                    Button {
                        text: "Dump Mem"
                        onClicked: {
                            MemoryHelper.dumpStats("manual")
                            MemoryHelper.dumpRssBreakdown("manual")
                        }
                    }
                }
            }

            Rectangle {
                color: "lightgreen"
                Loader {
                    id: loader2
                    active: false
                    anchors.fill: parent
                    source: "repeater.qml"
                }
                Row {
                    anchors.centerIn: parent

                    Button {
                        text: loader2.active ? "Unload" : "Load"
                        onClicked: {
                            if (loader2.active) {
                                loader2.active = false
                            } else {
                                loader2.active = true
                            }
                        }
                    }

                    Button {
                        text: "Force GC + Trim"
                        onClicked: {
                            gc()
                            MemoryHelper.reclaim()   // malloc_trim (glibc) / purge (jemalloc)
                        }
                    }

                    Button {
                        text: "Deep Trim"
                        onClicked: {
                            MemoryHelper.releaseCaches()
                            MemoryHelper.trimEngine()
                            gc()
                            deepTrimTimer.restart()   // trims + dumps after a frame
                        }
                    }

                    Button {
                        text: "Dump Mem"
                        onClicked: {
                            MemoryHelper.dumpStats("manual")
                            MemoryHelper.dumpRssBreakdown("manual")
                        }
                    }
                }
            }
    }
}
