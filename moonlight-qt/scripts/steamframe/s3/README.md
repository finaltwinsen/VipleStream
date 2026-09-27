# S3／PoC-F-pre：linux-builder 上的 SteamVR null driver 前置（SOP）

`docs/vr_architecture.md` §8.1 的 **S3**：linux-builder 上以 SteamVR Linux null driver（隔離）加
`gamescope --backend openvr`，作為 α overlay、**PoC-F-pre**、PoC-2b 的前置。§2.1 規定「沒有腳本路徑的模擬環境，
不能當成關卡證據」，所以這裡的每一步都有腳本或確切命令。

PoC-F-pre 要回答的問題（R2 的前半）：**KDE runtime 的 Flatpak 沙箱內，能不能載入 SteamVR 的 OpenXR runtime？**
Frame 上的 PoC-F 用同一個 `xr-probe` 回答後半。

## 0. 範圍與同意事項

| 步驟 | 需要 Steam 登入？ | 會改系統？ | 誰做 |
|---|---|---|---|
| 1 檢查 gamescope openvr backend | 否 | 否（唯讀） | 腳本 |
| 2 建立隔離的 Steam | **是** | 是（安裝 Steam；專用帳號或 Flatpak 版 Steam） | **使用者**（腳本不做） |
| 3 裝 SteamVR | **是** | 是 | **使用者**（Steam 介面） |
| 4 寫 null driver 設定 | 否 | 只改隔離 Steam 的 `config/steamvr.vrsettings`（有備份） | 腳本（預設 dry-run，`--apply` 才寫） |
| 5 啟動 SteamVR、設成 OpenXR runtime | 已登入 | SteamVR 會寫 `~/.config/openxr/1/active_runtime.json`（隔離帳號內） | 使用者 |
| 6 裝 x86_64 dev Flatpak | 否 | `flatpak --user` 安裝 | 腳本（`build-steamframe.sh --smoke`）或使用者 |
| 7 沙箱內 xr-probe | 否 | 否 | 腳本 |
| 8（選用）gamescope openvr overlay | 已登入 | 否 | 使用者 |

**這裡沒有任何腳本會登入 Steam、啟動 Steam、下載 SteamVR 或安裝套件。** 需要帳號的步驟一律交使用者決定與執行
（SteamVR 需要 Steam 帳號授權，steamcmd 匿名登入一般拿不到，UNVERIFIED）。

## 1. gamescope 有沒有 openvr backend

```bash
bash moonlight-qt/scripts/steamframe/s3/check-gamescope-openvr.sh
```
- `verdict: openvr-backend-present`（rc 0）→ 第 8 步可做。
- `absent`／`unknown`（rc 3）或 `not-installed`（rc 4）→ 選項交使用者：自建 gamescope 到隔離前綴、容器內的
  gamescope，或 S3 只做第 7 步（PoC-F-pre 本身不需要 gamescope）。

## 2. 隔離的 Steam（使用者執行）

§6 規定 SteamVR 要隔離，不裝進 `/usr/local`、不污染 x64 AppImage 的 ldd 收集。兩種做法擇一：

- **A. S3 專用 Linux 帳號＋原生 Steam（建議，佈局最接近 Frame）**
  ```bash
  sudo adduser --disabled-password vrtest        # 帳號名自訂
  # 以 vrtest 登入桌面 session（SteamVR 需要圖形 session），安裝／啟動 Steam、登入
  touch ~/.viple-s3-isolated                     # 以 vrtest 身分：宣告這個帳號是 S3 專用（第 4 步的防護會檢查）
  ```
  Steam 根目錄：`/home/vrtest/.local/share/Steam`。之後的第 4–7 步都以這個帳號執行。
- **B. 專為 S3 新裝的 Flatpak 版 Steam（`com.valvesoftware.Steam`，`--user`）**
  **只適用於平常沒在用 Flatpak 版 Steam 的帳號**：必須是為 S3 新裝的那一份。日常就在用 Flathub 版 Steam 的話，
  改用做法 A——第 4 步會把 `forcedDriver` 改成 null，真實頭盔就被 null driver 取代。
  ```bash
  flatpak install --user flathub com.valvesoftware.Steam   # 新裝；啟動、登入
  touch ~/.var/app/com.valvesoftware.Steam/.viple-s3-isolated   # 宣告這份 Flatpak Steam 是 S3 專用（第 4 步的防護會檢查）
  ```
  Steam 根目錄：`~/.var/app/com.valvesoftware.Steam/.local/share/Steam`。
  代表性差異：SteamVR 在另一個沙箱內，佈局與 Frame 原生不同；而且 VipleStream 的沙箱要看到
  `~/.var/app/com.valvesoftware.Steam/…` 底下的路徑，flatpak 是否允許以 `--filesystem` 暴露別的 app 的私有目錄
  **UNVERIFIED**；vrserver 與 VipleStream 分屬兩個沙箱，IPC 能否互通也 **UNVERIFIED**。B 失敗不代表 Frame 會失敗。

## 3. 裝 SteamVR（使用者執行）

在隔離的 Steam 介面安裝 SteamVR（Linux 版）。

## 4. 寫 null driver 設定

