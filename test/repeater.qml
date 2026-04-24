import QtQuick

Rectangle {
    Flickable {
        id: flickable
        anchors.fill: parent

        contentWidth: column.width
        contentHeight: column.height

        Column {
            id: column
            width: flickable.width
            Repeater {
                id: repeater
                model: 100
                delegate: Rectangle {
                    width: 100
                    height: 100
                    color: Qt.rgba(Math.random(), Math.random(), Math.random(), 1)
                    border.color: "black"
                    border.width: 1
                }
            }
        }
    }
}
