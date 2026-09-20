import QtQuick 2.15
import QtQuick.Controls 2.15

Item {
    id: root
    property string title: "Setting"
    property string description: ""
    property string tip: description
    implicitHeight: content.implicitHeight

    Column {
        id: content
        width: parent.width
        spacing: 4

        Row {
            width: parent.width
            spacing: 7

            Text {
                text: root.title
                color: "#EAF2FF"
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            Rectangle {
                width: 17
                height: 17
                radius: 9
                color: helpMouse.containsMouse ? "#22D3EE" : "#243348"
                border.color: helpMouse.containsMouse ? "#67E8F9" : "#41526A"

                Text {
                    anchors.centerIn: parent
                    text: "?"
                    color: helpMouse.containsMouse ? "#07111F" : "#A9B8CC"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }

                MouseArea {
                    id: helpMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.WhatsThisCursor
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 250
                    ToolTip.timeout: 10000
                    ToolTip.text: root.tip
                }
            }
        }

        Text {
            width: parent.width
            text: root.description
            color: "#8292A8"
            font.pixelSize: 11
            lineHeight: 1.18
            wrapMode: Text.WordWrap
        }
    }
}
