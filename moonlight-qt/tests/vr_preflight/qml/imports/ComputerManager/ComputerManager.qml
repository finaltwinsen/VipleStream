pragma Singleton
import QtQuick 2.9

// QML 測試用的 ComputerManager 替身：只有 AppView.qml／QuitSegue.qml 用到的部分
QtObject {
    // QuitSegue 等這個訊號；error 是 undefined＝結束成功
    signal quitAppCompleted(var error)

    function setVrAppLists(vrOnly) {}
    function startPolling() {}
    function stopPollingAsync() {}
}
