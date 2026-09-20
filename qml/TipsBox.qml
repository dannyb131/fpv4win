import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: tipsBox
    visible: false
    width: Math.min(messageText.implicitWidth + 36, parent ? parent.width - 48 : 520)
    height: messageText.implicitHeight + 24
    anchors.centerIn: parent
    radius: 10
    color: "#F01A293B"
    border.color: "#3B536F"
    border.width: 1

    property string tips: ""
    property int timeout: 3000

    function showPop(message, duration) {
        tips = message
        hideTimer.interval = duration || timeout
        visible = true
        hideTimer.restart()
    }

    function hide() {
        visible = false
        tips = ""
        hideTimer.stop()
    }

    Timer {
        id: hideTimer
        interval: tipsBox.timeout
        repeat: false
        onTriggered: tipsBox.hide()
    }

    Text {
        id: messageText
        anchors.centerIn: parent
        width: tipsBox.width - 32
        text: tipsBox.tips
        color: "#F2F7FF"
        font.pixelSize: 13
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }
}
