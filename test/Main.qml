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

                Button {
                    text: loader.active ? "Unload" : "Load"
                    anchors.centerIn: parent
                    onClicked: {
                        if (loader.active) {
                            loader.active = false
                            // Two-wave deleteLater() chain:
                            //   Iteration 1: Loader's root item deleted → ListView/DelegateModel
                            //                destructor queues deleteLater() for each delegate.
                            //   Iteration 2: All 1000 delegate C++ objects actually freed.
                            // One Qt.callLater only covers iteration 1. Nest two to reach
                            // iteration 2 before gc + trim run.
                            Qt.callLater(function() {
                                Qt.callLater(function() {
                                    gc()
                                    MemoryHelper.trimHeap()
                                })
                            })
                        } else {
                            loader.active = true
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
                Button {
                    text: loader2.active ? "Unload" : "Load"
                    anchors.centerIn: parent
                    onClicked: {
                        if (loader2.active) {
                            loader2.active = false
                            Qt.callLater(function() {
                                Qt.callLater(function() {
                                    gc()
                                    MemoryHelper.trimHeap()
                                })
                            })
                        } else {
                            loader2.active = true
                        }
                    }
                }
            }
    }
}
