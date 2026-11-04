import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtWebEngine

import VrLauncher 1.0

// VipleStream 2.0 §VR-PLAYER：影片模式的內建瀏覽器（Qt WebEngine＝Chromium 核心）。在網頁上找到影片之後按
// 「Play in VR」：先暫停網頁裡的影片，再交給 VR 播放器（VrLauncher.launchPlayer）——
//   - 網頁裡的 <video> 直接用 http(s) 網址播的（不是 blob:／MediaSource）：用那個網址直接播；
//   - 其他（YouTube 等用 MediaSource 的網站）：用這一頁的網址，播放器經 yt-dlp 解析（180°／360°／EAC 的判斷照舊）。
// 只在有 Qt WebEngine 的建置（Flatpak 用 io.qt.qtwebengine.BaseApp）由 VideoView 的 Loader 載入；沒有的建置不會碰到這個檔。
// 2026-11-02 起它就是影片模式的 YouTube 分頁（使用者要「跟網頁版一模一樣、可以登入」）：首頁 youtube.com、網址列仍可去別的網站。
Item {
    id: browser

    // VideoView 給：目前選的畫面格式（playerformat 名稱）
    property string formatName: "auto"
    property string homeUrl: VrLauncher.devVideoUrl !== "" ? VrLauncher.devVideoUrl : "https://www.youtube.com/"
    property bool pageHasVideo: false
    signal launchFailed(string message)

    function go(text) {
        var t = text.trim()
        if (t === "") {
            return
        }
        if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//.test(t)) {
            // 沒有 scheme：像網址的就補 https://，否則當成搜尋
            t = (t.indexOf(".") > 0 && t.indexOf(" ") < 0) ? "https://" + t
                                                         : "https://www.youtube.com/results?search_query=" + encodeURIComponent(t)
        }
        view.url = t
    }

    // 網頁裡正在播（或最大的）那一個 <video>：回傳 {src, title}
    readonly property string probeScript: "(function(){var vs=document.querySelectorAll('video');var best=null,area=0;" +
        "for(var i=0;i<vs.length;i++){var v=vs[i];var a=v.clientWidth*v.clientHeight;if(!v.paused)a+=1e9;if(a>area){area=a;best=v;}}" +
        "for(var j=0;j<vs.length;j++){try{vs[j].pause();}catch(e){}}" +
        "return {src: best?(best.currentSrc||best.src||''):'', title: document.title};})()"

    // 手把捲網頁（VideoView 轉過來；pages＝幾個可見高度）
    function scrollPages(pages) {
        view.runJavaScript("window.scrollBy(0," + Math.round(pages * view.height) + ")")
    }

    // B／返回鍵（VideoView 轉過來）：網頁的上一頁；沒有上一頁回 false
    function goBackIfPossible() {
        if (!view.canGoBack) {
            console.warn("[VIPLE-PLAYER] browser back: no history")
            return false
        }
        view.goBack()
        return true
    }

    // 手把從分頁列按「下」：焦點進網頁
    function focusPage() {
        view.forceActiveFocus()
    }

    // Google 登入會擋「內嵌瀏覽器」（「這個瀏覽器或應用程式可能不安全」），它看 User-Agent 裡的 QtWebEngine/x.y 字樣；
    // 拿掉那一段就是一般的 Chrome User-Agent（核心本來就是 Chromium），YouTube 也會給完整的桌面版網頁
    Component.onCompleted: {
        var ua = webProfile.httpUserAgent
        webProfile.httpUserAgent = ua.replace(/ ?QtWebEngine\/[^ ]+/, "")
        console.warn("[VIPLE-PLAYER] browser user agent: " + webProfile.httpUserAgent)
        view.url = browser.homeUrl
    }

    function playInVr() {
        view.runJavaScript(probeScript, function(r) {
            var src = r && r.src ? r.src : ""
            console.warn("[VIPLE-PLAYER] browser play in VR: page=" + view.url + " videoSrc=" + src.substring(0, 80))
            // §REVIEW-1104（使用者決策 C）：Google 影片伺服器的直連多半是低畫質、很快過期的檔案，改交網頁網址給 yt-dlp
            var direct = /^https?:\/\//.test(src) && !/^https?:\/\/[^\/]*googlevideo\.com\//.test(src)
            var source = direct ? src : view.url.toString()
            if (!VrLauncher.launchPlayer(source, browser.formatName, r && r.title ? r.title : view.title, "")) {
                browser.launchFailed(qsTr("Could not start the video player."))
            }
        })
    }

    Timer {
        // dev（--dev-video play=<秒>）：自動按「Play in VR」
        running: VrLauncher.devVideoPlaySec > 0
        interval: VrLauncher.devVideoPlaySec * 1000
        onTriggered: browser.playInVr()
    }

    // YouTube 的 VR 影片（180°／360°）自動轉到 VR 播放（2026-11-02 使用者要求：不必再按「Play in VR」）。
    // 看 YouTube 播放器自己的 player response：任一格式的 projectionType 不是 RECTANGULAR＝VR 影片；
    // 拿不到時退而看播放器有沒有 360° 的視角控制。videoId 要對得上（站內換頁時 player response 可能還是上一支）。
    // 同一支只自動轉一次：從 VR 播放器退回來（B）還停在這一頁時不會又被帶回去；要再看就按「Play in VR」
    property string autoLaunchedId: ""
    readonly property string vrProbeScript: "(function(){var r={n:document.querySelectorAll('video').length,id:'',vr:''};" +
        "try{var u=new URL(location.href);if(/(^|\\.)youtube\\.com$/.test(u.hostname)&&u.pathname==='/watch'){r.id=u.searchParams.get('v')||'';" +
        "var p=document.getElementById('movie_player');var pr=p&&p.getPlayerResponse?p.getPlayerResponse():null;" +
        "if(pr&&pr.videoDetails&&pr.videoDetails.videoId===r.id){var fs=(pr.streamingData&&pr.streamingData.adaptiveFormats)||[];" +
        "for(var i=0;i<fs.length;i++){var t=fs[i].projectionType;if(t&&t!=='RECTANGULAR'){r.vr=String(t);break;}}}" +
        "if(!r.vr&&p&&p.querySelector&&p.querySelector('.ytp-webgl-spherical-control')){r.vr='SPHERICAL';}}}catch(e){}" +
        "return r;})()"

    Timer {
        // 有沒有影片（讓按鈕顯眼一點）、是不是 YouTube 的 VR 影片（自動轉 VR 播放）：每秒看一次
        interval: 1000
        repeat: true
        running: browser.visible && !VrLauncher.running
        onTriggered: view.runJavaScript(browser.vrProbeScript, function(r) {
            browser.pageHasVideo = r && r.n > 0
            if (r && r.vr !== "" && r.id !== "" && r.id !== browser.autoLaunchedId && !view.loading && !VrLauncher.running) {
                browser.autoLaunchedId = r.id
                console.warn("[VIPLE-PLAYER] browser: VR video detected (" + r.vr + ", " + r.id + "), playing in VR")
                browser.playInVr()
            }
        })
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Button { text: qsTr("Back"); enabled: view.canGoBack; onClicked: view.goBack() }
            Button { text: qsTr("Forward"); enabled: view.canGoForward; onClicked: view.goForward() }
            Button { text: view.loading ? qsTr("Stop") : qsTr("Reload"); onClicked: view.loading ? view.stop() : view.reload() }
            Button { text: qsTr("Home"); onClicked: view.url = browser.homeUrl }
            TextField {
                id: address
                Layout.fillWidth: true
                selectByMouse: true
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                placeholderText: qsTr("Search or type a web address")
                onAccepted: browser.go(text)
            }
            Button {
                id: playButton
                text: qsTr("Play in VR")
                highlighted: browser.pageHasVideo
                enabled: /^https?:\/\//.test(view.url.toString())
                onClicked: browser.playInVr()
            }
        }
        ProgressBar {
            Layout.fillWidth: true
            visible: view.loading
            from: 0
            to: 100
            value: view.loadProgress
        }

        WebEngineView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            // url 在 Component.onCompleted 才設（先改好 User-Agent，第一個請求就用改過的）
            // 帳號登入（YouTube 等）與 cookie 留著：用有名字的設定檔（存在 app 的資料夾），不用無痕
            profile: WebEngineProfile {
                id: webProfile
                storageName: "VipleStream"
                offTheRecord: false
                httpCacheType: WebEngineProfile.DiskHttpCache
                persistentCookiesPolicy: WebEngineProfile.AllowPersistentCookies
            }
            onUrlChanged: {
                // 離開自動轉過的那支片（上一頁、點別支）：之後再點回來還是要自動轉
                if (browser.autoLaunchedId !== "" && url.toString().indexOf("v=" + browser.autoLaunchedId) < 0) {
                    browser.autoLaunchedId = ""
                }
                if (!address.activeFocus) {
                    address.text = url.toString()
                }
            }
            // 新視窗（target=_blank）在同一頁打開
            onNewWindowRequested: function(request) {
                view.url = request.requestedUrl
            }
            onLoadingChanged: function(info) {
                console.warn("[VIPLE-PLAYER] browser load status=" + info.status + " " + info.url
                             + (info.errorString ? " error=" + info.errorString : ""))
            }
            onRenderProcessTerminated: function(status, code) {
                console.warn("[VIPLE-PLAYER] browser render process terminated: status=" + status + " code=" + code)
            }
            onFullScreenRequested: function(request) {
                request.accept()
            }
        }
    }
}
