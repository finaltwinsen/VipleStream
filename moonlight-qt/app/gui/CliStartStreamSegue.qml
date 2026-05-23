import QtQuick 2.0
import QtQuick.Controls 2.2

import ComputerManager 1.0

Item {
    function onSearchingComputer() {
        stageLabel.text = qsTr("Establishing connection to PC...")
    }

    function onSearchingApp() {
        stageLabel.text = qsTr("Loading app list...")
    }

    function onSessionCreated(appName, session) {
        var component = Qt.createComponent("StreamSegue.qml")
        var segue = component.createObject(stackView, {
            "appName": appName,
            "session": session,
            "quitAfter": true
        })
        stackView.push(segue)
    }

    function onLaunchFailed(message) {
        errorDialog.text = message
        errorDialog.open()
        console.error(message)
    }

    function onAppQuitRequired(appName) {
        quitAppDialog.appName = appName
        quitAppDialog.open()
    }

    Component.onCompleted: {
        // VipleStream: when CLI stream drives the launcher from C++ (see
        // main.cpp StreamRequested), the session is already created by the
        // time this view loads.  In that case, skip the launcher.execute()
        // dance and push the StreamSegue directly with the pre-created
        // session.  Avoids relying on StackView.onActivated which did not
        // fire under Wayland headless CLI invocations.
        //
        // Use Qt.callLater so the push happens AFTER the current push of
        // this view completes — calling stackView.push() while still
        // mid-push throws "cannot replace while already in the process of
        // completing a push".
        if (typeof cliStreamSession !== "undefined" && cliStreamSession) {
            toolBar.visible = false
            // VipleStream: defer to event-loop tick.  Calling stackView.push
            // synchronously from Component.onCompleted of a view that is
            // itself mid-push throws "cannot replace while already in the
            // process of completing a push" in Qt 6.10.
            Qt.callLater(function() {
                var comp = Qt.createComponent("StreamSegue.qml")
                var segue = comp.createObject(stackView, {
                    "appName":   cliStreamAppName,
                    "session":   cliStreamSession,
                    "quitAfter": true
                })
                stackView.push(segue)
            })
        }
    }

    StackView.onActivated: {
        // Legacy path — used when the launcher hasn't been driven from C++
        // (no cliStreamSession context property).  Kept for compatibility
        // with any non-CLI entry point that may still load this view.
        if (typeof cliStreamSession !== "undefined" && cliStreamSession) {
            return
        }
        if (!launcher.isExecuted()) {
            toolBar.visible = false

            launcher.searchingComputer.connect(onSearchingComputer)
            launcher.searchingApp.connect(onSearchingApp)
            launcher.sessionCreated.connect(onSessionCreated)
            launcher.failed.connect(onLaunchFailed)
            launcher.appQuitRequired.connect(onAppQuitRequired)
            launcher.execute(ComputerManager)
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 5

        BusyIndicator {
            id: stageSpinner
            running: visible
        }

        Label {
            id: stageLabel
            height: stageSpinner.height
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter

            wrapMode: Text.Wrap
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        onClosed: {
            Qt.quit();
        }
    }

    NavigableMessageDialog {
        id: quitAppDialog
        text:qsTr("Are you sure you want to quit %1? Any unsaved progress will be lost.").arg(appName)
        standardButtons: Dialog.Yes | Dialog.No
        property string appName : ""

        function quitApp() {
            var component = Qt.createComponent("QuitSegue.qml")
            var params = {"appName": appName, "quitRunningAppFn": function() { launcher.quitRunningApp() }}
            stackView.push(component.createObject(stackView, params))
        }

        onAccepted: quitApp()
        onRejected: Qt.quit()
    }
}
