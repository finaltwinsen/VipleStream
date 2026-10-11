pragma Singleton
import QtQuick 2.9

// QML 測試用的替身：測試裡沒有手把，按鍵由 TestCase 的 keyClick 送
QtObject {
    signal scrollRequested(real pages)

    function getConnectedGamepads() { return 0 }
    function setUiNavMode(uiNavMode) {}
    function enable() {}
    function disable() {}
    function notifyWindowFocus(hasFocus) {}
}
