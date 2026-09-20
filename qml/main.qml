import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Qt.labs.platform 1.1
import realTimePlayer 1.0

ApplicationWindow {
    id: window
    visible: true
    width: 1280
    height: 820
    minimumWidth: 980
    minimumHeight: 660
    title: qsTr("fpv4win · OpenIPC ground station")
    color: "#08111F"

    property bool receiverRunning: false
    property bool videoActive: false
    property bool controlsVisible: true
    property string bitrateText: "Waiting for video"

    palette.window: "#08111F"
    palette.windowText: "#EAF2FF"
    palette.base: "#111D2E"
    palette.text: "#EAF2FF"
    palette.button: "#17263A"
    palette.buttonText: "#EAF2FF"
    palette.highlight: "#16CBE3"
    palette.highlightedText: "#07111F"

    function notify(message, duration) {
        tips.showPop(message, duration || 3000)
    }

    FileDialog {
        id: fileDialog
        title: "Select the matching OpenIPC key"
        nameFilters: ["WFB key files (*.key)", "All files (*)"]
        onAccepted: {
            var selectedPath = file.toString().replace("file:///", "")
            keySelector.text = decodeURIComponent(selectedPath)
        }
    }

    TipsBox { id: tips; z: 1000; tips: "" }

    Shortcut {
        sequence: "Ctrl+Shift+M"
        onActivated: window.controlsVisible = !window.controlsVisible
    }

    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 68
        color: "#0C1727"
        border.color: "#1A2A40"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 12

            Image {
                source: "qrc:/assets/fpv4win.png"
                sourceSize.width: 44
                sourceSize.height: 44
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
            Column {
                Layout.fillWidth: true
                spacing: 2
                Text { text: "fpv4win"; color: "#F7FAFF"; font.pixelSize: 20; font.weight: Font.Bold }
                Text { text: "OpenIPC video and telemetry ground station"; color: "#7F91AA"; font.pixelSize: 11 }
            }
            Button {
                id: controlsToggle
                text: window.controlsVisible ? "Hide controls" : "Show controls"
                font.pixelSize: 11
                ToolTip.visible: hovered
                ToolTip.text: "Collapse or restore the setup and monitor panel (Ctrl+Shift+M)."
                onClicked: window.controlsVisible = !window.controlsVisible
            }
            Rectangle {
                Layout.preferredWidth: statusText.implicitWidth + 28
                Layout.preferredHeight: 30
                radius: 15
                color: window.receiverRunning ? "#123B35" : "#17263A"
                border.color: window.receiverRunning ? "#2DD4BF" : "#33445B"
                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8; height: 8; radius: 4
                        color: window.receiverRunning ? "#34D399" : "#64748B"
                    }
                    Text {
                        id: statusText
                        text: window.receiverRunning ? "Receiver running" : "Receiver stopped"
                        color: window.receiverRunning ? "#A7F3D0" : "#B2C0D3"
                        font.pixelSize: 12; font.weight: Font.DemiBold
                    }
                }
            }
        }
    }

    RowLayout {
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 14
        spacing: 14

        Rectangle {
            id: videoPanel
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 520
            radius: 12
            color: "#02060B"
            border.color: "#1B2B40"
            clip: true

            QQuickRealTimePlayer {
                id: player
                anchors.fill: parent
                property var playingFile
                Component.onCompleted: {
                    NativeApi.onRtpStream.connect(function(sdpFile) {
                        playingFile = sdpFile
                        window.videoActive = true
                        play(sdpFile)
                    })
                    onPlayStopped.connect(function() {
                        if (window.receiverRunning && playingFile) {
                            stop()
                            play(playingFile)
                        }
                    })
                    onBitrate.connect(function(bitrate) {
                        if (bitrate > 1000000)
                            window.bitrateText = Number(bitrate / 1000000).toFixed(2) + " Mbps"
                        else if (bitrate > 1000)
                            window.bitrateText = Number(bitrate / 1000).toFixed(0) + " Kbps"
                        else
                            window.bitrateText = bitrate + " bps"
                    })
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 10
                visible: !window.videoActive
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 64; height: 64; radius: 32
                    color: "#102136"; border.color: "#24415F"
                    Text { anchors.centerIn: parent; text: "▶"; color: "#25D5EA"; font.pixelSize: 24 }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: window.receiverRunning ? "Waiting for the first video frame" : "Your live view will appear here"
                    color: "#DCE8F7"; font.pixelSize: 18; font.weight: Font.DemiBold
                }
                Text {
                    width: 420
                    horizontalAlignment: Text.AlignHCenter
                    text: window.receiverRunning
                          ? "Check that the channel, width, key and codec match the air unit."
                          : "Choose your radio settings on the right, then select Start receiver."
                    color: "#7689A2"; font.pixelSize: 12; wrapMode: Text.WordWrap
                }
            }

            Rectangle {
                anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 14
                width: liveText.implicitWidth + 24; height: 28; radius: 14
                color: "#B20B1421"
                border.color: window.videoActive ? "#25D5EA" : "#33445B"
                Text {
                    id: liveText
                    anchors.centerIn: parent
                    text: window.videoActive ? "LIVE · " + window.bitrateText : "LIVE VIEW"
                    color: window.videoActive ? "#67E8F9" : "#91A2B8"
                    font.pixelSize: 11; font.weight: Font.Bold
                }
            }

            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: 58
                color: "#D909121F"; border.color: "#25374C"
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 14; spacing: 10
                    Text {
                        Layout.fillWidth: true
                        text: window.videoActive ? "Hardware decoding is selected automatically when supported" : "Low-latency H.264 / H.265 receiver"
                        color: "#8FA0B6"; font.pixelSize: 11; elide: Text.ElideRight
                    }
                    Button {
                        text: "Capture JPG"
                        ToolTip.visible: hovered
                        ToolTip.text: "Save the current frame as a JPEG image."
                        onClicked: {
                            var fileName = player.captureJpeg()
                            window.notify(fileName !== "" ? "Saved " + fileName : "Capture failed", 3500)
                        }
                    }
                    Button {
                        id: recordButton
                        text: recordTimer.started ? "Stop recording" : "Record MP4"
                        ToolTip.visible: hovered
                        ToolTip.text: "Record the received video without re-encoding it."
                        onClicked: recordTimer.clickEvent()
                    }
                    RecordTimer {
                        id: recordTimer
                        Layout.preferredWidth: started ? 70 : 0
                        Layout.preferredHeight: 30
                        property bool started: false
                        function clickEvent() {
                            if (!started) {
                                started = player.startRecord()
                                if (started) start()
                                else window.notify("Recording could not start", 3500)
                            } else {
                                started = false
                                var fileName = player.stopRecord()
                                stop()
                                window.notify(fileName !== "" ? "Saved " + fileName : "Recording failed", 3500)
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            id: sidePanel
            visible: window.controlsVisible
            Layout.preferredWidth: window.controlsVisible ? Math.min(390, window.width * 0.36) : 0
            Layout.minimumWidth: window.controlsVisible ? 340 : 0
            Layout.fillHeight: true
            radius: 12
            color: "#0D1828"
            border.color: "#1B2B40"
            clip: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    background: Rectangle { color: "#0A1422" }
                    TabButton { text: "SETUP"; font.pixelSize: 12; font.weight: Font.DemiBold }
                    TabButton { text: "MONITOR"; font.pixelSize: 12; font.weight: Font.DemiBold }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: tabs.currentIndex

                    ScrollView {
                        id: setupScroll
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        Column {
                            width: setupScroll.availableWidth
                            padding: 14
                            spacing: 12

                            Rectangle {
                                width: parent.width - 28
                                height: radioContent.implicitHeight + 28
                                radius: 10; color: "#111F31"; border.color: "#20344D"
                                Column {
                                    id: radioContent
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
                                    spacing: 10
                                    Text { text: "1  RADIO LINK"; color: "#58DDEF"; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1 }
                                    SettingHelp {
                                        width: parent.width
                                        title: "Wi-Fi adapter"
                                        description: "The RTL8812AU receiver connected by USB."
                                        tip: "Install the adapter with Zadig using the WinUSB driver, reconnect it, then choose its VID:PID here. If this list is empty, close other programs using the adapter."
                                    }
                                    ComboBox {
                                        id: selectDev
                                        width: parent.width
                                        model: ListModel { id: dongleModel }
                                        Component.onCompleted: {
                                            var dongles = NativeApi.GetDongleList()
                                            for (var i = 0; i < dongles.length; i++) dongleModel.append({ text: dongles[i] })
                                            currentIndex = dongles.length > 0 ? 0 : -1
                                        }
                                    }

                                    Row {
                                        width: parent.width; spacing: 10
                                        Column {
                                            width: (parent.width - 10) / 2; spacing: 6
                                            SettingHelp {
                                                width: parent.width
                                                title: "Channel"
                                                description: "Must match the air unit."
                                                tip: "Use the exact Wi-Fi channel configured in OpenIPC. Channel 161 is common, but your air-unit setting is the source of truth. A mismatch gives packet counts of zero."
                                            }
                                            ComboBox {
                                                id: selectChannel
                                                width: parent.width
                                                model: ["1","2","3","4","5","6","7","8","9","10","11","12","13","32","36","40","44","48","52","56","60","64","68","96","100","104","108","112","116","120","124","128","132","136","140","144","149","153","157","161","169","173","177"]
                                                currentIndex: 39
                                                Component.onCompleted: {
                                                    var saved = NativeApi.GetConfig()["config.channel"]
                                                    if (saved && saved !== "") currentIndex = model.indexOf(saved)
                                                }
                                            }
                                        }
                                        Column {
                                            width: (parent.width - 10) / 2; spacing: 6
                                            SettingHelp {
                                                width: parent.width
                                                title: "Channel width"
                                                description: "Must also match the air unit."
                                                tip: "Choose the same bandwidth configured on the camera. 20 MHz is the safest starting point. 5 and 10 MHz modes require matching OpenIPC and adapter support."
                                            }
                                            ComboBox {
                                                id: selectBw
                                                width: parent.width
                                                model: ["20 MHz", "40 MHz", "80 MHz", "160 MHz", "80+80 MHz", "5 MHz", "10 MHz", "Maximum"]
                                                currentIndex: 0
                                                Component.onCompleted: {
                                                    var saved = NativeApi.GetConfig()["config.channelWidth"]
                                                    if (saved && saved !== "") currentIndex = Number(saved)
                                                }
                                            }
                                        }
                                    }

                                    SettingHelp {
                                        width: parent.width
                                        title: "Video codec"
                                        description: "Auto detects modern OpenIPC H.264 and H.265 streams."
                                        tip: "Leave this on Auto first. If packets increase but the picture remains blank, select the codec configured on the camera—normally H.265 on newer OpenIPC firmware."
                                    }
                                    ComboBox {
                                        id: selectCodec
                                        width: parent.width
                                        model: ["AUTO", "H264", "H265"]
                                        currentIndex: 0
                                        Component.onCompleted: {
                                            var saved = NativeApi.GetConfig()["config.codec"]
                                            if (saved && saved !== "") currentIndex = model.indexOf(saved)
                                        }
                                    }

                                    SettingHelp {
                                        width: parent.width
                                        title: "Encryption key"
                                        description: "The gs.key paired with your air unit."
                                        tip: "Choose the same gs.key installed on the OpenIPC camera. Video and MAVLink both depend on it. A wrong key can show Wi-Fi/WFB traffic but no usable video or telemetry."
                                    }
                                    Button {
                                        id: keySelector
                                        width: parent.width
                                        text: "gs.key"
                                        onClicked: fileDialog.open()
                                        Component.onCompleted: {
                                            var saved = NativeApi.GetConfig()["config.key"]
                                            if (saved && saved !== "") text = saved
                                        }
                                    }

                                    Button {
                                        id: receiverButton
                                        width: parent.width; height: 46
                                        text: window.receiverRunning ? "STOP RECEIVER" : "START RECEIVER"
                                        font.pixelSize: 13; font.weight: Font.Bold
                                        background: Rectangle {
                                            radius: 8
                                            color: window.receiverRunning ? (receiverButton.down ? "#7F1D1D" : "#9F2E35") : (receiverButton.down ? "#0891A5" : "#16CBE3")
                                        }
                                        contentItem: Text {
                                            text: receiverButton.text
                                            color: window.receiverRunning ? "#FFF1F2" : "#05121B"
                                            font: receiverButton.font
                                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                        }
                                        ToolTip.visible: hovered
                                        ToolTip.text: window.receiverRunning ? "Stop receiving video and telemetry." : "Save these settings and begin listening for the air unit."
                                        onClicked: {
                                            if (!window.receiverRunning) {
                                                if (selectDev.currentIndex < 0) {
                                                    window.notify("No RTL8812AU adapter was found", 4500)
                                                    return
                                                }
                                                window.receiverRunning = NativeApi.Start(selectDev.currentText,
                                                                                       Number(selectChannel.currentText),
                                                                                       Number(selectBw.currentIndex),
                                                                                       keySelector.text,
                                                                                       selectCodec.currentText)
                                                if (!window.receiverRunning) window.notify("Receiver could not start. Check the Monitor log.", 4500)
                                            } else {
                                                NativeApi.Stop()
                                                player.stop()
                                                window.videoActive = false
                                                window.bitrateText = "Waiting for video"
                                                if (recordTimer.started) recordTimer.clickEvent()
                                            }
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                width: parent.width - 28
                                height: streamContent.implicitHeight + 28
                                radius: 10; color: "#111F31"; border.color: "#20344D"
                                Column {
                                    id: streamContent
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
                                    spacing: 10
                                    Text { text: "2  SHARE VIDEO"; color: "#58DDEF"; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1 }
                                    SettingHelp {
                                        width: parent.width
                                        title: "Stream destination"
                                        description: "Forward the live feed without re-encoding it."
                                        tip: "For VLC on this PC use the HTTP preset and open http://127.0.0.1:8080/stream.ts. For Mission Planner use the UDP preset, then configure its GStreamer source for UDP port 5601. Start the receiver before starting the stream."
                                    }
                                    TextField {
                                        id: streamUrl
                                        width: parent.width
                                        text: "http://0.0.0.0:8080/stream.ts"
                                        placeholderText: "Streaming URL"
                                        selectByMouse: true
                                        font.pixelSize: 11
                                    }
                                    Row {
                                        width: parent.width; spacing: 8
                                        Button {
                                            width: (parent.width - 8) / 2
                                            text: "VLC / network"
                                            ToolTip.visible: hovered
                                            ToolTip.text: "Host an HTTP stream. Use 127.0.0.1 from this PC, or this PC's LAN address from another device."
                                            onClicked: streamUrl.text = "http://0.0.0.0:8080/stream.ts"
                                        }
                                        Button {
                                            width: (parent.width - 8) / 2
                                            text: "Mission Planner"
                                            ToolTip.visible: hovered
                                            ToolTip.text: "Send MPEG-TS to Mission Planner on UDP port 5601."
                                            onClicked: streamUrl.text = "udp://127.0.0.1:5601?pkt_size=1316"
                                        }
                                    }
                                    Button {
                                        id: streamButton
                                        width: parent.width
                                        property bool streaming: false
                                        text: streaming ? "STOP STREAM" : "START STREAM"
                                        font.weight: Font.DemiBold
                                        Component.onCompleted: {
                                            player.onStreamStopped.connect(function(error) {
                                                streaming = false
                                                if (error !== "") window.notify(error, 6000)
                                            })
                                        }
                                        onClicked: {
                                            if (streaming) {
                                                player.stopStream()
                                                return
                                            }
                                            var error = player.startStream(streamUrl.text)
                                            if (error === "") {
                                                streaming = true
                                                window.notify("Streaming started", 3000)
                                            } else window.notify(error, 6000)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    ScrollView {
                        id: monitorScroll
                        clip: true
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        Column {
                            width: monitorScroll.availableWidth
                            padding: 14
                            spacing: 12

                            Rectangle {
                                width: parent.width - 28
                                height: healthContent.implicitHeight + 28
                                radius: 10; color: "#111F31"; border.color: "#20344D"
                                Column {
                                    id: healthContent
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
                                    spacing: 10
                                    Text { text: "LINK HEALTH"; color: "#58DDEF"; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1 }
                                    Text {
                                        width: parent.width
                                        text: "The counters should rise from left to right as packets pass each stage."
                                        color: "#8292A8"; font.pixelSize: 11; wrapMode: Text.WordWrap
                                    }
                                    Row {
                                        width: parent.width; spacing: 8
                                        Repeater {
                                            model: [
                                                { label: "WI-FI", value: NativeApi.wifiFrameCount, hint: "Raw frames" },
                                                { label: "WFB", value: NativeApi.wfbFrameCount, hint: "Decrypted" },
                                                { label: "VIDEO", value: NativeApi.rtpPktCount, hint: "RTP packets" }
                                            ]
                                            Rectangle {
                                                width: (healthContent.width - 16) / 3; height: 78; radius: 8
                                                color: "#0B1726"; border.color: modelData.value > 0 ? "#176B72" : "#24364C"
                                                Column {
                                                    anchors.centerIn: parent; spacing: 3
                                                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.label; color: "#7D8EA5"; font.pixelSize: 10; font.weight: Font.Bold }
                                                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.value; color: modelData.value > 0 ? "#67E8F9" : "#CBD5E1"; font.pixelSize: 18; font.weight: Font.Bold }
                                                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.hint; color: "#5F7189"; font.pixelSize: 9 }
                                                }
                                            }
                                        }
                                    }
                                    Text {
                                        width: parent.width
                                        text: "Tip: Wi-Fi = 0 means adapter/channel trouble. Wi-Fi rises but WFB = 0 usually means the key or link does not match. WFB rises but Video = 0 points to the stream or codec setup."
                                        color: "#A6B5C8"; font.pixelSize: 10; lineHeight: 1.2; wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            Rectangle {
                                width: parent.width - 28
                                height: telemetryContent.implicitHeight + 28
                                radius: 10; color: "#111F31"; border.color: "#20344D"
                                Column {
                                    id: telemetryContent
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14
                                    spacing: 8
                                    Text { text: "MAVLINK TELEMETRY"; color: "#58DDEF"; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1 }
                                    Row {
                                        width: parent.width; spacing: 8
                                        Rectangle {
                                            width: (parent.width - 8) / 2; height: 62; radius: 8; color: "#0B1726"
                                            Column { anchors.centerIn: parent; spacing: 3
                                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "AIR → MISSION PLANNER"; color: "#7D8EA5"; font.pixelSize: 9; font.weight: Font.Bold }
                                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: NativeApi.telemetryRxCount; color: "#A7F3D0"; font.pixelSize: 18; font.weight: Font.Bold }
                                            }
                                        }
                                        Rectangle {
                                            width: (parent.width - 8) / 2; height: 62; radius: 8; color: "#0B1726"
                                            Column { anchors.centerIn: parent; spacing: 3
                                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "MISSION PLANNER → AIR"; color: "#7D8EA5"; font.pixelSize: 9; font.weight: Font.Bold }
                                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: NativeApi.telemetryTxCount; color: "#A7F3D0"; font.pixelSize: 18; font.weight: Font.Bold }
                                            }
                                        }
                                    }
                                    Text {
                                        width: parent.width
                                        text: "Mission Planner: choose UDP and port 14550. The air unit must run mavfwd on the UART connected to a MAVLink-enabled flight-controller serial port."
                                        color: "#A6B5C8"; font.pixelSize: 10; wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            Rectangle {
                                width: parent.width - 28; height: 360; radius: 10
                                color: "#0A1422"; border.color: "#20344D"
                                ColumnLayout {
                                    anchors.fill: parent; anchors.margins: 12; spacing: 8
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text { text: "ACTIVITY LOG"; color: "#58DDEF"; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1; Layout.fillWidth: true }
                                        Button {
                                            text: "Copy all"
                                            onClicked: {
                                                outputLog.selectAll()
                                                outputLog.copy()
                                                outputLog.deselect()
                                                window.notify("Log copied to clipboard", 2500)
                                            }
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: "Saved to " + NativeApi.GetLogFilePath()
                                        color: "#63758E"; font.pixelSize: 9; elide: Text.ElideMiddle
                                    }
                                    ScrollView {
                                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                        TextArea {
                                            id: outputLog
                                            readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                                            font.family: "Consolas"; font.pixelSize: 10; color: "#B8C5D8"
                                            background: Rectangle { color: "#07101B"; radius: 6 }
                                            Component.onCompleted: {
                                                append("Log file: " + NativeApi.GetLogFilePath())
                                                NativeApi.onLog.connect(function(level, message) {
                                                    append("[" + level + "] " + message)
                                                    cursorPosition = length
                                                })
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Component.onCompleted: {
        NativeApi.onWifiStop.connect(function() {
            window.receiverRunning = false
            window.videoActive = false
            player.stop()
        })
    }
}
