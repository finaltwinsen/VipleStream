# Steam Frame client 建置與實機工具（`moonlight-qt/scripts/steamframe/`）

VipleStream 2.0 的 Steam Frame（SteamOS，Linux arm64）client 以 Flatpak 打包。完整操作手冊（builder 前置、
G-BUILD 結果、probe 用法、Day 0／Day 1 清單）在 **`docs/steam_frame_client.md`**；這份只說明目錄內容與最短用法。
設計背景：`docs/vr_architecture.md` §2.1、§2.2、§6、§8.1。

## 目錄

| 路徑 | 用途 |
|---|---|
| `flatpak/io.github.finaltwinsen.VipleStream.yml` | Flatpak manifest（app-id `io.github.finaltwinsen.VipleStream`，runtime `org.kde.Platform//6.11`）。**不要直接餵給 flatpak-builder**：viplestream 模組的來源由建置腳本產生 |
| `flatpak/finish-args.md` | 每一條 finish-arg 的理由、取捨、UNVERIFIED 項目、刻意不帶的清單、libdrm 決定 |
| `flatpak/build-viplestream.sh` | viplestream 模組在沙箱內的建置步驟（picoquic／picotls 離線建置 → qmake → make → 授權） |
| `flatpak/gamescope-wsi/*.json` | gamescope WSI implicit layer（x86_64、aarch64 各一；aarch64 版 UNVERIFIED） |
| `s3/` | S3／PoC-F-pre 前置：gamescope openvr backend 偵測、SteamVR null driver 設定、沙箱內 `xr-probe` 對 SteamVR runtime（見 `s3/README.md`） |
| `../build-steamframe.sh` | 建置入口（`--arch x86_64|aarch64 --flavor dev|release`） |
| `../frame-poc-collect.sh` | Frame 實機證據收集（Day 0／Day 1），只在本機產生 tarball |

## 最短用法（在 linux-builder 上）

```bash
cd ~/VipleStream
bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --check          # 缺什麼、怎麼裝（不會自動裝）
bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64 --flavor dev --smoke
bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64 --flavor dev --smoke-only   # 不重建，只重跑 smoke
nohup setsid bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --flavor dev --bundle --smoke \
    > ~/steamframe-build.log 2>&1 < /dev/null & disown
tail -n 50 ~/steamframe-build.log
```

- 長建置經 SSH **不要包 `systemd-inhibit`**：polkit 會回 `interactive authentication required`，整條命令直接失敗。
  builder 已把 `sleep.target` mask 掉，本來就不會休眠；只有在本機桌面 session 內（或用 sudo 取得 inhibitor）
  才可選擇性地加 `systemd-inhibit --what=sleep:idle`，建置本身一律用自己的帳號跑。
- 同一個 `--work` 同時只能跑一個建置（state、`manifest/`、`repo/` 兩個 arch 共用，腳本用 flock 擋下）；
  兩個 arch 要同時建，就各用自己的 `--work`。
- `--smoke-only`：不準備來源、不建置，直接對 `repo/` 裡現有的建置跑 smoke（例：建置時沒有 GUI session，登入桌面後補跑）。

所有選項、結束碼、輸出位置、原始碼同步（dev patch／release `git bundle`）：`bash moonlight-qt/scripts/build-steamframe.sh --help`。

## 規則

- **一律透過 `build-steamframe.sh` 建置**（CLAUDE.md 建置規範）。它負責前置檢查、來源快照或 pin、計時、bundle、smoke test；手動呼叫 flatpak-builder 會漏掉 `viplestream-source.json`、`flavor.txt`、`SOURCE_INFO`。
- **腳本不安裝任何東西**。缺 flatpak、flatpak-builder、qemu-user-binfmt、KDE runtime 時只印出確切指令並以 2 結束；安裝要使用者同意後手動執行（flatpak 一律 `--user`）。唯一會改動 builder 上 flatpak 狀態的是 `--smoke`／`--smoke-only`（`--user` 安裝到本地 remote `viple-local`）。
- **release 只有 aarch64**。x86_64 Flatpak 開 libdrm，只給開發用（S1、PoC-F-pre）。
- **版號不在這裡改**。`moonlight-qt/app/version.txt` 由 `build-tools\version.ps1` 管；release 要求它等於 `version.json`，所有 Frame 建置都不 bump。
- **pin 只信 commit**。manifest 的每個 git 來源都 pin commit，tag／分支只寫在註解。picotls 的 pin 只寫一次（YAML anchor `&picotls-pin`），`build-viplestream.sh` 會和 picoquic `CMakeLists.txt` 的預設 tag 比對，不一致就失敗。
- **授權與原始碼**：依賴模組的授權由 flatpak-builder 自動收到 `/app/share/licenses/<app-id>/<module>/`；moonlight-qt、picoquic、picotls 由 `build-viplestream.sh` 手動補。release 的 viplestream 來源一律寫公開 URL（`https://github.com/finaltwinsen/VipleStream.git`）＋HEAD commit，`/app/manifest.json` 不帶 builder 的本機路徑。picoquic 的 commit 必須能從 GitHub 上的 ref 到達，否則 rc 3。主 repo HEAD 還沒 push 時照常建置（下載階段以 `url.insteadOf` 從本機 repo 取），但產物命名 `…-linux-arm64-unpublished.flatpak`、**不可上 release**；push 後重跑同一條命令（全部 cache hit）才會產出正式檔名。需要隨 release 保存原始碼時，另跑一次 `flatpak-builder --bundle-sources`（腳本預設不做，避免干擾 G-BUILD 計時）。

## 已知風險（細節見 `flatpak/finish-args.md` §4）

1. **Qt 6.11 是新變數**（Windows 與 builder 都在 6.10.x）。退路：manifest 的 `runtime-version` 與腳本的 `KDE_BRANCH` 一起改成 `6.10`（同為 freedesktop 25.08 基底、仍在維護）。
2. **qemu 建置時間（G-BUILD）**：aarch64 在 x86_64 builder 上靠 qemu-user，乾淨建置可能要好幾個小時（ncnn 的 glslang、FFmpeg）。dev flavor 關掉 app 的 LTO；G-BUILD 以 release 設定判定，增量 > 45 分鐘交使用者做 U7 決策（原生 arm64 builder）。
3. **libplacebo 選 B（v7.360.1）** 能否配 FFmpeg 9.0 編過 UNVERIFIED；不行就退回 A（Flathub 的 commit＋gamescope 補丁）。
4. **沙箱內載入 SteamVR 的 OpenXR runtime** UNVERIFIED（R2）：先做 `s3/` 的 PoC-F-pre。
5. **`~/.steam:ro` 會暴露 `registry.vdf`**：PoC-F 後收窄。
6. **gamescope WSI 的 aarch64 layer** 是否存在 UNVERIFIED：Frame 上沒有就拿掉 `host-os:ro`。
