import QtQuick
import QtQuick.Controls

Rectangle {
    ListView {
        anchors.fill: parent
        model: 1000
        cacheBuffer: 10000
        delegate: Button {
            text: "Button " + index
        }
    }
}
