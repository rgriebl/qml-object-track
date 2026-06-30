import QtQuick
import QtQuick.Controls

Rectangle {
    property alias listView: listView
    ListView {
        id: listView
        anchors.fill: parent
        model: 1000
        cacheBuffer: 10000
        delegate: Button {
            text: "Button " + index
        }
    }
}
