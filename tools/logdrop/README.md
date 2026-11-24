# VipleStream logdrop

測試者在 app 或 server 網頁按「上傳給開發者」時，報告會送到這支 Google Apps Script，存進部署者自己的
Google 雲端硬碟（資料夾「VipleStream logs」）。上傳成功後畫面會顯示一組 6 碼代碼，測試者把代碼傳給開發者即可。

- **同意制**：Android 第一次開 app、server 第一次進網頁時詢問一次「是否自動傳送匿名 log」，之後在設定頁可改。
  同意後在特定時機自動上傳，不用每次按按鈕：
  - Android（目前只有眼鏡）：XR 眼鏡診斷完成、眼鏡串流結束。每個時機至少隔 10 分鐘、每天最多 5 份。
  - server：串流結束、上次異常結束後重新啟動。每個時機至少隔 6 小時、每天最多 3 份。
- **匿名安裝 ID**：每個安裝隨機產生 8 碼，跟裝置與帳號無關，顯示在設定頁。測試者回報問題時報這個 ID，
  開發者用 `op=list&id=<ID>` 就能找到他的所有報告。手動上傳另外會顯示單份報告的 6 碼代碼。
- 送出前兩端都會先遮掉 IP、MAC、電子郵件、電腦名稱、使用者名稱。
- 協定寫在 `Code.gs` 檔頭。

## 部署（只做一次）

1. 到 <https://script.google.com> → 「新專案」，把 `Code.gs` 全部貼進去取代預設內容，專案名稱取 `VipleStream logdrop`。
2. 函式選單選 `setup` → 「執行」。第一次會要求授權（存取你的雲端硬碟），按允許。
   執行紀錄會印出 `FOLDER_ID` 與 `READ_KEY`。
3. 「部署」→「新增部署作業」→ 類型選「網頁應用程式」：
   - 執行身分：**我**
   - 誰可以存取：**任何人**
4. 部署後複製「網頁應用程式」網址（`https://script.google.com/macros/s/…/exec`）。
5. 把網址填進：
   - Android：`moonlight-android/app/src/main/java/com/limelight/utils/LogDrop.java` 的 `ENDPOINT`（已填）
   - server：`Sunshine/src/logdrop.h` 的 `ENDPOINT`（已填）
6. `READ_KEY` 只給要讀報告的人，不要進 repo。

改了 `Code.gs` 之後要「管理部署作業」→ 編輯 → 版本選「新版本」，網址才會沿用。
網址被濫用時，封存舊部署、建一個新部署，再換掉兩端的 `ENDPOINT`。

## 讀取

```bash
URL='https://script.google.com/macros/s/…/exec'
curl -sL "$URL?op=ping"
curl -sL "$URL?op=list&key=$READ_KEY"
curl -sL "$URL?op=get&key=$READ_KEY&code=K7Q2MX" -o report.txt
```

## 限制

- 單次請求 20 MB、解壓後 40 MB；每分鐘 20 份、每天 300 份（全部上傳者合計）。
- POST 的回應會先 302 轉到 `script.googleusercontent.com`，client 要跟隨轉址並改用 GET 取回 JSON
  （OkHttp 與 libcurl 預設就是這樣）。
