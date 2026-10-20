import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0

// §VR-SHELL（2026-10-20）：頭盔 VR 介面的外殼。不是視窗：backend/xrshell.cpp 用 QQuickRenderControl 把它畫在離屏的
// QQuickWindow 裡，讀回成圖片貼到 XR 面板上，控制器射線轉成滑鼠事件送回來。內容與平面視窗共用 AppContent.qml。
//
// 根物件叫 window，提供 AppContent 與各頁面會用到的 window.theme、pollingActive、clearOnBack；平面視窗才有意義的
// show()／hide() 等做成空函式（VR 裡介面一直都在面板上，不必藏）。
Item {
    id: window

    property bool pollingActive: false
    property bool clearOnBack: false
    // 介面一直顯示在 XR 面板上
    property bool active: true
    property Theme theme: Theme {}

    // 面板的邏輯尺寸（xrshell.cpp 依它建離屏畫面）
    width: 1280
    height: 800

    Material.theme: Material.Dark
    Material.background: theme.ink
    Material.foreground: theme.paper
    Material.primary: theme.ink2
    Material.accent: theme.lime

    function showMainWindow() {}
    function show() {}
    function hide() {}
    function showMinimized() {}
    function showMaximized() {}
    function showFullScreen() {}
    function raise() {}
    function requestActivate() {}

    Rectangle {
        anchors.fill: parent
        color: window.theme.ink
    }

    AppContent {
        id: appContent
        anchors.fill: parent
        focus: true
    }

    Component.onCompleted: {
        // 平面視窗看「可見＋有焦點」決定要不要輪詢主機；VR 介面一直都在
        ComputerManager.startPolling()
        pollingActive = true
    }
}
