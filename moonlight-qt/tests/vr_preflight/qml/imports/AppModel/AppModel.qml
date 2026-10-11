import QtQuick 2.9

// QML 測試用的 AppModel 替身（真的是 C++：app/gui/appmodel.h）。只做 AppView.qml 會用到的部分；
// 主機與 app 的狀態由測試直接改下面的屬性。
ListModel {
    // ---- 測試設定的狀態 ----
    property int runningAppId: 0
    property string runningAppName: ""
    property string ownerUuid: ""
    property string ownerName: ""
    property string localUuid: "this-device"
    property string vrReason: ""
    // vrLaunchTarget() 回傳的內容（跟真的一樣三個欄位）
    property var target: ({"address": "host.example:47989", "host": "host.example", "port": 47989})

    // ---- 給測試看的紀錄 ----
    property int targetReads: 0
    property int quitCalls: 0

    signal computerLost()
    signal peerIsVipleChanged()
    signal steamSwitchProgress(string state, string message)

    function initialize(computerManager, computerIndex, showHiddenGames, vrOnly) {
        append({"name": "Beat Saber", "running": false, "boxart": "", "hidden": false, "appid": 11,
                "directLaunch": false, "isAppCollectorGame": false})
        append({"name": "Half-Life: Alyx", "running": false, "boxart": "", "hidden": false, "appid": 12,
                "directLaunch": false, "isAppCollectorGame": false})
    }

    function vrLaunchTarget() {
        targetReads++
        return target
    }

    function vrUnavailableReason() { return vrReason }
    function getRunningAppId() { return runningAppId }
    function getRunningAppName() { return runningAppName }
    function getSessionOwnerUuid() { return ownerUuid }
    function getSessionOwnerName() { return ownerName }
    function getLocalUniqueId() { return localUuid }
    function getDirectLaunchAppIndex() { return -1 }
    function featuredAppIndex() { return count > 0 ? 0 : -1 }
    function nameAt(index) { return (index >= 0 && index < count) ? get(index).name : "" }
    function createSessionForApp(appIndex, takeover) { return null }
    function quitRunningApp() { quitCalls++ }
    function setAppHidden(appIndex, hidden) {}
    function setAppDirectLaunch(appIndex, directLaunch) {}

    function peerSupportsSteamProfiles() { return false }
    function refreshSteamProfiles() { return false }
    function steamProfileCount() { return 0 }
    function steamProfilePersona(index) { return "" }
    function steamProfileAccount(index) { return "" }
    function steamProfileSwitchable(index) { return false }
    function currentSteamProfileIndex() { return -1 }
    function requestSteamSwitch(accountName) { return false }
    function lastSteamSwitchError() { return "" }
}