```bash
# 先 dry-run 看會寫什麼
bash moonlight-qt/scripts/steamframe/s3/write-null-driver-vrsettings.sh --steam-root ~/.local/share/Steam
# 確認後寫入（SteamVR 必須是關著的）
bash moonlight-qt/scripts/steamframe/s3/write-null-driver-vrsettings.sh --steam-root ~/.local/share/Steam --apply
```
- 只接受隔離的 Steam，而且都要操作者刻意建立的標記檔：`~/.var/app/com.valvesoftware.Steam/.viple-s3-isolated`
  存在的 Flatpak 版 Steam（做法 B），或 `~/.viple-s3-isolated` 存在的專用帳號（做法 A）。其他一律拒絕（rc 2）；
  只看路徑分不出「為 S3 新裝的」和「日常用的」Flatpak Steam，所以 Flatpak 版也要標記。
- 預設每眼 1728×1728、90 Hz（對應 VR emulate 樣本 3456×1728@90）；`--render`、`--hz`、`--window` 可改。
- 已存在的設定檔會先備份成 `steamvr.vrsettings.bak-<時間>`，只合併 `steamvr`／`driver_null` 的相關鍵；結束時印出還原指令。
- 鍵值是社群慣用的 null driver 設定，SteamVR 版本不同是否都認 **UNVERIFIED**；不生效時要改 SteamVR 安裝目錄的
  `drivers/null/resources/settings/default.vrsettings`（`"enable": true`），這一步請手動做並記錄。

## 5. 啟動 SteamVR（使用者執行）

在隔離帳號的桌面 session 啟動 SteamVR；null driver 會開一個桌面視窗當「頭盔畫面」。在 SteamVR 設定
「OpenXR」頁把 SteamVR 設為目前的 OpenXR runtime（會寫 `~/.config/openxr/1/active_runtime.json`）。
確認 `pgrep -a vrserver` 有東西。

## 6. 安裝 x86_64 dev Flatpak

以隔離帳號（或要跑 xr-probe 的那個帳號）：
```bash
bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64 --flavor dev --smoke
# 或把別處建好的 bundle 裝進來
flatpak install --user --bundle VipleStream-Client-<版號>-linux-x64-dev.flatpak
```
x86_64 dev 建置帶 `CONFIG+=openxr`，所以 `xr-probe` 有實作（AppImage 與 Windows 建置會回 10）。

## 7. 沙箱內 xr-probe（PoC-F-pre 的答案）

```bash
bash moonlight-qt/scripts/steamframe/s3/run-xr-probe-steamvr.sh --steam-root ~/.local/share/Steam
```
跑三件事，輸出在 `~/viple-s3-xrprobe-<時間>/`：

| 案例 | 內容 | 判讀 |
|---|---|---|
| `xr-probe-auto` | 只靠 manifest 的 finish-args 與 app 的 runtime 探索 | rc 0＝原生佈局下 finish-args 足夠 |
| `xr-probe-explicit` | 臨時 `--filesystem=<SteamVR>:ro`＋`--xr-runtime-json` | **rc 0＝runtime 能在 KDE runtime 沙箱內載入**；rc 13＝載入失敗，JSON 內有 `dlopen` 的 `dlerror()` |
| `sandbox-ldd` | 沙箱內 `ldd <runtime .so>` | 列出 KDE runtime 缺的函式庫（對照 `host-side.txt` 的 host ldd） |

腳本結束碼＝`xr-probe-explicit` 的結束碼。結果連同 `summary.txt` 回報，並記進 `docs/steam_frame_client.md`。

## 8.（選用）gamescope openvr overlay（α 形態）

第 1 步 verdict 是 present 時，在 SteamVR 已啟動的 session：
```bash
gamescope --backend openvr -W 1280 -H 720 -- flatpak run --arch=x86_64 io.github.finaltwinsen.VipleStream//dev
```
預期：SteamVR dashboard 出現一個 overlay，裡面是 VipleStream 的主機列表。gamescope 的 openvr 相關參數依版本不同
（**UNVERIFIED**），以 `gamescope --help` 為準。

## 代表性限制（回報時一併寫明）

- builder 上是 x86_64 的 SteamVR；Frame 是 arm64 的 SteamVR（Steam Runtime 與函式庫版本都可能不同）。
- null driver 不是真實 HMD：沒有 tracking、refresh rate 是設定值、沒有 Frame 的 compositor 行為。
- 做法 B（Flatpak 版 Steam）的佈局與 IPC 都和 Frame 原生不同。
- PoC-F-pre 通過只代表「機制可行」；Frame 上仍要用 `frame-poc-collect.sh --with-app-probes` 跑一次 xr-probe（PoC-F）。

## 還原

- 設定：第 4 步印出的還原指令（`cp -p <備份> <目標>` 或刪除新建的檔案）。
- app：`flatpak --user uninstall io.github.finaltwinsen.VipleStream//dev`。
- 專用帳號：使用者自行 `sudo deluser --remove-home vrtest`。
- 做法 B 的 Flatpak Steam：`rm ~/.var/app/com.valvesoftware.Steam/.viple-s3-isolated`，或整份移除
  `flatpak --user uninstall --delete-data com.valvesoftware.Steam`。
