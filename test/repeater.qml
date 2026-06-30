import QtQuick
import QtQuick.Controls

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
                model: 1000
                delegate: ComboBox {
                    model: ["Combo " + index]
                }
            }
        }
    }
}
