import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

import Updater 1.0

// 三狀態對話框：confirm / downloading / failed。
//
// 不用 standardButtons + DialogButtonBox，因為標準按鈕 emit accepted /
// rejected 後 Dialog 會 auto-close，從 confirm 切到 downloading 會閃。
// 改成自製 footer + closePolicy: NoAutoClose，狀態切換完全由 mode 控制。
//
// readyToRestart 觸發後立即 Qt.quit() — helper 已在等本 PID 退出，
// 多停留會撞 30s Wait-Process timeout。
NavigableDialog {
    id: dialog

    closePolicy: Popup.NoAutoClose
    modal: true

    property string newVersion: ""
    property string releasePageUrl: ""
    property string mode: "confirm"           // "confirm" / "downloading" / "failed"
    property string errorMessage: ""

    function openForVersion(version, pageUrl) {
        dialog.newVersion     = version
        dialog.releasePageUrl = pageUrl
        dialog.mode           = "confirm"
        dialog.errorMessage   = ""
        dialog.open()
    }

    Connections {
        target: Updater
        function onUpdateFailed(msg) {
            dialog.errorMessage = msg
            dialog.mode         = "failed"
            if (!dialog.opened) dialog.open()
        }
        function onReadyToRestart() {
            Qt.quit()
        }
    }

    ColumnLayout {
        spacing: 16
        width: 480

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 16
            text: {
                if (dialog.mode === "confirm") {
                    return qsTr("Download and install VipleStream %1?").arg(dialog.newVersion)
                }
                if (dialog.mode === "downloading") {
                    return Updater.status || qsTr("Working…")
                }
                return qsTr("Update failed")
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dialog.mode === "confirm"
            text: qsTr("VipleStream will close and relaunch automatically after the update is applied. " +
                       "On Windows you may be prompted by UAC if installed to Program Files.")
        }

        // 下載進度
        ColumnLayout {
            visible: dialog.mode === "downloading"
            Layout.fillWidth: true
            spacing: 6

            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to:   Updater.bytesTotal > 0 ? Updater.bytesTotal : 1
                value: Updater.bytesReceived
                indeterminate: Updater.bytesTotal <= 0
            }

            Label {
                Layout.fillWidth: true
                font.pixelSize: 12
                opacity: 0.7
                text: {
                    if (Updater.bytesTotal > 0) {
                        var mbDone  = (Updater.bytesReceived / (1024 * 1024)).toFixed(1)
                        var mbTotal = (Updater.bytesTotal    / (1024 * 1024)).toFixed(1)
                        return qsTr("%1 / %2 MB").arg(mbDone).arg(mbTotal)
                    }
                    return qsTr("Connecting…")
                }
            }
        }

        // 失敗訊息
        Label {
            visible: dialog.mode === "failed"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: "#ff7070"
            text: dialog.errorMessage
        }

        Label {
            visible: dialog.mode === "failed"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 12
            opacity: 0.7
            text: qsTr("Click 'Open release page' to download manually.")
        }
    }

    footer: Item {
        implicitHeight: 56

        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            // confirm mode: Yes / No
            Button {
                id: btnYes
                visible: dialog.mode === "confirm"
                flat: true
                text: qsTr("Yes")
                onClicked: {
                    dialog.mode = "downloading"
                    Updater.startUpdate(dialog.newVersion)
                }
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed:  clicked()
                Keys.onLeftPressed:   btnNo.forceActiveFocus(Qt.TabFocus)
            }
            Button {
                id: btnNo
                visible: dialog.mode === "confirm"
                flat: true
                text: qsTr("No")
                onClicked: dialog.close()
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed:  clicked()
                Keys.onRightPressed:  btnYes.forceActiveFocus(Qt.TabFocus)
            }

            // downloading mode: Cancel
            Button {
                id: btnCancel
                visible: dialog.mode === "downloading"
                flat: true
                text: qsTr("Cancel")
                onClicked: Updater.cancel()
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed:  clicked()
            }

            // failed mode: Open release page / Close
            Button {
                id: btnOpen
                visible: dialog.mode === "failed"
                flat: true
                text: qsTr("Open release page")
                onClicked: {
                    Qt.openUrlExternally(dialog.releasePageUrl)
                    dialog.close()
                }
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed:  clicked()
                Keys.onLeftPressed:   btnClose.forceActiveFocus(Qt.TabFocus)
            }
            Button {
                id: btnClose
                visible: dialog.mode === "failed"
                flat: true
                text: qsTr("Close")
                onClicked: dialog.close()
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed:  clicked()
                Keys.onRightPressed:  btnOpen.forceActiveFocus(Qt.TabFocus)
            }
        }
    }

    // Mode 切換時把焦點丟到該模式的第一顆 button，gamepad/keyboard 才能直接按 Enter
    onModeChanged: {
        Qt.callLater(function() {
            if (mode === "confirm")        btnYes.forceActiveFocus(Qt.TabFocus)
            else if (mode === "downloading") btnCancel.forceActiveFocus(Qt.TabFocus)
            else if (mode === "failed")    btnOpen.forceActiveFocus(Qt.TabFocus)
        })
    }
    onOpened: {
        if (mode === "confirm") btnYes.forceActiveFocus(Qt.TabFocus)
    }
}
