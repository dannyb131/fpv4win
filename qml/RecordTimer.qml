import QtQuick 2.15

Rectangle {
    id: recordTimer
    visible: false
    width: 72
    height: 30
    radius: 7
    color: "#D91D2939"
    border.color: "#5A708D"

    property double startTime: 0
    property double recordLen: 0

    Row {
        anchors.centerIn: parent
        spacing: 6
        Text {
            id: recordDot
            text: "●"
            color: "#FB7185"
            font.pixelSize: 12
        }
        Text {
            text: Math.floor(recordTimer.recordLen) + "s"
            color: "#FFFFFF"
            font.pixelSize: 12
            font.weight: Font.DemiBold
        }
    }

    Timer {
        id: pulseTimer
        interval: 600
        running: recordTimer.visible
        repeat: true
        onTriggered: recordDot.visible = !recordDot.visible
    }

    Timer {
        id: elapsedTimer
        interval: 100
        repeat: true
        onTriggered: recordTimer.recordLen = (new Date().getTime() - recordTimer.startTime) / 1000
    }

    function start() {
        visible = true
        startTime = new Date().getTime()
        recordLen = 0
        elapsedTimer.start()
    }

    function stop() {
        visible = false
        elapsedTimer.stop()
        recordDot.visible = true
    }
}
