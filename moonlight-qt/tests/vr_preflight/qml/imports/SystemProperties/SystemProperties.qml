pragma Singleton
import QtQuick 2.9

// QML 測試用的替身：ErrorMessageDialog.qml（QuitSegue 的錯誤對話框）看 hasBrowser
QtObject {
    property bool hasBrowser: false
    property bool usesMaterial3Theme: true
}
