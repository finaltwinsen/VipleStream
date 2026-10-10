# Steam Frame client 指南（Linux arm64 Flatpak）

> 狀態：**M2a（α 建置）**，2026-09-28。設計背景見 [`vr_architecture.md`](vr_architecture.md) §2.1、§2.2、§6、§7、§8.1。
> 建置與實機工具都在 `moonlight-qt/scripts/`（已進 git）：`build-steamframe.sh`、`frame-poc-collect.sh`、
> `steamframe/`（manifest、[`finish-args.md`](../moonlight-qt/scripts/steamframe/flatpak/finish-args.md)、
> [`s3/`](../moonlight-qt/scripts/steamframe/s3/README.md)）。
> 主機代號：`<builder>` 是 Linux x64 建置機（Ubuntu 26.04），`<dev-client>` 是 Windows 開發機，`<host>` 是測試
> server，`<frame>` 是 Steam Frame。文中不寫任何實際 IP、帳號或主機名稱。

## 0. 一頁摘要

1. Frame client 是 moonlight-qt 的**原生 Linux arm64 版**，以 **Flatpak** 打包，app-id
   **`io.github.finaltwinsen.VipleStream`**（U6，2026-09-27 定案）。
2. 在 `<builder>` 上用 **`bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --flavor dev|release`** 建置；
   aarch64 靠 qemu-user。腳本不安裝任何東西，缺什麼就印出確切指令並以 2 結束。
3. 三個探測動作 `xr-probe`、`v4l2-probe`、`decode-bench`，加上 `[VIPLE-SF-ENV]` 環境摘要與
   `stream --dump-bitstream`，是 Frame 到貨後 Day 1 的主要工具。所有平台都編得進去，用不到的建置回 10。
4. Frame 上的證據一律用 `frame-poc-collect.sh` 在本機打包，再由 `<dev-client>` 用 scp 拉回。
5. S3／PoC-F-pre（需要 Steam 登入）**延到 M3a 開頭**（使用者 2026-09-28 決定）。

---

## 1. 概觀

### 1.1 為什麼是原生 Linux arm64 Flatpak

- **原生 arm64**：Frame 跑的是 Linux arm64 的 SteamOS。moonlight-qt 在 M2a 修掉 aarch64 的建置阻斷（F8：
  nvvideoparser SIMD、ncnn、picoquic／picotls 路徑，見 `vr_architecture.md` §2.1）之後可以直接編成 arm64，
  桌面模式 α／β 與 PCVR 用同一個 client，不需要轉譯層。
- **Flatpak（P1）**：Qt 6 由 KDE runtime 提供，不必自建 Qt；沙箱與 `--user` 安裝不需要改動系統目錄。
  Flathub 上游 Moonlight 在 aarch64 用的是同一套配方與旗標（v4l2m2m＋libdrm），本專案的 manifest 以它為範本。
- **退路**：G-PKG（沙箱內拿不到 `/dev/video*`，或載不起 OpenXR runtime）失敗時，β／rc 改走 P2（sniper arm64 zip），
  再不行才是 P3。見 `vr_architecture.md` §2.2。

### 1.2 app-id 與路徑

app-id **`io.github.finaltwinsen.VipleStream`** 在第一個 Frame 建置前定案，之後不可再改：改了以後
`~/.var/app/<app-id>/` 底下的設定、配對資料與 log 都會斷掉。

| 用途 | 路徑（沙箱外看到的位置） |
|---|---|
| 設定（QSettings） | `~/.var/app/<app-id>/config/VipleStream/VipleStream.conf` |
| log | `~/.var/app/<app-id>/cache/VipleStream/VipleStream/logs/`（`VipleStream-<epoch>.log` 留最新 10 份；`probe-*.log` 留最新 50 份，§6.1） |
| 探測 JSON 預設位置 | 同 log 目錄，`probe-<action>-<epochMs>-<pid>.json`（不會自動清） |
| 樣本（沙箱看得到） | `~/.var/app/<app-id>/data/samples/`（`frame-poc-collect.sh --samples` 會複製進來） |
| 安裝內容（沙箱內） | `/app/`；建置來源與旗標記在 `/app/share/VipleStream/SOURCE_INFO`；授權在 `/app/share/licenses/<app-id>/` |

設定與 log 路徑是依 `Path::initialize` 與 QSettings 慣例推得（Flatpak 把 `XDG_CONFIG_HOME`／`XDG_CACHE_HOME`／
`XDG_DATA_HOME` 改到 `~/.var/app/<app-id>/` 底下），實際位置在 PoC-1 確認。

Flatpak branch：dev 建置是 `dev`，release 是 `stable`。執行 dev 版要指定 branch，例如
`flatpak run io.github.finaltwinsen.VipleStream//dev --help`。

### 1.3 版號

Frame 建置**一律不 bump**。版號來自 `moonlight-qt/app/version.txt`（由 `build-tools\version.ps1` 管理）；
release flavor 要求它等於 `version.json`，dev 不一致時只警告。

---

## 2. builder 前置（一次性，需使用者同意）

`<builder>` 是 x86_64 的 Ubuntu 26.04。以下指令要使用者同意後手動執行（`build-steamframe.sh` 不會代為安裝）。
flatpak 一律裝在 `--user`，不需要 sudo。

```bash
# 1. 工具（apt 候選版：flatpak 1.16.6、flatpak-builder 1.4.8、qemu-user 10.2.1）
sudo apt install flatpak flatpak-builder qemu-user qemu-user-binfmt

# 2. Flathub remote（--user）
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

# 3. KDE 6.11 SDK 與 Platform，x86_64 與 aarch64 各一份
flatpak --user install -y flathub org.kde.Sdk//6.11 org.kde.Platform//6.11
flatpak --user install -y --arch=aarch64 flathub org.kde.Sdk//6.11 org.kde.Platform//6.11

# 4. 確認 qemu-aarch64 的 binfmt：要 enabled，flags 那一行要含 F
cat /proc/sys/fs/binfmt_misc/qemu-aarch64

# 5. 讓腳本自己檢查一次（缺什麼會列出確切指令，exit 2）
cd ~/VipleStream
bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --check
bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64 --check
```

- **F（fix-binary）flag 為什麼重要**：flatpak-builder 在 bwrap 沙箱內執行 aarch64 程式，沙箱裡看不到 host 的
  `/usr/bin/qemu-aarch64`。F flag 讓 kernel 在註冊 binfmt 時就把 qemu 開好，沙箱內才跑得動。
  Ubuntu 26.04 沒有 `qemu-user-static` 實體套件，`qemu-user-binfmt` 負責註冊。
- **空間**：兩種 arch 的 SDK、git 鏡像、ccache、ostree repo 合計數十 GB；工作目錄剩不到 30 GB 時腳本會警告。
- **`--check` 的最後一步**會實際跑 `flatpak run --arch=<arch> --command=true org.kde.Sdk//6.11`，一次驗證 bwrap
  （AppArmor 的 userns 限制）、binfmt 與 SDK。這一步失敗時手動重跑同一行看錯誤訊息。

---

## 3. `build-steamframe.sh`

一律透過這支腳本建 Flatpak（CLAUDE.md 建置規範）。不要直接對 manifest 跑 `flatpak-builder`：viplestream 模組的來源
（`viplestream-source.json`）、`flavor.txt`、`SOURCE_INFO` 都由腳本產生。完整說明：
`bash moonlight-qt/scripts/build-steamframe.sh --help`。

### 3.1 用法與選項

```bash
bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64|aarch64 --flavor dev|release [選項]
```

repo 內的 `.sh` 是 100644，一律用 `bash …` 呼叫。

| 選項 | 說明 |
|---|---|
| `--arch x86_64\|aarch64` | 必要。x86_64 主機上建 aarch64 靠 qemu-user |
| `--flavor dev\|release` | 必要（`--check` 時可省）。差異見 §3.2 |
| `--work DIR` | 工作目錄，預設 `$HOME/viple-steamframe`。不可在 repo 內、不可是 `$HOME` 本身。state（下載與快取）、`manifest/`、`repo/` 兩種 arch 共用，所以**同一個 `--work` 同時只能跑一個建置**（腳本對整個工作目錄拿 `flock`，拿不到就以 rc 1 結束）；兩種 arch 要同時建，就各用自己的 `--work`（state 不共用，下載各做一次） |
| `--jobs N` | 平行度，傳給 `flatpak-builder --jobs` |
| `--clean` | 建置前清掉 state 的 `cache/ ccache/ build/ checksums/` 與 `build-<arch>/`，保留 `git/ downloads/`（量乾淨建置時不把下載時間算進去）。快取兩種 arch 共用，會一起清 |
| `--download-only` | 只下載來源，不建置、不計時。runtime 還沒裝好也能先跑 |
| `--bundle` | 產出 `.flatpak`（release 一律產） |
| `--smoke` | 建置完成後安裝到 `--user` 的本地 remote，跑 smoke test（§3.7） |
| `--smoke-only` | 不準備來源、不下載、不建置，直接對 `--work` 的 `repo/` 裡現有的 `<app-id>//<branch>` 跑 `--smoke` 那幾輪（§3.7）；`--arch`／`--flavor`（與 `--work`）要和當初建置時相同。`--version` 以安裝進來的 `SOURCE_INFO` 版號核對（`version.txt` 之後改過也不影響）；release 另外拿 `out/` 裡同版號最新的 bundle 驗，沒有就略過那一輪。前置檢查只要 flatpak、Platform runtime 與 binfmt（不需要 flatpak-builder、SDK）。不能和 `--clean`、`--download-only`、`--bundle` 一起用（rc 1）。用途：GUI 那一輪記成 `SKIPPED` 之後，有人登入桌面時補跑，不必為了 smoke test 重建（viplestream 模組每次都會整個重建） |
| `--check` | 只做前置檢查，印出缺什麼與確切的安裝指令，不建置 |

只有 `--smoke` 與 `--smoke-only` 會改動 builder 上的 flatpak 狀態（`--user` 安裝，不需要 sudo）。

### 3.2 dev 與 release

| | dev | release |
|---|---|---|
| viplestream 來源 | 工作樹快照：`git ls-files -co --exclude-standard` 列出的 `moonlight-qt/`、根目錄 `LICENSE`、picoquic submodule，每次重建（含未 commit 的改動與新檔；builder 上舊的建置產物不會帶進沙箱） | 主 repo 公開 URL 的 HEAD commit（`disable-submodules`）＋ picoquic 的 GitHub git 來源，pin 在 gitlink SHA。為什麼不用 `file://` 與還沒 push 時怎麼辦，見表下「release 產物不帶本機路徑」 |
| 前提 | 無（`version.txt` ≠ `version.json` 只警告） | `moonlight-qt/` 與 picoquic 沒有未 commit 的改動或未追蹤檔；`version.txt` = `version.json`；picoquic submodule 的 HEAD 等於 gitlink，而且該 commit 能從 GitHub 上的 ref 到達（§3.5） |
| flatpak branch | `dev` | `stable` |
| app 的 LTO | 關（`CONFIG+=disable-lto`，`-O3` 保留） | 開（`-O3 -flto`） |
| 可用 arch | x86_64、aarch64 | **只有 aarch64**；`--arch x86_64 --flavor release` 以 rc 1 拒絕（x86_64 Flatpak 開 libdrm，只給開發用，見 finish-args.md §3） |
| bundle | 加 `--bundle` 才產 | 一律產 |
| 用途 | 日常開發、S1、PoC-F-pre、G-BUILD 參考數字 | 2.0.0 起 release 的第六件、G-BUILD 判定 |

- release 的乾淨檢查只看 `moonlight-qt/` 與 picoquic：builder 上 `Sunshine/third-party/inputtino`、`wlr-protocols`
  永遠是未追蹤（`.deb` 建置需要），不影響。
- **對外發佈前主 repo 與 picoquic fork 都要 push**，manifest 裡的 commit 才有公開的原始碼對應（GPL 原始碼提供義務）。
- **release 產物不帶本機路徑**：flatpak-builder 會把展開後的 manifest 寫進 `/app/manifest.json`，這一步在 build-commands
  與 cleanup 之後，刪不掉，而且會跟著 `.flatpak` 發佈出去。所以 `build-steamframe.sh` 的 release 來源**一律寫公開 URL**
  `https://github.com/finaltwinsen/VipleStream.git` ＋ HEAD commit（`disable-submodules`），不寫
  `file://<builder 上的 repo 路徑>`（含 builder 的帳號名稱，外部也取不到）；`/app/manifest.json` 本身就是正確的原始碼參照。
  HEAD 在不在 GitHub 上，腳本直接問 GitHub 判斷（和 picoquic 同一套檢查，§3.5），不看本機可能過期的 `origin/*`。
  連不上 GitHub 時主 repo 一律當成還沒 push（產物走下一點的 `-unpublished`，屬於安全的一邊）；picoquic 的檢查則直接以 rc 3 結束。
- **主 repo 還沒 push 時照常建置**（G-BUILD 增量量法就是「本地 commit、不 push」，§4.1）：manifest 不變，只在下載階段以
  `GIT_CONFIG_COUNT` 形式的 `url.<本機 repo>.insteadOf` 把公開 URL 導到本機 repo（只影響那一次 flatpak-builder 的 git 子行程，
  需要 git ≥ 2.31）；下載前先用 `git ls-remote <公開 URL>` 自檢，看不到本機 HEAD 就以 rc 3 結束。產物改名
  `VipleStream-Client-X.Y.Z-linux-arm64-unpublished.flatpak`，**不可上 release**。push 之後重跑同一條命令（flatpak-builder 的
  git 鏡像與模組 checksum 都依 manifest 的 URL＋commit 計算，所以不再下載、所有模組 cache hit），就會產出正式檔名。
- 發佈前自己再驗一次（直接讀安裝目錄，不必經 qemu 執行；沒有輸出才對）：

  ```bash
  flatpak --user install -y --bundle ~/viple-steamframe/out/VipleStream-Client-<版號>-linux-arm64.flatpak   # 預設 --work
  loc=$(flatpak --user info --arch=aarch64 --show-location io.github.finaltwinsen.VipleStream//stable)
  grep -nE 'file://|/home/' "$loc/files/manifest.json"
  ```
- 需要隨 release 保存原始碼時，另跑一次 `flatpak-builder --bundle-sources`（腳本預設不做，以免干擾計時）。

### 3.3 結束碼

| rc | 意義 |
|---|---|
| 0 | 成功 |
| 1 | 參數錯誤（含拒絕 x86_64 release、`--work` 位置不合法、`--work` 正被另一個建置使用） |
| 2 | 前置條件不足（已印出安裝指令；含缺 `flock`、`--smoke-only` 時 `repo/` 還不存在） |
| 3 | 來源或版號檢查失敗（含 picoquic commit 無法從 GitHub 上的 ref 到達或連不上 GitHub、HEAD 未 push 時 `url.insteadOf` 自檢失敗）；manifest 準備或解析失敗（`flatpak-builder --show-manifest`） |
| 4 | flatpak-builder 失敗（下載或建置） |
| 5 | bundle 失敗 |
| 6 | smoke test 失敗 |

### 3.4 輸出（都在 `--work` 底下）

```
manifest/                                     這次實際用的 manifest、viplestream-source.json、flavor.txt、
                                              SOURCE_INFO、dev 快照（src/）
logs/steamframe-<arch>-<flavor>-<時間>.log      本腳本的記錄
logs/fb-<arch>-<flavor>-<download|build>-<時間>.log
                                              flatpak-builder 輸出（每行前面加 epoch 秒）
logs/resolved-manifest-<arch>-<flavor>-<時間>.json
logs/gbuild-<arch>-<flavor>-<時間>.tsv          各模組耗時表
logs/gbuild-history.tsv                       每次建置一行（G-BUILD 紀錄）
logs/smoke-<arch>-<flavor>-<時間>-<case>.txt    smoke test 各次輸出
repo/                                         ostree repo（branch dev／stable）
out/VipleStream-Client-<版號>-linux-arm64.flatpak            release（HEAD 已在 GitHub 上）
out/VipleStream-Client-<版號>-linux-arm64-unpublished.flatpak
                                              release 但 HEAD 還沒 push：不可上 release（§3.2）
out/VipleStream-Client-<版號>-linux-<arm64|x64>-dev.flatpak   dev（--bundle）
                                              每個 .flatpak 旁附 .sha256
```

`gbuild-history.tsv` 的欄位：`time arch flavor clean jobs rc total_s viplestream_s head`。

### 3.5 原始碼同步（`<dev-client>` → `<builder>`）

**dev**（未 commit 的改動，含新檔）：

```bash
# <dev-client>
git add -N <新檔…>                 # 讓新檔出現在 diff 裡（intent-to-add，不 stage 內容）
git diff --binary HEAD > m2a.patch
scp m2a.patch <user>@<builder>:~/

# <builder>
cd ~/VipleStream
git apply -R ~/last.patch          # 上一份 patch 還套著時先退掉
git apply ~/m2a.patch && cp ~/m2a.patch ~/last.patch
```

也可以把變更的檔案打成 tar 解到 builder 的 repo。**不要在 builder 上 `git clean -fdx` 或 `git stash -u`**：
會連 `.deb` 建置需要的未追蹤目錄（`Sunshine/third-party/inputtino`、`wlr-protocols`）一起刪掉。

**release**（commit SHA 必須和 `<dev-client>` 完全一致，工作樹乾淨）：

```bash
# 已 push
git pull --ff-only

# 未 push：用 git bundle 帶 commit 過去
# <dev-client>
git bundle create viple.bundle origin/main..main     # builder 沒有 origin/main 那些 commit 時改用：git bundle create viple.bundle main
scp viple.bundle <user>@<builder>:~/
# <builder>
git bundle verify ~/viple.bundle
git fetch ~/viple.bundle main:refs/remotes/bundle/main
git merge --ff-only bundle/main
```

picoquic submodule 的 commit 必須已經在 GitHub（`finaltwinsen/picoquic`）上，而且要能從某個 ref 到達。release 腳本會檢查：
先 `git ls-remote`；不是任何 ref 的 head 時，再抓 GitHub 上所有 ref 的 commit 物件確認它在其中。只能依 SHA 取得的 commit
（例如已被 force-push 蓋掉的舊 commit、只存在於 fork network 裡別的 fork）不算：flatpak-builder 對只 pin commit 的 git 來源
只會抓 ref，不會依 SHA 抓，放行的話要到下載階段才以 rc 4 失敗。檢查不過時以 rc 3 結束，先 push picoquic fork 的
`viplestream-main`。

### 3.6 長建置

qemu 下的 aarch64 建置動輒數小時，一律脫離 SSH session 再輪詢：

```bash
nohup setsid bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --flavor dev --bundle --smoke \
    > ~/steamframe-build.log 2>&1 < /dev/null & disown
tail -n 50 ~/steamframe-build.log
```

**經 SSH 不要包 `systemd-inhibit`**：它要向 logind 取得 sleep inhibitor。經 SSH 呼叫時不是本機的 active session，polkit
會要求互動驗證，回 `interactive authentication required`，整條命令（連同建置）直接失敗。builder 已經關掉 AC 電源下的休眠
（`sleep.target` masked），本來就不會休眠，經 SSH 直接用 `nohup setsid` 即可。只有在 builder 本機桌面 session 的終端機裡
（或用 sudo 取得 inhibitor）才可以選擇性地加 `systemd-inhibit --what=sleep:idle`；建置本身一律用自己的帳號跑，不要以 root
執行。換到沒關休眠的機器時，請使用者先關掉休眠，不要在 SSH 下硬包。
`build-steamframe.sh --help`、`steamframe/README.md`、`building.md` §7、`vr_architecture.md` 都是這個寫法。

同一個 `--work` 同時只能跑一個 `build-steamframe.sh`（§3.1，`--smoke-only` 也算）。背景建置還沒結束時，要跑別的 arch 或
flavor 就改用另一個 `--work`，否則第二次執行會在開頭以 rc 1 結束。

### 3.7 smoke test（`--smoke`、`--smoke-only`）

1. 把 `repo/` 加成 `--user` 的本地 remote `viple-local`（`--no-gpg-verify`），`--reinstall` 安裝 `<app-id>//<branch>`。
2. 兩種環境各跑 `--help` ×2、`--version` ×1（`--version` 的輸出必須含版號）：
   - **GUI 環境**：找得到 `$XDG_RUNTIME_DIR/wayland-0`（或 `WAYLAND_DISPLAY` 指定的 socket）或有 `DISPLAY` 時才跑；
     都沒有就記 `SKIPPED`、不算失敗。有人登入桌面後用 `--smoke-only`（同樣的 `--arch`／`--flavor`）補跑，不必重建。
   - **純 SSH 環境**：清掉 `WAYLAND_DISPLAY`／`DISPLAY`，加 `--nosocket=wayland --nosocket=fallback-x11 --nosocket=x11`。
     一定要跑，驗證沒有視窗環境時 `--help` 改走 offscreen QPA、不會落到 EGLFS 去搶 DRM（M2a R2）。
3. release 另外用 `flatpak --user install --bundle` 裝產出的 `.flatpak` 再跑一輪。

逾時：原生 180 秒，qemu 900 秒（qemu 下 Qt 初始化很慢）。SSH 下腳本會自動補 `XDG_RUNTIME_DIR` 與
`DBUS_SESSION_BUS_ADDRESS`。移除：`flatpak --user uninstall io.github.finaltwinsen.VipleStream//dev`。

### 3.8 manifest 與模組

manifest：`moonlight-qt/scripts/steamframe/flatpak/io.github.finaltwinsen.VipleStream.yml`，範本是 Flathub 的
`com.moonlight_stream.Moonlight`。runtime `org.kde.Platform//6.11`（SDK `org.kde.Sdk//6.11`）。

| 順序 | 模組 | pin | 備註 |
|---|---|---|---|
| 1 | ncnn | `ncnn-20250503-full-source.zip`（sha256 pin） | `strip-components: 0`、`NCNN_VERSION=20250503`；補裝 `ncnn/stb_image_write.h` |
| 2 | SDL3 | `release-3.4.14`（`147a8ee3`） | 不開 IPO（qemu 省時） |
| 3 | sdl2-compat | `release-2.32.70`（`a53b6ad9`） | app 只用 SDL2 API，和 Windows 同一套 |
| 4 | SDL2_ttf | SDL2 分支（`a883e490`） | runtime 只有 sdl3-ttf |
| 5 | dav1d | `1.5.4`（`54706fc6`） | |
| 6 | openxr-loader | OpenXR-SDK `release-1.1.63`（`f2448a87`） | 產 `openxr.pc`，只有 `CONFIG+=openxr` 的建置會用 |
| 7 | ffmpeg | cgutman `moonlight_9.0_r1`（`d17de7e3`） | Flathub 旗標去掉 `--enable-lto`，**另開 h264／hevc／av1 parser**（decode-bench 用 `av_parser_parse2`；`--disable-all` 會把 parser 全關）。v4l2m2m 靠 autodetect＋`--fatal-warnings` 保證編進去；v4l2-request 一併開 |
| 8 | libplacebo | `v7.360.1`（`cee9b076`） | 選 B：API 360，和 Windows prebuilt 相同，不需要 Flathub 的 gamescope 補丁。和 FFmpeg 9 編不過就退回 A（`4d82c689`＋補丁） |
| 9 | gamescope-wsi-x86_64／-aarch64 | 本地 JSON（`only-arches`） | implicit layer，`library_path` 指向 host 的 `.so`；aarch64 版 UNVERIFIED |
| 10 | viplestream | `viplestream-source.json`（腳本產生）＋ picotls（`bfa67875`，`dest: picotls-src`） | `buildsystem: simple`，執行 `steamframe/flatpak/build-viplestream.sh` |

- **順序**：最常改的放後面。flatpak-builder 的快取逐模組串接，前面任何一個模組變動，後面全部重建（qemu 下光 ncnn
  就要好幾個小時）。
- **不建 libdecor**（freedesktop 25.08 runtime 內建）；**libdrm 兩種 arch 都開**；`appstream-compose: false`（非 Flathub
  發佈，上架時再開）。
- **所有 git 來源都 pin commit**，tag／分支只寫在註解。picotls 的 pin 只寫一次（YAML anchor），`build-viplestream.sh`
  會和 picoquic `CMakeLists.txt` 非 AEGIS 分支的預設 tag 比對，不一致就失敗（`FETCHCONTENT_SOURCE_DIR` 會忽略
  `GIT_TAG`，不比對會靜默漂移）。
- **`build-viplestream.sh`**（沙箱內）：離線建 picoquic＋picotls 靜態庫（picotls-fusion 只在 x86_64 有）→ qmake shadow build
  （`PREFIX=/app NCNN_PREFIX=/app PICOQUIC_BUILD=… PICOQUIC_DIR=… VIPLE_DESKTOP_ID=$FLATPAK_ID CONFIG+=openxr
  DEFINES+=VIPLE_MPQUIC`，dev 另帶 `CONFIG+=disable-lto`；明確傳入 SDK 的 `CFLAGS`／`CXXFLAGS`／`LDFLAGS`）→
  `make release` → `make install` → 補 moonlight-qt／picoquic／picotls 授權、寫 `SOURCE_INFO` → 自檢（`ldd`
  有沒有 `not found`、有沒有連到 `libopenxr_loader` 與 `libncnn`，只警告，最終以 smoke test 為準）。
  qmake 變數見 [`building.md`](building.md) §7。
- 依賴模組的授權由 flatpak-builder 自動收到 `/app/share/licenses/<app-id>/<module>/`。

---

## 4. G-BUILD：qemu 建置時間

關卡定義見 `vr_architecture.md` §7（G-BUILD 原列 M0，實際在 M2a 開頭量）。

### 4.1 量法

1. **先下載、不計時**：
   `bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --flavor release --download-only`。主 repo 來源帶
   `disable-shallow-clone`，HEAD 已 push 時第一次會從 GitHub 完整抓整個 repo（約 200 MB）；還沒 push 時從本機 repo 抓（§3.2）。
2. **計時的建置一律 `--disable-download --disable-updates`**（腳本自動帶）。flatpak-builder 每行輸出加 epoch 秒，
   結束時印各模組耗時表，並寫進 `logs/gbuild-*.tsv` 與 `logs/gbuild-history.tsv`。bundle 時間另外記。
3. **乾淨建置**：加 `--clean`（清 `cache/ ccache/ build/ checksums/` 與 `build-<arch>/`，保留 `git/ downloads/`）。
   接 AC 電源、不要同時跑串流測試。qemu 下要好幾個小時，照 §3.6 用 `nohup setsid` 脫離 SSH（不要包 `systemd-inhibit`）；
   量測期間不要在同一個 `--work` 再跑別的建置（會被 `flock` 擋下）。
4. **增量建置**：改一個葉節點 `.cpp` 的註解後重建，跑兩次。release 要求工作樹乾淨，改動要先本地 commit（不必 push；
   這時產物是 `-unpublished.flatpak`，只拿來計時，§3.2）。

**「增量」的定義**：viplestream 模組的來源是 `dir`（dev）或隨 commit 變動的 git 來源（release），flatpak-builder
對它沒有模組內的增量，只要內容變了就**整個模組重建**。所以增量＝**所有依賴模組 Cache hit**，viplestream 模組在
ccache 已暖的情況下完整重建一次。release 的 LTO 連結不吃 ccache，這正是增量時間的主要來源。

**門檻**：增量 ≤ 45 分鐘，以 **release flavor（開 LTO）** 判定。dev flavor 關掉 app 的 LTO，數字只當參考。
超過 45 分鐘就停下，交使用者做 U7（原生 ARM builder）決策。

### 4.2 結果

2026-09-28 在 `<builder>`（Ryzen 7 PRO 2700U 4C/8T、29 GiB、Wi-Fi）量測，flatpak-builder 1.4.8、qemu-user 10.2.1，
`--jobs` 自動（8）。時間是 `build-steamframe.sh` 印出的「建置階段總耗時」，不含不計時的下載階段；bundle 另列。

| 日期 | arch | flavor | 種類 | jobs | 建置總耗時 | viplestream 模組 | bundle | 備註 |
|---|---|---|---|---|---|---|---|---|
| 09-28 | aarch64 | dev | 乾淨 | 8 | 14463 s（4 h 01 m） | 3681 s（ccache 冷） | 16 s | 依賴：ncnn 5114、FFmpeg 2644、SDL3 1681、openxr 392、libplacebo 304、dav1d 213、sdl2-compat 210、SDL2_ttf 206 s；期間有一次低優先權（`nice -n 19`）的 x64 AppImage 建置重疊 |
| 09-28 | aarch64 | dev | 增量 | 8 | **453 s（7.5 m）** | 440 s | 17 s | 依賴全部 Cache hit；改動＝審查修正（main.cpp、v4l2caps、decodebench、sfenv、commandlineparser 等） |
| 09-28 | aarch64 | release | 增量 | 8 | **201 s（3.4 m）** | 186 s | 16 s | commit 729d4050、HEAD 未 push → `-unpublished` 產物；程式碼與上一列相同，ccache 幾乎全中；實際旗標 `-O2`（LTO 未生效，見下）；`--bundle` 安裝後 smoke 通過；`/app/manifest.json` 是 GitHub URL＋commit，沒有本機路徑 |
| 09-28 | x86_64 | dev | 乾淨（參考） | 8 | 1318 s | 271 s | — | 原生速度；ncnn 561、FFmpeg 171、SDL3 148 s |
| 09-28 | x86_64 | dev | 增量（參考） | 8 | 68 s | 53 s | — | 同上的審查修正 |

**判定：G-BUILD 通過**（增量 7.5 分鐘，遠低於 45 分鐘門檻），U7（原生 ARM builder）不必提前採購。

- **LTO 其實沒有生效**：Flatpak（KDE SDK 的 qmake）與 Ubuntu 的 qmake 都開 `force_debug_info`，release 建置用的是
  `QMAKE_CXXFLAGS_RELEASE_WITH_DEBUGINFO`，app 實際編譯旗標是 `-O2 -g`，`app.pro` 的 `-O3 -flto` 從來沒套上（x64
  AppImage 也一樣，是既有問題）。所以 release flavor 與 dev flavor 的增量時間預期相同，`CONFIG+=disable-lto` 在 Linux
  目前等於沒作用。要不要讓 Linux 真的吃到 `-O3 -flto` 會改變已出貨 AppImage 的程式碼產生，列在 `docs/TODO.md` 另案處理。

---

## 5. finish-args 摘要

完整理由、取捨與 UNVERIFIED 項目在 [`finish-args.md`](../moonlight-qt/scripts/steamframe/flatpak/finish-args.md)（和
manifest 必須同步修改）。

| 類別 | finish-args |
|---|---|
| 同 Flathub | `--share=network`、`--share=ipc`、`--socket=wayland`、`--socket=fallback-x11`、`--socket=pulseaudio`、`--device=all`（V4L2 `/dev/video*`、DRM、`/dev/hidraw*`）、`--talk-name=org.freedesktop.ScreenSaver`、`--filesystem=xdg-run/gamescope-0`、`--filesystem=host-os:ro`（gamescope WSI layer）、清掉 `LIBVA_DRIVER_NAME`／`LIBVA_DRIVERS_PATH` |
| 新增（OpenXR／SteamVR） | `--filesystem=xdg-config/openxr:ro`、`--filesystem=xdg-config/openvr:ro`、`--filesystem=xdg-data/Steam/steamapps/common/SteamVR:ro`、`--filesystem=~/.steam:ro`（PoC-F 後收窄） |
| 刻意不帶 | `IGNORE_RFI_LATENCY_BUG`（C30，我們的 fork 不讀）、`QT_QUICK_CONTROLS_STYLE`（程式內已設）、`--filesystem=home`、`host-etc`（Monado S1 測試時才臨時加）、`--socket=session-bus` 等 |

測試時才需要的權限一律在命令列臨時加（`flatpak run --filesystem=<dir>:ro …`），不進 manifest。

---

## 6. 探測動作

三個動作在**所有平台都編得進去**；這一版用不到的功能回 10（Windows 的 `v4l2-probe`、沒有 `CONFIG+=openxr` 的
`xr-probe`、沒有 FFmpeg 的 `decode-bench`）。

```bash
# Flatpak（Frame、或 builder 上 x86_64／aarch64 的 dev 版）
flatpak run io.github.finaltwinsen.VipleStream xr-probe
flatpak run io.github.finaltwinsen.VipleStream v4l2-probe --header-test all --expbuf
flatpak run --filesystem=$HOME/samples:ro io.github.finaltwinsen.VipleStream decode-bench $HOME/samples/hevc_1080p60.hevc
flatpak run --arch=aarch64 io.github.finaltwinsen.VipleStream//dev decode-bench <樣本> --decoder sw --frames 60

# x64 AppImage
./VipleStream-Client-<ver>-linux-x64.AppImage v4l2-probe

# Windows（<dev-client>）
temp\moonlight\VipleStream.exe decode-bench <樣本>.hevc --decoder hwaccel:d3d11va
```

### 6.1 共通規則

- **早期派發**：`main.cpp` 在 log 就緒後、`QGuiApplication` 之前，只建 `QCoreApplication` 就派發探測，跑完
  `std::_Exit`。不碰 QPA、字型、QML、配對金鑰與 HID 探測，所以 SSH、qemu、gamescope 握有 DRM 時都能跑。
  **不取單一實例鎖**：Frame 上 GUI 開著時也能跑。Windows 會掛上父 console，輸出直接看得到。
- **輸出三層**：
  1. stdout：給人看的摘要，一行一個事實。第一行是 `<action>: VipleStream <版號> <平台>`（例如 `linux-arm64 flatpak`）。
  2. log：同一行加上 tag 寫進 log（檔名見下方「probe 的 log 檔」；tag 與行格式見 [`log_tags.md`](log_tags.md) §5c）。
  3. JSON：完整結果一律寫檔。`--json <path>` 指定，預設 `<log 目錄>/probe-<action>-<epochMs>-<pid>.json`；
     stderr 最後一行印 `json: <path>`。
- **JSON 根鍵**（三個探測一致）：`probe`（動作名）、`schema`（目前 1）、`rc`、`env`（`SfEnv::snapshot()` 的完整環境，
  含 Vulkan 裝置與 V4L2 QUERYCAP），其餘是各探測自己的欄位。
- **結束碼**（`cli/probeutil.h`）：

  | rc | 意義 |
  |---|---|
  | 0 | 完成，而且主要能力存在 |
  | 1 | 命令列錯誤（未知選項、缺參數、值不合法；`--help`／`--version` 本身回 0） |
  | 10 | 本建置沒編進這個功能，或平台不支援 |
  | 11 | 輸入檔或參數語意錯誤（由探測自己判斷） |
  | 12 | 需要後續里程碑（`xr-probe --session` 要 M3a） |
  | 13 | 完成，但主要能力不存在（沒有 m2m decoder、沒有可用的 XR runtime、指定的 decoder 開不起來） |
  | 14 | 執行中錯誤 |
  | 15 | JSON 寫檔失敗。**只會把 rc 0 改成 15**，其他結束碼本身就是結論，保留原值 |

- **probe 的 log 檔**：寫 log 檔的建置（Windows、Linux release、Flatpak）在探測模式改寫
  `probe-<action>-<epochMs>-<pid>.log`，同一秒連跑也不會互相截斷。探測模式**只把 `probe-*.log` 修到最新 50 份**，
  **絕不動 `VipleStream-*.log`**（Day 1 會連跑幾十次探測，不能把真正的串流 log 擠掉）。probe 的 JSON 不會自動清。
- **沙箱路徑**：Flatpak 沙箱內只看得到 `~/.var/app/<app-id>/` 與 finish-args 明列的路徑。讀樣本時用
  `flatpak run --filesystem=<目錄>:ro …` 臨時開放，或先複製到 `~/.var/app/<app-id>/data/samples/`；`--json` 指到
  沙箱外的路徑時要開可寫權限（去掉 `:ro`）。開檔失敗時錯誤訊息會附上這段提示。

### 6.2 `[VIPLE-SF-ENV]`：執行環境摘要

目的：Frame 上的每份 log 與探測 JSON 都能自己說明是在什麼環境跑的（打包形態、runtime、arch、host OS、
Wayland／gamescope、V4L2 與 DRM 裝置、OpenXR active runtime）。一律編譯，非 Linux 是 no-op。

- **啟動時兩行**（Linux；SDL video driver 決定之後，`list` 動作不印；探測動作在派發時印）：只收便宜的資訊（讀
  `/.flatpak-info`、os-release、sysfs、環境變數白名單、DRM driver 名稱、OpenXR runtime JSON 路徑），不 dlopen Vulkan、
  不對 `/dev/video*` 做 ioctl，不拖慢一般啟動。
- **session 開場一行** `[VIPLE-SF-ENV] session: …`（Linux，接在 `[VIPLE-SESSION]` 之後），另帶實際採用的 SDL video
  driver 與 `SDL_APP_ID`。
- **JSON 的 `env`**：完整版，另外 dlopen Vulkan 列出裝置、對每個 `/dev/video*` 做 QUERYCAP。只有探測動作會付這個成本。
  - **Vulkan 裝置會合併重複**：Flatpak 內同一張 GPU 可能被列兩次（builder 上實測 RADV 出現兩筆一模一樣的裝置，推測是
    沙箱內兩份 ICD manifest 指到同一個 driver）。合併鍵是 deviceUUID＋driverUUID（裝置 ≥ Vulkan 1.1、UUID 不是全 0；
    否則退回 vendorID／deviceID／driverName／name），所以同一張卡上的不同 driver（例如 RADV 與 AMDVLK）仍分開列。
    `env.vulkan.enumerated` 是 loader 原始筆數、`duplicatesMerged` 是合併掉幾筆，每筆裝置的 `instances` 是它被列了幾次、
    `dedupBy` 是用哪一種鍵；被合併的那幾筆若有欄位不同（例如 Mesa 版本），整筆放在 `differingInstances`。
    UUID 本身不寫進 JSON（部分 driver 的 deviceUUID 是每張卡唯一的硬體識別碼）。
- **隱私**：環境變數只記白名單；`STEAM*` 只記名稱不記值（可能含 token）；`/.flatpak-info` 的 `[Instance]` 只取白名單欄位。
- Flatpak 內 host 的 os-release 從 `/run/host/os-release` 讀，runtime 的從 `/etc/os-release` 讀，兩份都記。
  `QT_QPA_PLATFORM`、`SDL_VIDEODRIVER` 記原始值與目前值（`main.cpp` 可能為了 SSH／eglfs 陷阱改寫過）。

行格式見 [`log_tags.md`](log_tags.md) §5c。

### 6.3 `xr-probe`：OpenXR runtime（PoC-2、PoC-F）

```
viplestream xr-probe [--session] [--xr-runtime-json <path>] [--loader-debug] [--json <path>]
```

| 選項 | 說明 |
|---|---|
| `--session` | B 段（session、refresh rate、FOV、reference space、imageRect 異色測試）。需要 M3a 的 XrContext，這一版回 **12** |
| `--xr-runtime-json <path>` | dev：在 `xrCreateInstance` 前，於**行程內**設 `XR_RUNTIME_JSON` |
| `--loader-debug` | dev：行程內設 `XR_LOADER_DEBUG=all`，loader 的輸出在 stderr |
| `--json <path>` | JSON 報告位置 |

- **A 段**（instance／system 層，不建 session）：loader 與 API layer、擴充清單（列舉後才啟用；含 `VALVE` 的全部列出）、
  API 1.1 → 1.0 退回、`convert_timespec_time` 來回差、`xrGetSystem` 與 system 屬性（含眼動）、PRIMARY_STEREO 的
  view 與 blend mode、`vulkan_enable2`（沒有才用 `vulkan_enable`）的需求與 runtime 指定的 GPU（不建 device），最後一定
  `xrDestroyInstance`。
- 只有帶 `CONFIG+=openxr` 的建置（Flatpak，兩種 arch）有 A 段；AppImage、Windows 回 **10**，但仍會列出 runtime JSON
  的探索結果。
- **Flatpak 裡的 runtime JSON**：沙箱把 `XDG_CONFIG_HOME` 改成 `~/.var/app/<app-id>/config`，OpenXR loader 看不到 host
  的 `~/.config/openxr/1/active_runtime.json`（`xdg-config/openxr:ro` 是把它掛在沙箱內的**原路徑**）。
  `XrRuntimeJson::resolveActive()` 照 loader 的順序自己找，另外補查 host 路徑（`$HOST_XDG_CONFIG_HOME`、`$HOME/.config`、
  `/run/host/etc/xdg`、`/run/host/etc`、`/app/etc`）。只在 loader 自己找不到、而 host 路徑找得到時，於行程內設
  `XR_RUNTIME_JSON` 再建 instance，原因記進 JSON 的 `envApplied`。使用者自己設了 `XR_RUNTIME_JSON` 就尊重它。
- **結束碼**：0 runtime 載入成功（沒接 HMD、`FORM_FACTOR_UNAVAILABLE` 也算 0）；13 找不到或載不起 runtime——這時
  另外 dlopen runtime 的 `.so`，把 `dlerror()` 與 ELF 架構記進 JSON，這本身就是 PoC-F 的答案；14 instance 建好之後
  runtime 回 `INSTANCE_LOST`／`RUNTIME_FAILURE`；10 本建置沒有 OpenXR；12 `--session`（10 優先）。

### 6.4 `v4l2-probe`：V4L2 stateful 解碼器（PoC-0、PoC-3）

```
viplestream v4l2-probe [--device <node>] [--header-test h264|hevc|all] [--expbuf] [--json <path>]
```

| 選項 | 說明 |
|---|---|
| `--device <node>` | 只探測這個節點（預設所有 `/dev/video*`） |
| `--header-test h264\|hevc\|all` | 送 720p 測試幀：等 `SOURCE_CHANGE`、讀 CAPTURE 格式與可見區域、等第一張解出的幀。**會短暫佔用硬體解碼器** |
| `--expbuf` | header-test 時另測 `VIDIOC_EXPBUF`。沒帶 `--header-test` 時是命令列錯誤（rc 1） |
| `--json <path>` | JSON 報告位置 |

- 每個節點：QUERYCAP；m2m 的 OUTPUT／CAPTURE 格式、frame size；decoder 的每個 coded 視訊格式先 `S_FMT` 再逐一列 CAPTURE
  格式與控制項（profile、level 上限、tier、`MIN_BUFFERS_FOR_CAPTURE` 靜態值）；decoder 另外做 `TRY_DECODER_CMD`、`G_PARM`，
  並以 `REQBUFS(count=0)` 查 queue 能力。非 m2m 節點（攝影機）只做 QUERYCAP，不碰格式。
- **`REQBUFS(0)` 只對 decoder 做**，舊式旗標（`legacyCaps`）判成 m2m 的 decoder 也跳過（JSON 的 `queueCaps` 記
  `skipped (legacy caps)`，log 行印 `queueCaps output=skipped(legacy-caps)`）：它會動到 queue 狀態。例如
  `exclusive_caps=0` 的 v4l2loopback 會被舊式旗標判成 m2m，0.14 之前的版本（0.12.x、0.13.x）收到 REQBUFS 會重設所有
  opener 共用的 buffer 旗標，正在讀這台虛擬攝影機的程式之後每次 DQBUF 都會失敗，直到它重開。
- **VR level 判定**：3456×1728 的需求是 HEVC 90 Hz 要 5.2、72 Hz 要 5.1；H.264 90 Hz 要 6.0、72 Hz 要 5.2。log 行的
  `vr90=`／`vr72=` 就是這個比對。
- **header-test 的流程**：兩個 OUTPUT buffer 都先寫好同一份測試 AU，但 STREAMON 前**只把第一份入列** → 等
  `SOURCE_CHANGE` 1 秒；等不到就補送第二份再等 1 秒（JSON `sourceChangeNeededNextAu=true`，log 行
  `sourceChange=<ms>(nextAu)`；兩次都沒等到時印 `sourceChange=none(<總等待 ms>ms)`）→ 設定 CAPTURE → 分三段等第一幀，
  每段 1 秒（第二份已經送過或送不出去時略過第二段）。第二份補送的時機與結果記在 JSON 的 `secondAu`（`stage`、`queued`、
  `reason`；driver 只配到一個 OUTPUT buffer 時不補送）。每個 header-test 最壞約 **4 秒**。
- **header-test 的讀法**：出幀時機記在 JSON 的 `firstFrame.outputAfter`，log 行印成 `out=<值>`。app 的 v4l2m2m 路徑一個
  buffer 只放一個 AU、從不 drain，所以**只有 `immediate` 代表沒有額外的幀延遲**：

  | `outputAfter` | 出幀時機 | 對串流的意義 |
  |---|---|---|
  | `immediate` | 只有第一份 AU、不 drain 就吐幀 | 沒有額外的幀延遲 |
  | `nextAu` | 補送第二份 AU 之後才吐幀；為了 `SOURCE_CHANGE` 已經補送過時，第一段出幀也記成這個 | 要看到下一個 AU 才輸出，串流時每幀至少多等一個幀間隔（90 Hz 約 11 ms），直接吃掉 G-rc「解碼 p99 < 11 ms」的預算。第二份 AU 和第一份是同一張 IDR（H.264）或 CRA（HEVC），不是 P 幀，被第二張 IRAP 擠出 DPB 的情況也算在這裡，所以這是**樂觀的下限** |
  | `drain` | 送 `DEC_CMD_STOP` 之後才吐幀 | 串流路徑從不 drain，照現況不能用 |
  | `none` | 三段都沒有幀 | 原因看 log 行的 `firstFrame=none(…)` 與 JSON |

  - JSON 的 `firstFrame.withoutDrain`（沒送 drain 就出幀）只為相容保留，`immediate` 與 `nextAu` 都是 true，**不要**拿它
    判斷低延遲。
  - `firstFrame=<ms>` 從 STREAMON CAPTURE 起算，不是單幀解碼延遲。
  - **`nextAu` 可能是誤標**：OUTPUT queue 要入列 ≥ 2 個 buffer 才開始處理的 driver（vb2 的 `min_queued_buffers`），以及
    header 解析超過 1 秒的慢 firmware，也會被標成 `sourceChangeNeededNextAu` 或 `nextAu`，原因並不是要看到下一個 AU。
    iris 是不是這樣還沒查證；判讀時搭配 JSON 的 `secondAu` 與 OUTPUT 格式的 `minBuffersForOutputStatic`，最後以
    `decode-bench --fps 90` 的實測為準。
  - `firstFrame=none(…)` 的括號：`(drained, no frame)`＝`DEC_CMD_STOP` 成功送出後仍沒有幀；
    `(source change, capture needs reconfig)`＝CAPTURE 串流中途收到 `SOURCE_CHANGE`，driver 回了空的 LAST buffer，
    是解析度變更、CAPTURE 要重新配置，不是解不出來（`result=source-change-during-capture`，JSON
    `firstFrame.sourceChangeDuringCapture=true`；沒拿到 LAST 時印 `(source change during capture)`）；
    `(LAST without drain)`＝沒送 `DEC_CMD_STOP` 卻拿到 LAST；沒有括號＝三段都逾時（`result=no-frame`）。
    `firstFrame.drainIssued` 只有 `VIDIOC_DECODER_CMD` 成功才是 true，失敗原因記在 `firstFrame.drain`。
  - 等不到 `SOURCE_CHANGE` 時照舊式流程繼續，JSON 標 `captureSetupWithoutSourceChange`，後面的結果要搭配它判讀。
- **真正的每幀延遲不看 header-test**：header-test 只判斷出幀模式，測試幀的 SPS 也不是 Sunshine 的串流參數。延遲以
  `decode-bench <Sunshine 錄的樣本> --fps 90` 的 `lat p99` 與 `eagainMax` 為準（§6.5）。不帶 `--fps` 的連發模式下一個封包
  立刻就送，看不出「要等下一個 AU 才出幀」的延遲。
- **decoder 的定義**：m2m 而且 OUTPUT 至少有一個 compressed 的**視訊**格式。JPEG、MJPEG、PJPG、JPGL、DV、MPEG 多工容器
  這類非視訊格式（kernel 會自動替它們加上 COMPRESSED 旗標）不算，記在 `otherCodedFormats`，只有這類格式的節點標
  `role=non-video-decoder`（例如 mtk-jpeg、mxc-jpeg）。已知 codec 表以外、也不在排除清單上的格式（例如還沒查證的 AV1
  fourcc）照舊算 decoder。
- **`kind`** 只看已知 codec 表內的格式：`stateful`、`stateless`（Request API）、`mixed`；表外格式不預設成 stateful，
  只有表外格式時標 `unknown`。只宣告 CAPTURE＋OUTPUT 舊式旗標的 driver 也算 m2m，標 `legacyCaps`。
- **app 能不能用，看 `hasStatefulVideoDecoder`**：H.264／HEVC／AV1／VP9 的 stateful decoder（app 的 v4l2m2m 路徑只能用這種）。
  每個 decoder 的清單在 `statefulVideoCodecs`（log 的 `m2m decoders:` 行印成 `statefulVideo=[…]`），JSON 的 summary 有
  `hasStatefulVideoDecoder`，有 decoder 時 stdout 另印一行 `stateful video decoder: yes (…)` 或 `no (…)`（rc 13 時沒有這行，
  `hasStatefulVideoDecoder=false`）。G-α 的替代路徑條件與 PoC-0 看這一項，**不看結束碼**。只有 JPEG 之類格式的節點另計在
  summary 的 `nonVideoDecoders`。
- **開檔失敗**：記 errno、節點的 mode／owner、ACL、本行程的 `getgroups()`，用來判斷 SSH（沒有 active seat）下的
  uaccess 問題。`/dev` 看不到節點但 sysfs 有時，提示 Flatpak 需要 `--device=all`。
- **結束碼**：0 至少一個 m2m decoder（stateless 與 vicodec 的 FWHT 也算，所以 0 不等於 app 能用）；13
  `no m2m decoder visible (…)`，括號內是原因；10 非 Linux 建置。**header-test 失敗不改結束碼**（decoder 存不存在才是主要能力）。
- **builder 上的測試裝置**：`sudo modprobe vicodec multiplanar=1`（測完 `sudo modprobe -r vicodec`），預期看到 FWHT
  的 stateful decoder、stateless decoder、encoder 三個節點，結束碼 0 但 `hasStatefulVideoDecoder=false`（FWHT 不在清單內）；
  沒有 H.264／HEVC，header-test 會全部 skip。qemu 不轉譯 V4L2 ioctl，aarch64 dev 版在 builder 上應為 13。

### 6.5 `decode-bench`：用真實 bitstream 量解碼器（PoC-3、3b、3c、4）

```
viplestream decode-bench <file> [--codec auto|h264|hevc|h265|av1] [--decoder auto|sw|<名稱>|hwaccel:<type>]
                         [--out drm_prime|sw] [--fps N] [--frames N] [--loop N] [--warmup N]
                         [--capture-buffers N] [--output-buffers N] [--hold N]
                         [--drop-frames LIST] [--drop-every N] [--corrupt-frames LIST] [--flush-on-error]
                         [--compare-sw] [--map-vulkan] [--verbose] [--json <path>]
```

**輸入**：H.264／HEVC 的 Annex-B（`.h264 .264 .h265 .265 .hevc`），AV1 的 IVF（`.ivf`）。不連 libavformat，所以不吃 mp4。
`--codec auto` 依副檔名判斷。樣本用 `stream --dump-bitstream` 錄（§6.6）。

| 選項 | 說明 |
|---|---|
| `--decoder` | `auto`（預設）、`sw`、FFmpeg decoder 名稱（如 `hevc_v4l2m2m`）、`hwaccel:<type>`（如 `hwaccel:vaapi`、`hwaccel:d3d11va`） |
| `--out` | `drm_prime`（預設，`get_format` 選 DRM_PRIME，對應 L2）或 `sw`（系統記憶體幀，對應 L3） |
| `--fps N` | 依 i/N 排程送出，模擬串流；0（預設）＝連發量吞吐。**量延遲一律帶 `--fps`**（Frame 用 90）：連發模式下一個封包立刻就送，看不出「要等下一個 AU 才出幀」的延遲（§6.4） |
| `--frames N`／`--loop N`／`--warmup N` | 每輪最多 N 幀（0＝整檔）；重播 N 輪；前 N 幀不計入統計 |
| `--capture-buffers N`／`--output-buffers N` | v4l2m2m 的 buffer 數（2–256；預設照 app：capture `4 + PACER_MAX_OUTSTANDING_FRAMES + 2`、output 2）。PoC-3c |
| `--hold N` | 每張解出的幀延後 N 幀才釋放，模擬 renderer／GPU 持有 |
| `--drop-frames LIST`／`--corrupt-frames LIST` | 不送／中段翻轉位元組後才送這些幀。LIST 形如 `100,200-204`；單數別名 `--drop-frame`、`--corrupt-frame` 也收 |
| `--drop-every N` | 每 N 幀丟一幀（0＝關；N 至少 2） |
| `--flush-on-error` | 出錯後 `avcodec_flush_buffers()`，看能不能恢復 |
| `--compare-sw` | 另用 SW decoder 解同一檔（不丟幀）當參考，逐幀算 luma PSNR |
| `--map-vulkan` | headless libplacebo，每幀量 `pl_map_avframe_ex`（只有 libplacebo 建置）。PoC-4 |
| `--verbose` | FFmpeg log 升到 debug |

- **decoder 設定完全照 app**（`ffmpeg.cpp`）：LOW_DELAY、OUTPUT_CORRUPT、SHOW_ALL、`AV_EF_EXPLODE`、SW 的執行緒設定、
  硬體解碼 `thread_count=1`、`extra_hw_frames=1`、v4l2m2m 的 buffer 數。量到的就是串流時的行為。
- **auto 的選擇順序**：`<codec>_v4l2m2m` → 有 `HW_DEVICE_CTX` 的 hwaccel（d3d11va、vaapi、vulkan、vdpau、cuda、dxva2、
  d3d12va、videotoolbox、drm）→ SW（`h264`、`hevc`、`libdav1d`／`libaom-av1`）。每個候選先試解前 8 個 AU，能出幀才採用，
  再重開一個乾淨的 instance 計時。明確指定 decoder 時不試解。
- **幀號**：`--drop-*`、`--corrupt-*` 用的是**檔內第 k 幀（0 起算）**，`--loop` 時每一輪都套用。`--drop-every N` 丟的是
  `(k+1) % N == 0` 的幀，**永遠不丟 k=0**（第一個 IDR）。
- **時間基準**：`pts` 是全域幀號，`pkt_timebase` 固定 `{1, 1000000}`（v4l2m2m 靠它換算 timestamp）。這和 app 非 VR 串流
  的 1/90000 刻意不同；輸出 pts 無法配對時記 `ptsMismatch`。
- **統計欄位**：
  - 延遲＝送出到收到同一幀的時間（p50／p95／p99／max）；EOF 擠出的幀與 warmup 期間的幀不計入。`sendBlock` 是
    `send_packet` 本身的阻塞時間。
  - `eagainMax`：送出後、下一張幀輸出之前累積的最長封包數，約等於管線深度；`sendEagainMax` 另記 `send_packet` 回
    EAGAIN 的最長連續次數。
  - `stall`：有 `--fps` 時超過 3 個幀間隔沒有輸出，否則 100 ms。
  - `drm=`（首幀與結果行）：每個 layer 的格式都列（例如 VAAPI 匯出的 NV12 是 `R8+GR88`），接著是第一個 object 的
    modifier、object 數與各 plane 的 pitch；拿不到 DRM 描述時是 `-`。完整描述在 JSON 的 `firstFrame.drm.layers[]`。
  - 每個事件（drop／corrupt／error）一行：`errFrames`、`missing`、`stallMs`、`healFrames`、`idrNeeded`。
    `healFrames`＝事件後到 PSNR **穩定回到 40 dB 以上**的幀數（需要 `--compare-sw`，否則 `n/a`）；`idrNeeded=1`＝視窗內沒
    恢復，或是靠下一個 keyframe 才恢復（串流時一樣要 IDR）。彙總行的 `selfHealed`／`needIdr` 是兩者的計數。PoC-3b 看這裡。
  - **事件視窗與合併**：注入事件（drop／corrupt）的視窗到下一個注入事件（或結尾）為止，中間的 error 事件不會切斷它。
    同一次注入的後果常是斷斷續續的錯誤旗標，會另外冒出幾個 error 事件；落在視窗內、PSNR 癒合點之前（沒癒合就是整個
    視窗）的都併回注入事件，不另成一行、不另算 `selfHealed`／`needIdr`，事件行尾印 `merged=N`。癒合之後才出現的 error
    事件自己算。
  - **`idrNeeded=inconclusive`**：視窗被下一個事件截斷時 PSNR 還沒回到 40 dB，判斷不出再等會不會自己好；彙總行另計
    `inconclusive`，不算進 `needIdr`。`--drop-every` 很密、而 decoder 不會自己好時，大多數事件會是這個，只有視窗內碰到下一個
    keyframe 的才算 `needIdr`；解讀 PoC-3b 時一起看 JSON 的 `windowFrames`。彙總行尾端是 `inconclusive=N merged=N`
    （`events` 含被併的事件；`selfHealed`＋`needIdr`＋`inconclusive` 不含）。
  - JSON：`events[]` 依全域幀號 `g` 排序；被併的 error 事件緊接在所屬注入事件之後，只帶 `mergedInto`（注入事件的 `g`），
    注入事件帶 `mergedErrors`；截斷時 `idrNeeded` 是 `null`、`truncated: true`。`eventSummary` 有 `merged`、`inconclusive`
    （後者要 `--compare-sw` 生效）。事件行最多 20 行，JSON 最多 2000 筆（超過時 `eventSummary.jsonTruncated`）。
- **計時擾動**：`--compare-sw`、`--map-vulkan` 與首幀的 DRM descriptor 描述都在計時迴圈內，會擾動延遲與吞吐；JSON 的
  `notes` 會註明。量延遲的那一輪不要帶。
- **總幀數上限**：一次計時的總幀數（AU 數 × `--loop`）上限 10,000,000 幀（逐幀統計表約 0.4 GB）。超過時在開 decoder
  之前就以 rc 11 結束，訊息會提示降低 `--loop` 或 `--frames`。
- **結束碼**：0 完成；1 命令列錯誤（含沒給檔案）；10 沒有 FFmpeg；11 輸入檔讀不到或格式不對、參數語意錯誤（含總幀數
  超過上限）；13 沒有可用的 decoder，或明確指定的 decoder 一幀都解不出來；14 `send_packet` 連續 EAGAIN 超過 5 秒而中止
  （例如 `--hold` 太多、capture buffer 耗盡）、auto 選到的 decoder 在執行中才解不出幀，或逐幀統計表配置失敗（記憶體不足，
  照樣寫出 JSON）；15 JSON 寫檔失敗。

### 6.6 錄樣本：`stream --dump-bitstream <path>`（dev-only）

把 decoder 實際收到的 bitstream（組好完整 AU、送進 decoder 之前）錄下來，給 `decode-bench` 用。

```cmd
:: <dev-client>，先建好 temp\moonlight\VipleStream.exe
temp\moonlight\VipleStream.exe stream <host> Desktop --resolution 1920x1080 --fps 60 --video-codec HEVC ^
    --bitrate 20000 --display-mode windowed --dump-bitstream <樣本目錄>\hevc_1080p60
:: VR emulate 樣本（3456x1728@90 HEVC；host 的 vr_pcvr 不能是 disabled，例如 stub）
temp\moonlight\VipleStream.exe stream <host> Desktop --display-target pcvr --vr-emulate --dump-bitstream <樣本目錄>\vr_emulate_hevc
```

- **不持久化**：`--dump-bitstream` 的值只存在行程內，絕不寫進設定；下一次一般啟動不會繼續錄。**但 `stream` 的其他
  覆寫（解析度、fps、codec、碼率等）照既有行為會在 session 開始時寫回設定**，錄完要自行還原。
- **檔案格式**：H.264／HEVC 寫 Annex-B（`.h264`／`.hevc`），AV1 寫 IVF（`.ivf`，檔頭幀數固定寫 0）。路徑沒有已知副檔名
  時依 codec 自動補；副檔名和 codec 不符時改用正確的並警告；給的是既有目錄時產生 `viple-dump-<yyyyMMdd-HHmmss>.<ext>`。
  每個 AU 寫完就 flush（被強制結束也不掉尾端）。格式或解析度改變（decoder 重建）時關檔，另開 `<base>-<n>.<ext>`。
- **解析度受 host 顯示器限制**：host 顯示器的最大解析度會把請求夾住（`[VIPLE-RES] … host advertises max: WxH`），
  在 1080p 顯示器上要 1440p 拿到的仍是 1080p。以 `[VIPLE-BSDUMP] open … <W>x<H>` 與 decode-bench 的
  `bench: input … WxH` 為準。
- **內容要會動**：靜態桌面的 P 幀幾乎是空的，參考幀依賴很弱，掉幀／破損後的恢復（PoC-3b）不具代表性。
  PoC-3b 用的樣本要錄**持續變動的內容**（播放影片、遊戲、持續移動的視窗）。
- 樣本放 `<dev-client>` 的 `scripts\benchmark\samples\`（本機目錄，不進 git），要帶去 Frame 時交給
  `frame-poc-collect.sh --samples`（§7）。
- log：`[VIPLE-BSDUMP] armed …` → `open …` → 每 10 秒 `progress …` → `closed … reason=…`（見 `log_tags.md` §5c）。

---

## 7. Frame 實機：`frame-poc-collect.sh`

在 Frame 本機執行（Developer Mode 的 SSH，或桌面模式的終端機），**只在本機產生 tarball**，由 `<dev-client>` 用 scp 拉回。
同一套流程也是 G-β、G-rc 的 log 回收 SOP。

```
bash frame-poc-collect.sh [--out DIR] [--app-id ID] [--iface IF] [--rtt-target HOST]
                          [--with-app-probes] [--samples DIR] [--no-redact]
```

| 選項 | 說明 |
|---|---|
| `--out DIR` | 輸出目錄（預設 `~/viplestream-poc-<時間>`，必須是空的）；結束時產生 `DIR.tar.gz` 與 `DIR.tar.gz.sha256` |
| `--app-id ID` | 預設 `io.github.finaltwinsen.VipleStream` |
| `--iface IF` | Wi-Fi 介面（預設自動偵測第一個無線介面） |
| `--rtt-target HOST` | **opt-in**，本腳本唯一的網路動作：`ip route get`、基準 ping（50 次、0.2 s），再以 180 pps、232 B（對應 0x5506 pose 封包）量 1800 次 RTT 分佈（PoC-6）。沒給就完全不碰網路 |
| `--with-app-probes` | Day 1：以 `flatpak run` 跑 VipleStream 的探測（§6） |
| `--samples DIR` | 把樣本複製到 `~/.var/app/<app-id>/data/samples/`（沙箱看得到的位置）；搭配 `--with-app-probes` 時對每個樣本跑 decode-bench（連發、`--fps 90`、`--decoder sw` 各一次） |
| `--redact`／`--no-redact` | **預設遮蔽** MAC、SSID、IP、SteamID 等（只作用在收集出來的副本上，§7.3）；`--no-redact` 關閉 |

原則：不用 `set -e`，單一命令失敗照樣繼續，每條命令的 rc 與耗時記進 `index.tsv`（很多 rc 非 0 是「沒有這個裝置／
權限不足」，本身就是證據）；一般命令 60 秒逾時、探測 600 秒；**先複製既有的 app log，再跑任何探測**。

### 7.1 Day 0：唯讀探測（不需要 VipleStream）

```bash
scp moonlight-qt/scripts/frame-poc-collect.sh <frame-user>@<frame>:~/      # 在 <dev-client>
bash ~/frame-poc-collect.sh                                                 # 在 <frame>
```

| 收集項目 | 對應 |
|---|---|
| uname、os-release（含 `steamos-release`）、id、CPU／記憶體／磁碟、kernel cmdline、環境變數白名單 | meta |
| 既有 app log（Flatpak 與原生兩個位置） | 探測前先複製 |
| `/dev/video*`、`/dev/media*`、sysfs 名稱、相關 kernel 模組、dmesg／journal 的 VPU 訊息；有 `v4l2-ctl` 時逐節點列格式與控制項 | PoC-0 |
| `/dev/dri`、`vulkaninfo --summary`、Vulkan layer 目錄與 **gamescope WSI `.so` 是否存在**（決定 `host-os:ro` 要不要留） | PoC-0、PoC-7 |
| flatpak 版本、installation、remotes、已裝 app／runtime；app 已安裝時另跑 `flatpak info --show-permissions` 與沙箱內實際看得到的裝置、`/.flatpak-info`、openxr 目錄 | PoC-1 |
| `ip` link／addr／route、`/proc/net/wireless`、`iw` 的 link／info／**power_save**／station dump；（opt-in）RTT | PoC-6 |
| gamescope 行程的參數與環境白名單、`$XDG_RUNTIME_DIR` 內的 socket、Steam／SteamVR 行程 | PoC-7 |
| OpenXR active runtime（使用者層與系統層）、SteamVR 目錄、`openvrpaths.vrpath`、runtime JSON 的 `library_path` 與 `ldd` | PoC-F |

Day 0 也可以裝 Flathub 上游 Moonlight 當過渡方案與 PoC-0 的對照組（`vr_architecture.md` §7）。安裝範圍用 `--user`
還是 `--system`，PoC-1 確認後補進本文件。

### 7.2 Day 1：α 套件跑探測

```bash
# 裝 α bundle（release 產物，或 dev bundle）
flatpak install --user --bundle VipleStream-Client-<ver>-linux-arm64.flatpak
# 樣本先 scp 到 Frame 的某個目錄，再交給 --samples
bash ~/frame-poc-collect.sh --with-app-probes --samples ~/samples
```

`--with-app-probes` 依序跑（JSON 先寫在 `~/.var/app/<app-id>/data/poc-<時間>/`，最後複製進輸出目錄）：

| 探測 | 對應 |
|---|---|
| `v4l2-probe` | PoC-0、PoC-3（level／profile）。PoC-0 看 `stateful video decoder:` 那一行（JSON `summary.hasStatefulVideoDecoder`），不看 rc（§6.4） |
| `v4l2-probe --header-test all --expbuf` | PoC-3、PoC-4（EXPBUF）；會短暫佔用 VPU |
| `xr-probe` | PoC-2、PoC-F |
| `xr-probe --loader-debug` | PoC-F 失敗時看 runtime 搜尋與 dlopen（loader 輸出在 `.txt` 裡） |
| 每個樣本 `decode-bench <樣本>`：連發（auto）、`--fps 90`、`--decoder sw` 各一次 | PoC-3、PoC-3b、PoC-3c |

探測之後新產生的 log（`probe-*.log`、串流 log）另外複製一份到 `app-logs/after-*`。

**每幀延遲看 `--fps 90` 那一輪**（stdout 在 `probes/decode-bench-<樣本檔名>-fps90.txt`，JSON 在 `probes/json/` 底下同名的
`.json`）：auto 那一輪是連發模式，只看吞吐與能不能解，
看不出「要等下一個 AU 才出幀」的延遲。header-test 的 `out=` 判讀（§6.4）要拿 `--fps 90` 那一輪的 `lat p99`、`eagainMax`
佐證。這一輪依串流節拍送，樣本有 N 幀就要跑約 N/90 秒。

### 7.3 遮蔽與拉回

- 遮蔽是正規表示式替換：MAC → `<mac>`、SSID → `<ssid>`（`iw` 輸出的 `SSID:`／`ssid` 行）、IP 保留類別（`<ipv4-loopback>`、
  `<ipv4-private>`、`<ipv4-cgnat>`、`<ipv4-linklocal>`、`<ipv4-any>`、`<ipv4>`、`<ipv6-linklocal>`、`<ipv6-ula>`、`<ipv6>`），
  方便判讀 VPN／隧道介面有沒有吃掉私網路由。四段都 ≤ 255 的版本字串（如 `1.2.3.4`）也會被當成 IPv4。
- SteamID64（`7656119` 開頭的 17 位數）→ `<steamid64>`；`steamwebhelper` 命令列的 `-steamid=<非 0 的數字>` →
  `-steamid=<steamid>`；`-steamid=0` 刻意保留，它代表還沒登入，本身就是證據。序號（`serialno=`，含 `/proc/cmdline` 的
  `androidboot.serialno=`）→ `<serial>`。
- 主機名稱在收集端就不產生（`uname -srvmpio`，不帶 `-n`；`journalctl` 加 `--no-hostname`）。全域替換成 `<hostname>`（不分
  大小寫）只是後備，而且只在主機名稱不是通用字（`steamdeck`、`steamos`、`steamframe`、`frame`、`deck`、`steam`、`linux`、
  `localhost`）、長度至少 4 時才做：Frame 的預設主機名稱很可能是 `steamdeck`，全域替換會把 os-release 的 `VARIANT_ID`、
  行程參數裡的同名字串一起換掉，破壞證據。有沒有做全域替換記在 `REDACTED.txt`。
- 已知的文字類檔案（`.log`、`.txt`、`.tsv`、`.json`、`.conf`、`.vrpath`、`.vrsettings`、`.info`，以及 `app-logs/`、`probes/`
  底下全部）一律遮蔽，含 NUL 的也一樣（當機或沒電後被截斷的 log 尾端常有 NUL 填充，用 `grep -I` 篩選會把它判成二進位檔、
  跳過遮蔽卻照樣打包）。其他檔案看起來是文字就遮蔽；真正的二進位檔遮不了，不打包，搬到 Frame 上的
  `<輸出目錄>.unredacted/`（可能是當機證據，所以不刪，只留在本機），搬不走就直接刪除（寧可少一份證據，也不把沒遮蔽的
  內容打包出去），檔名都列在 `REDACTED.txt`。
- **刻意沒有遮蔽的**：帳號名稱與 `/home/<帳號>` 路徑、app log 裡 host PC 的名稱。實際遮蔽的類別、替換字樣與排除的檔案，
  以輸出目錄的 `REDACTED.txt` 為準。**遮蔽是規則式的，不保證完整**：節錄貼進本文件或任何公開位置之前，要人工再看一次。
- 拉回（在 `<dev-client>`）：

  ```bash
  scp <frame-user>@<frame>:<輸出目錄>.tar.gz <frame-user>@<frame>:<輸出目錄>.tar.gz.sha256 .
  sha256sum -c <輸出目錄名>.tar.gz.sha256
  ```

- 測試產物只用 scp 在機器之間直傳，不上雲。

### 7.4 Day 0／Day 1 實機結果（2026-09-28）

| 項目 | 結果 |
|---|---|
| 平台 | SteamOS 0.3.0（variant vr）、kernel 6.18 aarch64、帳號 `steamos`（群組 video、render、input）；Turnip Adreno 750（Mesa 26.x）；gamescope 3.16.28，socket `gamescope-0`；面板 4320×2160，SteamVR 預設 120 Hz（另有 108／96／90／80／72） |
| PoC-0 | **通過**。iris stateful 解碼器 `/dev/video-dec0`（→ video22），沙箱內（`--device=all`）可用。H.264（到 level 6.0）、HEVC、VP9；**沒有 AV1**（L1 的 AV1 選項在 Frame 上不存在）。capture 格式 NV12、NV21、Q08C（UBWC）、AB24。編碼器在 `/dev/video-enc0` |
| decode-bench（`hevc_v4l2m2m`，`--fps 90`） | 輸出 DRM_PRIME、線性 NV12（modifier 0）。VR 3456×1728：p50 4.50、p95 4.96、**p99 5.60 ms**、0 錯誤、`ptsMismatch=0`（G-rc 門檻 < 11 ms；靜態桌面樣本）。1080p：p99 約 3.2 ms。軟體解碼 3456×1728 只有 49 fps，PCVR 一定要硬解 |
| v4l2-probe header-test | H.264／HEVC `out=immediate`，首幀約 1.6–1.8 ms，EXPBUF 可用 |
| iris 對破損的反應（PoC-3b 一部分） | 掉參考幀、送破損幀、連發 IDR（最密 0.2 s）、扣住 8/10 張 capture buffer：iris 只把受影響的幀標 `errFlag`、繼續出幀，**從不卡住**。唯一會卡的是參數集改變的 IDR（§7.5） |
| PoC-6 Wi-Fi | 6 GHz、160 MHz、約 −67 dBm、省電開啟。5 pps RTT p50 8.1／p95 44 ms（省電）；180 pps p50 2.67／p95 3.76／p99 6.22 ms、0% loss。串流時丟幀率變動很大（0.2%～4.6%），戴著頭盔時明顯變差 |
| PoC-F | 見 `moonlight-qt/scripts/steamframe/flatpak/finish-args.md` §6：函式庫相容，但 Flatpak 的 PID namespace 讓 SteamVR IPC 出錯，**曾導致 Frame 的 SteamVR 重啟** |
| α 啟動路徑 | 用 `steamos-add-to-steam <可執行檔>` 加成非 Steam 遊戲，由 Steam 啟動：app 跑在 Xwayland（`DISPLAY=:1`），Vulkan 走 gamescope WSI，畫面是 SteamVR dashboard 的 `valve.steam.desktopgame.<appid>` overlay。從 SSH 以 `steam steam://rungameid/<gameid>` 可觸發同一路徑 |
| PlVk 前端 | 修正前 `pl_map_avframe_ex()` 失敗（iris 的 DRM_PRIME 是單一 layer、兩個平面的 NV12，libplacebo 只吃每平面一個 layer），退回 SDL/OpenGL。§SF-DRMSPLIT 改寫成 R8＋GR88 後 `frontend=PlVk`，log 見 `log_tags.md` |
| client log 位置 | `~/.var/app/<app-id>/cache/VipleStream/VipleStream/logs/VipleStream-<時間>.log` |

### 7.5 G-α 凍結根因（2026-09-28）

**現象**：串流幾秒到十幾秒後畫面定格，之後收到 SIGTERM 也結束不了。`[VIPLE-FREEZE]` 顯示
`dec=submitPacket …、in=out、liveFrames=1、render=waitQueue`：解碼執行緒卡在 `avcodec_send_packet` 內的 V4L2 poll，
app 沒有扣住任何 capture buffer。另一種結局是 `avcodec_receive_frame() failed: End of file` 連續 20 次後重置 decoder。

**根因**：
1. Wi-Fi 掉包讓 host 的 ABR 降碼（reset＋IDR）。
2. Sunshine 的 NVENC 把 level／tier 交給自動選擇，降碼後在下一個 IDR 依新碼率重選：實測 HEVC level 4.1 從 26 Mbps 的
   **High tier 變成 18 Mbps 的 Main tier**（Main tier 上限 20 Mbps），VPS／SPS 的 profile_tier_level 一個位元組改變。
3. iris 看到參數集改變就發 `V4L2_EVENT_SOURCE_CHANGE`。FFmpeg v4l2m2m（`v4l2_handle_event`）在解析度沒變時只送
   `V4L2_DEC_CMD_START`，iris 卻要 capture queue 重新 STREAMON；兩邊互等。

戴頭盔時掉包多、ABR 常降到 20 Mbps 以下，所以看起來「只在頭盔顯示中發生」；頭盔待機時也能重現。用
`stream --dump-bitstream` 錄下凍結前的 bitstream，`decode-bench` 重播必定卡住（凍結前的 IDR tier 位元翻轉）。

**修法**（兩層）：
- server：`nvenc_base.cpp` 的 `pin_level_for_reconfigure()` 在初始化後讀回 NVENC 實際寫出的 SPS，把 level（HEVC 另加
  tier）固定進 ABR reconfigure 用的設定，整個 session 的參數集不再改變。
- client：`ffmpeg.cpp` §SF-PARAMSETS，v4l2m2m decoder 遇到參數集改變的 IDR 時不餵進舊實例，改成重建 decoder 再要 IDR
  （對上 vanilla Sunshine 或其他會改參數集的 host 也不會卡死）。

**另外記下的**：Linux 上的 `[VIPLE-INPUT-STALL]` 是誤報（只有 Windows 查真的輸入，非 Windows 一律當作有輸入，PlVk 在
Pacer 執行緒繪製時主迴圈閒置就會被記）。

### 7.6 G-α 第二、三輪與通過（2026-09-29）

結果在 `scripts\benchmark\results\galpha-20260929-r3\`（本機，不入 git）。**G-α 通過**：

| 組 | Frame 畫出 ÷ host 送出（門檻 ≥ 99%） | 解碼 p50 | 網路丟幀 | 參數集重建 | 核心錯誤 |
|---|---|---|---|---|---|
| 1080p60 20 Mbps（15 分鐘） | 99.75% | 1.97 ms | 0.88% | 0 | 0 |
| 1440p60 25 Mbps（15 分鐘） | 99.37% | 2.04 ms | 0.66% | 0 | 0 |
| 1080p120 25 Mbps（15 分鐘） | 99.47% | 1.90 ms | 0.51% | 0 | 0 |

使用者目視確認畫面順暢、絕對滑鼠的雷射指向正確。判定方式的兩個修正（使用者決定）：
- **幀率改看「送出 vs 畫出」**：host 送出取 sunshine.log `[VIPLE-BCAST-RATE]` 在該組時段的加總，Frame 畫出取
  `[VIPLE-PRESENT-Stats] cumul`。1080p120 那組 host 只送出約 96 fps（host 擷取端，不查），Frame 全收全畫。
- **頓挫率不列入**：Frame 輸出 120 Hz，60 fps 內容本來就隔一次刷新換一幀；到達時間稍有抖動就有幀多停一次刷新
  （25～31 ms），貼著 33.3 ms 的門檻被算進頓挫，但人眼看不出來。

**途中修掉的問題**：
1. **核心錯誤（kernel Oops）**：第二輪 1440p60 在第 3 分鐘，`PacerRender` 執行緒於 `DRM_IOCTL_PRIME_FD_TO_HANDLE`
   → `msm_gem_prime_import` → `msm_gem_import` 失敗後的清理路徑 `drm_gem_put_pages` NULL deref。SteamOS 6.18 msm 驅動錯誤路徑
   的 bug，被 libplacebo `pl_map_avframe_drm`「每幀每平面新建一次 dmabuf 匯入」觸發（60 fps＝每秒 120 次）。執行緒永遠卡在
   核心、行程成殭屍，SIGKILL 無效，只能重開機。修法 §SF-DMABUF-CACHE：PlVk 自己處理 DRM_PRIME，依 dmabuf 身分
   （fstat 的 dev/ino＋offset/pitch/fourcc/modifier/尺寸）快取 `pl_tex`，解碼器重建（hw_frames_ctx 改變）時整批清掉，
   上限 64 筆（最久沒用的先釋放）。每場只匯入 6～8 次，45 分鐘 0 次核心錯誤。
2. **從啟動程式開 GUI 閃一下就消失**：不是縮到背景，是 SIGSEGV。GUI 啟動的解碼探測測試幀，iris 回報寬 1344、
   pitch 只有 1280（buffer 大小＝1280×736×1.5），`pl_tex_create` validation 失敗後程式崩潰。§SF-PITCHCLAMP 把寬度夾到
   pitch 內、裁切範圍一併夾住；命令列串流的 1080p／1440p 不會觸發。
3. **頭盔放著會休眠**：SteamVR `power.turnOffScreensTimeout` 預設 5 秒（頭盔靜止就關螢幕），SteamOS 的系統睡眠設定管不到。
   手改 `steamvr.vrsettings` 會在 SteamVR 關閉時被覆寫，要在執行中用
   `/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-float power.turnOffScreensTimeout 86400`（`--set-settings-bool
   power.pauseCompositorOnStandby 0`）。vrserver log 的 ` - 0 - entering standby` 才是頭盔，1、2 是控制器。
4. **命令列參數會被存成設定**：`Session` 開始串流時 `save()` 偏好設定，`--quic` 之類的覆寫會留在設定檔（第二輪因此全走
   QUIC、作廢）。驗測腳本一律明確帶 `--no-quic`。
5. **殭屍行程後遠端重開**：`ssh … /usr/bin/steamos-polkit-helpers/steamos-reboot-now`（polkit `allow_any=yes`）。必須在
   SSH 前景執行；`setsid -f` 背景化會被 pkexec 以 "Refusing to render service to dead parents" 拒絕。

**QUIC 在 Frame Wi-Fi 上的丟包（2026-09-29 查明）**：兩個原因。
1. 多路徑配到死路、server 選到未驗證的路徑（§MP-ROUTECHK、§MP-VERIFIED，fb1defbc）：丟幀 13.9% → 2.33%。
2. **QUIC 封包沒有 DSCP**：UDP 影像 socket 由 qWAVE 標 DSCP 40（CS5，Wi-Fi 基地台放進 WMM 影像佇列），QUIC 是 0（best effort），
   在 Wi-Fi 上會一次連續掉好幾包、超過 FEC。qWAVE 無法標記共用的 QUIC socket（`QOSAddSocketToFlow` 回 1168），改由
   `add-firewall-rule.bat` 建立永久的原則式 QoS `VipleStream-Server-QUIC`（`viplestream-server.exe` UDP 來源埠 48010 → DSCP 40；
   `delete-firewall-rule.bat` 移除）。**已安裝的 host 要重跑一次 `scripts\add-firewall-rule.bat` 才會生效**（自我更新不跑）。
   交錯 A/B（Frame、QUIC、60 秒）：DSCP 40 丟幀中位數約 0.8%、無 DSCP 約 2.7%；緊接著的同環境對照 UDP 1.30%、QUIC＋DSCP 0.27%。
   Wi-Fi 變異大（個別一次 8.6%），結論屬強烈傾向。細節 `scripts\benchmark\results\mpq-20260929\report.md`（本機）。

---

## 8. S3／PoC-F-pre（延到 M3a 開頭）

PoC-F-pre 要回答：**KDE runtime 的 Flatpak 沙箱內，能不能載入 SteamVR 的 OpenXR runtime？**（R2 的前半；Frame 上的
PoC-F 用同一個 `xr-probe` 回答後半。）做法是 `<builder>` 上隔離的 SteamVR Linux null driver，加 x86_64 dev Flatpak 的
`xr-probe`。

- **使用者 2026-09-28 決定延到 M3a 開頭**：安裝 Steam／SteamVR 需要 Steam 帳號登入。M2a 只備妥腳本與 SOP。
- 腳本與步驟：[`moonlight-qt/scripts/steamframe/s3/README.md`](../moonlight-qt/scripts/steamframe/s3/README.md)
  - `check-gamescope-openvr.sh`：唯讀檢查 gamescope 有沒有 openvr backend（0 有、3 沒有或無法判斷、4 沒裝）。
  - `write-null-driver-vrsettings.sh`：寫 null driver 設定，預設 dry-run，`--apply` 才寫；只接受帶 opt-in 標記檔的隔離 Steam，
    寫前備份。原生 Steam 要在 S3 專用帳號下有 `~/.viple-s3-isolated`；Flatpak 版 Steam 也要有標記檔
    `~/.var/app/com.valvesoftware.Steam/.viple-s3-isolated`：Flatpak 只把它和原生 Steam 分開，分不出是不是使用者日常用的
    那一份。所以只能用專為 S3 新裝的 Flatpak Steam；日常就用 Flatpak Steam 的話不要建立標記，改用專用帳號的做法
    （步驟見 s3/README.md）。
  - `run-xr-probe-steamvr.sh`：沙箱內跑 `xr-probe`（auto、explicit、沙箱內 ldd 三種），結束碼＝explicit 那一次的結束碼。
- 所有 S3 腳本都**不登入、不啟動 Steam、不安裝任何東西**。結果連同 `summary.txt` 記進本文件。

---

## 9. 已知風險

1. **Qt 6.11 是新變數**（Windows 與 builder 目前都在 6.10.x）。出現 Qt 6.11 專屬問題時退回 `runtime-version: '6.10'`
   （同為 freedesktop 25.08 基底、仍在維護）；manifest 與 `build-steamframe.sh` 的 `KDE_BRANCH` 要一起改。
2. ~~gamescope WSI 的 aarch64 layer UNVERIFIED~~（2026-09-28：Frame host 有這個 layer，串流時 WSI 生效）：SteamOS ARM 是否提供 `libVkLayer_FROG_gamescope_wsi_aarch64.so`、放在哪裡，
   都沒有證據。沒有時 Vulkan loader 只會略過（無害），但 `--filesystem=host-os:ro` 就沒有存在理由，要拿掉。Day 0 會列出
   host 上的 layer 檔案。
3. **`--filesystem=~/.steam:ro` 會暴露 `~/.steam/registry.vdf`**（含 Steam 帳號名）。Frame 上用不到（SteamVR 在 `/opt/steamvr`），M3a 拿掉。
4. **沙箱內的 OpenXR（R2）**：2026-09-28 Frame 實測，函式庫相容，但 Flatpak 的 PID namespace 讓 SteamVR IPC 出錯（§7.4、finish-args.md §6），β／rc 的包裝形態在 M3a 開頭決定。原本的說明——沙箱內的 OpenXR loader 看不到 host 的 `~/.config/openxr`：由 `XrRuntimeJson` 解析 host 路徑，在行程內設
   `XR_RUNTIME_JSON`（§6.3）。SteamVR runtime 的 `.so` 依賴 Steam Runtime 的函式庫，和 KDE runtime 的 glibc／libstdc++
   是否相容 **UNVERIFIED**（PoC-F-pre、PoC-F）。
5. **libplacebo 選 B（v7.360.1）** 和 FFmpeg 9 的 libav helper 能不能編過 UNVERIFIED；不行就退回 A（補丁一定要帶，否則
   gamescope 下 SIGABRT）。
6. **qemu 建置時間**（G-BUILD，§4）：超標時 U7 提前。
7. ~~`--device=all` 在 SSH 下的 uaccess~~（2026-09-28：SSH 下可開 `/dev/video-dec0`）：Developer Mode 經 SSH 跑 `flatpak run` 時，`/dev/video*` 權限是否因為沒有
   active seat 而不同 UNVERIFIED；`v4l2-probe` 開檔失敗時會記下判讀所需的資訊（§6.4）。
8. **release 產物的 `/app/manifest.json` 會跟著 `.flatpak` 發佈**：主 repo 來源已改成一律寫公開 URL，還沒 push 時產物
   命名 `-unpublished`（§3.2）。風險剩在人為操作：`-unpublished` 的 bundle 被改名上傳，或發佈前沒用 §3.2 的指令驗
   `/app/manifest.json`。

---

## 10. 驗證紀錄

### 驗證紀錄（M2a，2026-09-28）

| 項目 | 環境 | 結果 |
|---|---|---|
| Windows 建置 | `<dev-client>` `build_moonlight.cmd --no-bump` | 通過；新檔 0 警告，無 LNK 警告 |
| smoke `--help` ×2、`--version` | Windows、x64 AppImage（純 SSH／GUI env）、x86_64 Flatpak（純 SSH／headless Wayland）、aarch64 Flatpak（qemu，兩種環境） | 全部 rc=0；aarch64 確認是 AArch64 ELF（e_machine 0xb7）、FFmpeg lavc 63.1.100 |
| 三個 probe `--help` ×2 | Windows、AppImage | rc=0 |
| `v4l2-probe` | Windows → rc=10；AppImage 無裝置 → rc=13；`vicodec multiplanar=1` → 正確分出 encoder／stateful／stateless，`stateful video decoder: no`；Flatpak 沙箱內看得到 vicodec（`--device=all` 生效）；aarch64（qemu）→ rc=13 | 符合預期；vicodec 測完即卸載 |
| `xr-probe` | Windows／AppImage → rc=10 並列出 runtime JSON 候選；Flatpak（兩種 arch）→ loader 已連上、無 runtime → rc=13 | 符合預期（PoC-F-pre 延到 M3a） |
| `decode-bench` | Windows sw／d3d11va、AppImage sw／VAAPI、Flatpak 沙箱內 VAAPI（p50 2.7 ms）、aarch64 qemu sw | 全部 rc=0，`ptsMismatch=0`；VR 樣本 3456×1728 丟幀注入被偵測（靜態桌面內容所以 PSNR 未掉，PoC-3b 要錄動態內容） |
| `stream --dump-bitstream` | `<dev-client>` → `<host>`：HEVC 1080p60、H.264 1080p60、VR emulate 3456×1728@90 | 正常錄製；VR flags 不寫回 QSettings；其他 CLI 覆寫由錄製腳本逐項還原 |
| 一般 session 回歸（不變式 5） | `<dev-client>` → `<host>` 30 s | 61.3 fps、解碼 0.39 ms、無 BSDUMP 行（M1a 基準 59.5 fps／0.37 ms） |
| Flatpak 串流（x86_64，headless gnome-shell） | `<builder>` → `<host>` | `[VIPLE-LNXFE] source=libdrm [amdgpu]`；auto → VAAPI＋EGL/GLES；`linuxVideoFrontend=vulkan` → `frontend=PlVk backend=VAAPI`；`WAYLAND_DEBUG` 的 `set_app_id` 全部是 app-id；繼承 `SDL_VIDEODRIVER=x11`／`SDL_VIDEO_X11_WMCLASS=bogus` 時仍是 `sdl-driver=wayland`、app-id 正確（OVERRIDE 生效） |
| 洩漏 grep | commit 前 | 乾淨 |

觀察（非 M2a 回歸，G-α 時留意）：`<builder>` 的 Flatpak 串流 10 次中有 2 次 `serverinfo` 5 秒逾時、1 次卡在解碼器探測
（headless compositor 下），重跑即正常。兩種 arch 都裝在同一台時，`flatpak run` 不帶 `--arch` 可能挑到 aarch64（在 qemu 下跑），
測試一律明寫 `--arch`。

## 8. β：XR 虛擬螢幕（M3a）

`--display-target xr-desktop`：client 自己開 OpenXR session，把串流畫面畫在前方的虛擬螢幕上，控制器射線當滑鼠。
程式碼在 `app/streaming/xr/`（XrContext、XrVideo、xrdesktopscreen、xrinput）與
`app/streaming/video/ffmpeg-renderers/xrrenderer.*`，只在 `CONFIG+=openxr` 建置（Flatpak 兩種 arch 一律帶；
Windows 本機 `build_moonlight.cmd --openxr`）。log tag：`[VIPLE-XR]`、`[VIPLE-XR-INPUT]`（`docs/log_tags.md`）。

### 8.1 架構

```
startConnectionAsync 開頭（/launch 與 relay 之前）：XrContext bring-up（5 s）
  失敗 → 退回平面（不變式 5）
XR frame thread（XrContext 擁有）：xrWaitFrame／BeginFrame／EndFrame、送 layer（影像 quad 或 cylinder、
  狀態條、指標）、xrSyncActions→射線求交→SDL_USEREVENT→main thread→Li*Mouse*
decoder thread → XrRenderer::renderFrame：只放 mailbox（latest wins）
影像 render thread（XrVideo）：pl_vulkan_import 共用 XrContext 的 VkDevice，pl_map_avframe_ex→render→release；
  XR thread 只送最後 release 的影像（＝最後一幀複本，decoder 重建時不動）
```

- 擺放：第一次 FOCUSED 依頭部水平朝向擺在正前方 1.5 m、寬 60°；Ctrl+Alt+Shift+R recenter。runtime 有
  `XR_KHR_composition_layer_cylinder` 就用 cylinder（SteamVR 沒有，走 quad）。
- stale：1 s 沒新幀疊狀態條、5 s 改顯示 loading。
- 輸入：trigger 左鍵、squeeze 或 B 右鍵、搖桿 Y 捲動；XR FOCUSED 期間忽略平面視窗滑鼠（鍵盤照常）；失去
  FOCUSED 放開全部。
- 虛擬鍵盤：影像螢幕下方、拉近 30 cm、後仰 30°，寬為螢幕的 62%。開關：Touch 左 menu、Index 左 thumbstick click、
  Frame 左 view、Ctrl+Alt+Shift+K、鍵盤上的 ✕。修飾鍵黏滯一次；「中/英」送 Shift 單擊（host 端 Windows 注音的
  中／英切換）。貼圖由背景執行緒畫（Windows 首次畫字要初始化字型系統），XR thread 只上傳；按鍵高亮是疊在鍵上的
  半透明 quad。
- 生命週期（X5）：runtime 失效（LOSS_PENDING、instance loss、`xrWaitFrame` 回 SESSION_LOST／INSTANCE_LOST）
  → 刪 decoder、拆 XrContext、以 0.5／1／2 s 退避重建最多 3 次，之後經 `SDL_RENDER_DEVICE_RESET` 重建 decoder
  （有新 context 接回 XR，沒有就平面）；runtime 發起的 EXITING（例如使用者關掉 SteamVR）→ 不重建、直接平面，
  串流不中斷。PCVR 的「VR session 後失敗送 /cancel」留到 M4a。
- 無頭（Linux 沒有 Wayland／X11，例如經 SSH 或 Steam 以 OpenXR app 直接啟動）：Qt 用 offscreen（不讓 EGLFS
  搶 DRM master）、SDL 用 offscreen driver、VAAPI 改開 DRM render node、XR 模式不用 Pacer 節拍。

### 8.2 CLI

```bash
VipleStream stream <host> Desktop --display-target xr-desktop
# dev（不寫入設定）：
#   --xr-runtime-json <path>   行程內指定 OpenXR runtime manifest
#   --xr-dump-frame <path>     第 300 幀影像 quad 讀回 PNG（最長邊 ≤ 1280）；補幀開著時存六組、每組兩張＋估位移的輸入三個檔（見「半速串流與補幀」）
#   --xr-test-stall-ms N       第一幀 20 s 後丟 N ms 影像（驗 stale）
#   --xr-test-recenter-sec N   bring-up 後 N 秒 recenter
#   --xr-test-pointer          合成射線（4 s 一圈、圓頂按 trigger）
#   --xr-test-keyboard "<文字>"  鍵盤出現後合成射線逐鍵輸入
#   --xr-dump-keyboard <path>  鍵盤貼圖 PNG（量效能時不要帶）
#   --xr-test-fail bringup|loss|loss3|exit   失敗注入（見 8.4）
VipleStream xr-probe --session [--duration N] [--xr-runtime-json <path>]   # session 與 frame loop 量測
VipleStream xr-probe --selftest-ray                                         # 射線求交自測
```

解析度／更新率「自動」（§H.4-AUTO，2.0.0 起新安裝的預設值；CLI 用 `--resolution auto`、`--fps auto`）在
xr-desktop 模式下：解析度固定 **2560x1440**；更新率在 XrContext bring-up 後（/launch 之前）取 **XR runtime
實際顯示更新率**（與 `waitViews` 同規則：runtime 回報與量到的 predictedDisplayPeriod 不一致時以週期為準），再與
host `<DisplayModeSwitchable>` 回報的最高 Hz 取小。FRUC 開著時不改 server fps。log 看
`[VIPLE-RES] auto fps (XR desktop)`（見 `log_tags.md`）。明確帶 `--resolution WxH`／`--fps N` 時照舊用明確值。

### 8.3 模擬環境

| 代號 | 環境 | 腳本（dev-only） |
|---|---|---|
| S1 | `<builder>` 上無頭 Monado（null compositor＋模擬 HMD），client 用 x86_64 dev Flatpak | `moonlight-qt/scripts/xr-s1-monado.sh [--stream <host>]` |
| S2 | `<dev-client>`（Windows）SteamVR null driver，client 用 `build_moonlight.cmd --openxr` | `moonlight-qt/scripts/xr-s2-steamvr-null.ps1 -Mode on|start|off` |

- S1：腳本把主機的 Monado OpenXR client（`libopenxr_monado.so`＋`libcjson.so.1`）與一份 runtime JSON 放到
  `~/.var/app/<app-id>/data/xr-s1/`，`flatpak run` 只在測試時加 `--filesystem=xdg-run/monado_comp_ipc` 與
  `LD_LIBRARY_PATH`（不改 finish-args）。Monado 25 client 最高需要 GLIBC_2.38，KDE runtime 6.11 是 2.42。
  monado-service 會 epoll 監看 stdin，`< /dev/null` 起不來，腳本用一條不結束的 pipe。null compositor 固定
  20 Hz（`XRT_COMPOSITOR_DEFAULT_FRAMERATE` 對它無效）。兩種 arch 都裝時一律寫 `--arch`、`//dev`。
- S2：OpenXR app 只會拉起 vrserver，要先 `-Mode start` 完整啟動 SteamVR；null HMD 不動會進 standby、沒有控制器時
  dashboard 搶焦點，`-Mode on` 一併關掉。系統的 OpenXR active runtime 不改（用 `--xr-runtime-json` 指 SteamVR），
  SteamVR 會跳「未設為預設 runtime」通知，不影響，不要按。

### 8.4 驗證紀錄（2026-09-30）

| 項目 | S2（Windows SteamVR null、軟解） | S1（Monado 無頭、VAAPI） |
|---|---|---|
| session | FOCUSED、30 s 4109 幀 0% miss、90 Hz | FOCUSED、30 s 601 幀 0% miss、20 Hz、bring-up 110～130 ms |
| 影像進 XR（1080p60） | 每 10 s 畫出 581～589 幀、XR thread cpu p95 0.8 ms | 每 10 s 畫出 582～591 幀、render thread cpu p50 8.2 ms |
| 讀回畫面 | 內容正確 | 內容正確（底色比 S2 略亮，待查） |
| 射線滑鼠（合成） | 命中 99.9～100%，host 游標到位；selftest 12/12 | — |
| 虛擬鍵盤（合成輸入 `Hello VR 123`） | 15 次按壓全送出，VK 序列正確（H／V／R 前自動黏滯 Shift，下一鍵放開後解除）；鍵盤開啟期間 XR 漏幀 0%；首次貼圖背景畫 534 ms | — |
| 失敗注入 bringup | 退回平面，串流持續 | — |
| 失敗注入 loss | 0.5 s 後重建成功（bring-up 608 ms），影像接回 XR | — |
| 失敗注入 loss3 | 3 次重建（0.5／1／2 s）全失敗 → 平面 | — |
| 失敗注入 exit | FOCUSED→…→EXITING → 平面，不重建 | — |
| 正常結束 | EXITING → destroyed，rc 0，不觸發重建 | — |
| 無頭（offscreen） | 不適用 | Qt offscreen、SDL offscreen、VAAPI DRM render node |

### 8.5 已知限制與待辦

- Frame 控制器綁定用 `/interaction_profiles/valve/frame_controller_valve`（Valve OpenXR Unity 套件文件；右手 a/b/x/y、
  左手 dpad_up/left/down/right），SteamVR 2.17.10 已接受；真控制器行為待實機確認。
- Windows XR＋硬解（D3D11VA）：幀用 `av_hwframe_transfer_data` 搬到系統記憶體再上傳（1080p p50 3.6／p95 5.0 ms），
  不再退到 PlVk 自建 Vulkan device（那會讓 SteamVR compositor 停頓，開場漏幀約 5%）。讓 FFmpeg Vulkan 硬解共用
  XrContext 的 VkDevice 可省掉搬移，但 SteamVR 回報 OpenXR 的 Vulkan 上限 1.2、FFmpeg Vulkan 解碼要 1.3 等級功能，
  暫不做；4K 或高幀率時搬移成本需重估。Frame 用 DrmRenderer（DRM_PRIME 直接匯入），不受影響。
- 虛擬鍵盤：Shift 雙擊鎖定（Caps）、數字鍵盤、長按字元未做；Frame 左 view 當開關鍵待實機確認（不被接受時會自動
  去掉，改用 Ctrl+Alt+Shift+K 或藍牙鍵盤）；UNORM 格式的 swapchain 上貼圖會略亮（runtime 當線性值）。
- PoC-2b（β 的啟動形態：overlay 內子行程、同行程切換、Steam 直接以 OpenXR app 啟動）與 G-β 要在 Frame 實機做；
  在 Frame 上跑任何 XR 程式前要先通知使用者（曾讓 Frame 的 SteamVR 重啟）。

### 8.6 PCVR 走真 XR（M4a R1）

- `stream <host> <app> --display-target pcvr`（不帶 `--vr-emulate`）：XrContext 以 PCVR 模式 bring-up（參考空間
  STAGE→LOCAL_FLOOR→LOCAL，不建射線滑鼠／鍵盤），量每眼 FOV／eyeToHead／Hz 填 `/launch`；HMD 樣本經 0x5506 送出，
  server 回聲的 render pose（0x81）決定 projection layer 的每眼 pose；stale >500 ms 疊半透明黑、>2000 ms 顯示 loading（§VR-STALE；2026-10-03 前是 100／250 ms）。
- S1 驗證：`bash moonlight-qt/scripts/xr-s1-monado.sh --arch x86_64 --rotate --display-target pcvr --stream <host>
  --stream-args '--no-quic --vr-eye 1024x1024'`（`--rotate` 讓 Monado 模擬 HMD 旋轉；server 需 `vr_pcvr = stub` 或
  `enabled`）。2026-09-30 對本機 service（stub）60 s：projection 每 10 s 約 200 幀（20 Hz 每幀）、noMeta 0～1、回聲
  100%、tracking thread 模式 120/s、XR 漏幀 0%；`--xr-test-stall-ms 400`：live→fade（124 ms）→loading（274 ms）→live。
- **M4a R2 控制器與 haptic**：0x5506 帶左右 grip pose、按鍵、pressCtr、flags；menu＋trigger 1 s → SYSTEM；
  0x5508 HAPTIC → `xrApplyHapticFeedback`（log 見 `log_tags.md` 的 `[VIPLE-VR-INPUT]`）。S1 加
  `--controllers simple`（Monado 模擬左右控制器）與串流參數 `--vr-test-input --vr-test-haptic`：2026-09-30 對本機
  service（stub）45 s：兩手 active、tracking 120/s、pressCtr 與合成序列相符（左 SYSTEM／MENU／TRIGGER／GRIP、右
  A／B／TRIGGER）、systemCombos 每 8 s 一次、haptic applied 2～4／10 s、failed 0、XR 漏幀 0%、回聲 100%。
- 待辦：server 送 HAPTIC（`set_haptic_sink` 未註冊）、Frame 左手面鍵幾何、LATCH、XR 在 VR session 後失效時的
  `/cancel`、Hz 夾值時 period 與實際顯示不符的影響（S1 限定）、真 driver（`vr_pcvr = enabled`）端到端。
  （R3／R4 已補：HAPTIC sink、LATCH、真 driver 端到端。）
- **M4a R4 真 driver 端到端與失敗注入（2026-09-30）**：S1（Monado 無頭、模擬 HMD 旋轉＋左右控制器）→ `<dev-server>`
  （Windows service、`vr_pcvr = enabled`，app＝SteamVR Home：`vr_pcvr=enabled` 時由 Steam 250820 合成的 vr 類 app）。
  - 雙顯卡筆電：`pick_vr_adapter()` 在多張硬體卡中自動選「唯一接顯示輸出」的那張（log `[VIPLE-VR-CAP] adapter
    auto-pick`），否則 `/serverinfo` 不宣告 PCVR（b1）。
  - 端到端：編排器 IDLE→…→ACTIVE 約 7.2 s；driver 每 10 s vsync／present 600（60 Hz）、stale 0；讀回為 driver
    合成的 SBS（左右眼視差、兩手控制器模型）；暖機後回聲 100%、XR 漏幀 0%、tracking 120/s；LATCH 暖機後
    slackEma 收斂到 3.8～4.1 ms（目標 4 ms）、ppm 穩定於 −70～−59。
  - 500 ms 斷線（linux-builder：出方向 `tc netem loss 100%`、入方向 `iptables INPUT DROP`，只針對 server）：
    fade 141 ms→loading 293 ms→約 1 s 後回 live，LOSS→REFRESH_START 3 ms、恢復 150 ms、不退 IDR，session 不斷。
    **不要用 `iptables OUTPUT DROP` 注入**：本機 send 回 EPERM，ENet 直接結束連線（`Connection terminated: 1`），
    不代表真實掉包。
  - kill server（service 的 server 子行程被強制結束）：driver 89 ms 內 `pipe lost -> standby (hmd stays
    connected)`，新 server 起來後重新握手；client 約 10 s 後 ENet 逾時、XR 正常拆除、不崩潰；`/serverinfo` 恢復。
  - guard：session 結束後編排器 disarm→RESTORE_PENDING（SteamVR 不自動關，已知）；SteamVR 結束後下一次維護
    tick 還原 `steamvr.vrsettings`（`forcedDriver`、`driver_vrlink.enable` 與備份逐鍵一致）；server 當機重啟後
    也依 marker 補還原。
  - 修正：XR PCVR 每 10 s 約 2 次閃 loading（meta 在 render 後才查、ring 碰撞 → 改 render 前查一次、無 meta 幀在
    acquire 前丟棄、ring 128→512）；`xr-probe --session` 自鍵盤 commit 起 SIGABRT（QPainter 需要 QGuiApplication
    → 無 QGuiApplication 時不畫鍵盤）；S1 腳本到時對沙箱內 client 送 SIGTERM 並等它結束後才停 Monado（先停
    Monado 會讓 client 卡在 VAAPI 匯出的 fence 等待）。
  - 待辦：server 轉送 HAPTIC 的實測（需要會震動的 VR app 或登記 `vr_probe`）；連線錯誤後 CLI `stream` 在無頭
    XR 下不會自行結束（停在錯誤提示等使用者）；runtime 在串流中掛掉時 client 拆除可能卡在 VAAPI fence（Frame 上
    SteamVR 當掉的情境，待注入驗證）。
- **M4a 收尾（2026-09-30）**：
  - runtime 串流中死掉：所有 GPU 等待改成有上限——影像 render 後以 timeline semaphore 等 1 s（原本 `pl_gpu_finish`
    無上限）、拆除時以空 batch＋fence 等 2 s（原本 `vkDeviceWaitIdle`）。等不到就標 wedged：當成 session loss 走
    X5（β 重建／退平面；PCVR 結束），拆除時跳過所有會等 GPU 的呼叫、刻意洩漏 VkDevice／libplacebo 物件與仍被
    GPU 參照的解碼幀（`[VIPLE-XR] GPU wedged - leaking …`）。PCVR 在 VR session 後失效：顯示錯誤（GUI 對話框／
    CLI stderr）並以正常退出路徑結束 → 送 `/cancel`。dev 注入 `--xr-test-fail gpuwedge`（15 s 後模擬 GPU 等待
    逾時）；S1 腳本 `--kill-runtime-after SEC`（串流第 SEC 秒 SIGKILL monado-service）。
  - S1 驗證（linux-builder 本機測試 server，β）：串流 25 s 殺 Monado → loss→3 次重建（`xrCreateInstance`
    XR_ERROR_RUNTIME_FAILURE）→ 4 s 內放棄 XR 退平面；無頭環境平面 renderer 起不來 → `Stream error: …` 印到
    stderr、送 `/cancel`、行程自行結束（不再停在對話框）、teardownHang 0。`gpuwedge`：wedged 拆除立即完成、第
    1 次重建 <1 s 成功、串流照常跑完。PCVR（S1 → `.195` service，SteamVR Home）：第 45 s 殺 Monado → 顯示
    「The XR runtime stopped responding …」、1 s 內送 `/cancel`（server 回 200）並結束，`/serverinfo` 回 FREE；
    `gpuwedge` 同樣結束且 wedged 拆除不等待。Monado 的 `xrEnumerateDisplayRefreshRatesFB` 只有 20.0 → 要求 20、
    週期一致（夾值 60 照舊）。
  - CLI `stream`：session 建立後的錯誤（`displayLaunchError`／`stageFailed`）印 `Stream error: …` 到 stderr，
    `sessionFinished` 有錯誤時以 rc 1 結束、不開等人按確定的對話框；GUI 行為不變。
  - CLI `pair`：原本 `Pair succeeded` 後直接 quit，延遲寫入執行緒還沒把 srvcert 寫進設定 → 下次 `stream` 報
    「尚未配對」。改成結束前先 delete ComputerManager（解構會等 flush 寫完）。
  - 更新率（Frame 實測：`xrGetDisplayRefreshRateFB` 回 120、實際 `predictedDisplayPeriod` 13.89 ms＝72 Hz）：PCVR
    bring-up 時列出 `xrEnumerateDisplayRefreshRatesFB`、要求最接近 `--vr-hz`（預設 90）的值；`waitViews` 等週期
    穩定（符合要求 10 幀，或不符合但穩定 45 幀）才回報，回報值與週期不符（>3%）時以量到的週期為準並警告
    （`[VIPLE-XR] refresh mismatch`）。Monado 20 Hz 夾值邏輯不變。
  - 位元率：VR session 沒有明確指定位元率（CLI 沒帶 `--bitrate`、偏好值仍是平面預設）時，用 3456x1728@90＝150 Mbps
    按像素率線性縮放（上限 200 Mbps、下限平面預設），ABR 照常往下調（`[VIPLE-VR-SESSION] bitrate … (VR default …)`）。
    **2026-10-02 改（§VR-BITRATE）**：不再看平面偏好值——Frame 上那個值是 23000（對不上 1080p120 的預設 28000，
    被當成「使用者改過」），VR 又用 23 Mbps。現在只有 CLI `--bitrate` 才覆寫 VR 預設（下限 10 Mbps、上限 200 Mbps）。
  - **Frame → host 實測後的補充（2026-10-02）**：
    - Frame 的 SteamVR 依「每個 app 的更新率設定」切顯示器：沒設定過的 app（我們的 Steam 捷徑）是 72 Hz；
      `xrCreateSession` 當下 `xrEnumerateDisplayRefreshRatesFB` 只回目前值（[120.0]），app 變成 scene app 約 0.1 s 後
      SteamVR 才切到 72（vrcompositor.txt：`Request to change refresh to 72.000000Hz`，面板可用 72／80／90／96／108／
      120／144）。§XR-REFRESH-LATE：週期穩定 20 幀後若不是 `--vr-hz`，重新列舉並再要求一次（最多 2 次），仍不成才用
      量到的週期（穩定門檻由 10／45 幀改 20／60 幀）。**複測結果**：跑起來後列舉也只回目前值（`rates=[72.0]`），
      app 沒有辦法自己換更新率；要在 Frame 的 SteamVR 影像設定替這個 app 指定更新率（vrsettings 的
      `steam.app.<捷徑 id>.preferredRefreshRate`，其他 app 的例子：`vrlink.client`、`steam.app.810500` 都是 120）。
    - server 的 ABR 對 VR 太敏感（§VR-ABR-RATIO）：設定 150 Mbps、實際 13～80 Mbps。丟包其實只有 0.0x%。
    - OpenXR 遊戲上下顛倒（§CHAP-JSONID，`steamvr_driver.md` §9）。
  - **複測後的調整（2026-10-03；使用者回饋：方向正常、清晰度仍不夠、中途一度畫面丟失）**：
    - 更新率 120 Hz：在 Frame 上 `vrcmd --set-settings-float steam.app.<捷徑 id>.preferredRefreshRate 120`
      （讀回確認，設定會留著；`<捷徑 id>` 是 Steam 給非 Steam 遊戲捷徑的 32 位元 app id）。
    - 每眼解析度預設 1728² → **2160²**（Frame 面板每眼原生解析度；`--vr-eye` 可改）。4320×2160@120 的 HEVC level
      是 6.1（server log：`pinned HEVC level_idc=183`）。能力實測（`stream --dump-bitstream` 錄 200 Mbps 樣本、
      Frame 上 `decode-bench --decoder hevc_v4l2m2m`；host＝RTX 5060 Ti）：

      | 打包尺寸 | host 編碼延遲 | Frame 解碼 p50 | 備註 |
      |---|---|---|---|
      | 3456×1728@120 | 5.2 ms | 6.80 ms | |
      | 3840×1920@120 | 6.2 ms | 7.40 ms | |
      | 4320×2160@90 | 8.0 ms | 9.46 ms | |
      | 4320×2160@120 | 7.2 ms | 8.11 ms（p99 9.11） | 吞吐量 120 fps、0 錯誤；解碼時間接近一個幀間隔，餘裕不大 |

      host 的 SteamVR「解析度倍率」是使用者設定（實測當時 80%，app 每眼實際約 1932²），串流尺寸不受它影響。
    - §VR-STALE：使用者看到的「畫面丟失」是 stale 政策——258 ms 沒新幀就切到 loading 環境；10 分鐘內另有 22 次
      超過 100 ms 而變暗。門檻放寬成 500 ms（變暗）／2 s（loading）：最後一幀由 runtime 依頭部轉動重投影，
      短暫凍結比變暗或換場景不突兀。
    - §VR-FEC-LOSS（server）：584 s 內 124 幀救不回，其中 44% 只差不到 10% 的封包、71% 差不到 20%；server 記到的
      67 筆 LOSS 有 66 筆只掉 1 幀。VR 的 FEC 改成跟著 LOSS 調（`log_tags.md` 的 `[VIPLE-FEC] VR …`）：掉 1～2 幀
      +10%（上限 40%）、連掉 3 幀以上當斷線不加、30 s 沒事才每 5 s −5% 降回基準；FEC 多佔的份額從影像位元率上限扣。
      驗證：`<dev-client>` 跑 `--vr-emulate`（4320×2160@120、200 Mbps）＋ `<host>` 上的假 ARP 黑洞（`New-NetNeighbor` 假 MAC，約 300 ms）。
  - **第三輪（2026-10-03 早上，4320×2160@120、200 Mbps）**：
    - 120 Hz 生效（`display refresh rates=[120.0] current=120.0`）；解碼 p50 約 5 ms（比 decode-bench 好）。
    - 連線很差：適配器→頭盔的 rx 速率多在 MCS 3～4、常掉到 0～2（ack −60～−71 dBm），host 對 Frame 熱點 ping 逾時、
      RTT 尖峰到 134 ms；位元率多在 25～50 Mbps（ABR 6 分鐘調 159 次），FEC 開頭 3 s 升到 40%，斷線 45 次
      （3～180 幀）。**Frame 的家用 Wi‑Fi（wlan0）和熱點（wlanap）在同一張網卡（phy0）**，wlan0 在路由器兩個
      BSSID 之間反覆漫遊失敗（認證逾時、status 30），每次都讓熱點斷 3～7 s（host ping 的斷線時間對得上 kernel log）。
    - XR 漏幀從前 4 分鐘的 2～5% 一路升到 8～15%，SteamVR 不時把 app 降到 60／40 Hz（`period=16.67／25.00 ms`）。
    - **09:52:38 kernel Oops，頭盔整個卡死（按鍵全無反應），只能強制重開**：client 行程在 `DRM_IOCTL_PRIME_FD_TO_HANDLE`
      → `msm_gem_import` 失敗的清理路徑 `drm_gem_put_pages` NULL deref——和 G-α 第二輪（上面「途中修掉的問題」1.）
      同一個 SteamOS msm bug。執行緒死在核心、鎖沒放，client 收不到 SIGTERM、`xrRequestExitSession` 後 3 s 等不到
      STOPPING。**原因：XR 影像路徑（XrVideo）沒有用 §SF-DMABUF-CACHE**，仍用 `pl_map_avframe_ex` 每幀匯入
      dmabuf（4320×2160@120、兩平面＝每秒 240 次）；漏幀率逐步上升也可能和這個匯入量有關。
    - 修法：快取搬到 `plvk_common` 的 `DrmTexCache`，PlVk 與 XrVideo 共用（XrVideo 的 render thread 用、`testMap`
      不用）；log：`[VIPLE-XR] dmabuf cache: entries=… imports=… hits=…`。
  - **第四輪（同日 10:45，DrmTexCache 版，Frame 實機驗證通過）**：10 分鐘無核心錯誤，SIGTERM 後正常收尾；dmabuf 快取
    整場只匯入 10 次、命中約 14 萬次；XR 71,515 幀只漏 2 幀（開場切換時），影像 render thread CPU p95 從 4～9 ms 降到
    0.2 ms；MTP p50 47～60、p95 55～64 ms；解碼 6.1～8.3 ms（4320×2160@120 的預算是 8.33 ms，餘裕很小）；位元率時間
    加權約 154 Mbps（ABR 調 46 次）；LOSS 17 次全靠 intra refresh 恢復、0 次 IDR；FEC 升 13 次、降 19 次（單幀 +10%
    的路徑在實機上走到）。開場約 4 s 連線不穩（4 筆各 32 幀的 LOSS＋host ping 逾時），之後只有零星單幀掉包；
    這次 Frame 的家用 Wi‑Fi 沒有漫遊。msm 核心錯誤的回報資料（兩次 Oops、分析、上游修正 `e6863b085606`
    的 6.18 回移 patch）整理在本機 `scripts/benchmark/results/frame-msm-oops-report/`（不入 git）。
  - **使用者回饋（第四輪）**：畫質好很多，但打桌球時整張桌子不斷晃動；另外開場有兩層介面（Frame 自己的平面遊戲介面＋
    串流畫面裡 host 的 SteamVR 主控台）。
  - **§VR-POSEERR／§VR-PREDICT（桌子晃動）**：host 遊戲穩定 120 fps、時鐘抖動 2～4 ms，不是 host 效能。client 新量測
    `[VIPLE-VR-POSEERR]`：遊戲算繪姿態（0x81）對「該幀實際顯示時的頭部姿態」的差。原因是 HMD 的
    `Prop_SecondsFromVsyncToPhotons` 只填一個週期（8.3 ms），SteamVR 只把姿態預測到 vsync＋8.3 ms，但畫面要約
    55 ms 後才在頭盔顯示；頭盔的重投影只修轉動、不修平移，近處物體隨頭部移動晃。第五輪 A/B（同一遊戲 Eleven Table
    Tennis）：

    | | 5a：8.3 ms | 5b：44 ms（`vr_vsync_to_photons_us`） |
    |---|---|---|
    | 算繪姿態落後實際顯示 | 28～37 ms | 0～3 ms |
    | 平移偏差 p95 | 3～17 mm（多在 8～10） | 0.4～3.4 mm |

    改成自動：client 把落後延後 100 ms 再估（看得出超前，帶正負號；只算第一次顯示的幀——同一張影像因卡頓重複顯示時，
    落後是網路／解碼慢了，不是預測不夠遠），每秒放進 CLIENT_TIMING 新欄位（36 B）；server `vr::predict` 經 IPC STATE `SET_V2P`
    讓 driver 直接改屬性（原本的 `DEV_SET_V2P` 只在 selftest dev mode 送得出去，正式環境永遠送不到，已改）。控制分兩段：
    (1) 探測——3 個可靠視窗（≥ 60 樣本；2026-10-10 起改成串流半秒的幀數、夾在 20～60）平均 ≥ 1.5 ms 時往補償方向改 8～10 ms，等 2 s 再取 3 個視窗平均，落後往預期方向移動
    ≥ 步長一半才算 SteamVR 採用；不採用就退回起始值、本 session 停調，下一個 session 從推算值開始。(2) 追蹤——落後的指數移動
    平均（α 0.25）≥ 1 ms 時補一半、每秒 ≤ 3 ms，每個視窗先夾到 ±20 ms；session 內最多偏離起始值 ±40 ms。預設起始值＝一個
    週期＋30 ms（2026-10-10 起是頭盔的顯示週期，半速串流時不再用加倍之後的 host 週期）；設定檔有值則固定。
    5b 另見解碼 8.4～9.4 ms（超過 8.33 ms 預算、溫度正常），以及 MTP p95 從約 60 升到約 130 ms（疑為 echo 樣本配對選到較舊
    樣本的統計假象，待查）。
  - **第六輪 a（10-03 20:29，自動預測第一版＋無頭模式，300 s）**：SteamVR 採用串流中途改的值（探測 +5 ms 後落後 +2.3 →
    −1.6 ms）；整場 10 s 落後中位數都在 ±4 ms 內（5a 的 8.3 ms 是 28～37 ms）、MTP p50 53～60／p95 60～69 ms、XR 漏幀 0、
    解碼 7.6～9 ms。但第一版每 2 s 照單一視窗（雜訊約 ±3.5 ms）補 60%，預測值在 40～50 ms 間來回跳（平均約 45 ms），
    平移誤差 p95 2～6 mm、比 5b 固定 44 ms 的 0.4～3.4 mm 略差；n=33 的卡頓視窗冒出 +16 ms、結束前一秒 +147 ms 都被
    照補。使用者回饋：晃動變好、但有「一點點」呼吸感 → 改成上面的兩段式（模擬：穩定後預測值標準差 2.3 → 1.1 ms、
    ≥ 2 ms 的跳動 66 → 0 次／270 s）＋client 不估重複顯示的幀。
  - **第六輪 b（10-04 11:26，兩段式預測＋程式版無頭）**：探測 3 視窗平均 +2.1 ms → +8 ms，4 s 後 −5.9 ms（移動 8 ms）判定採用、
    進追蹤；打球時 10 s 落後中位數都在 ±2 ms，預測值多在 43～47 ms，每 10 s 擺幅從 5～10 ms 降到約 3 ms；開場按一次 Steam 鍵後
    整場沒有再加回手把模式（`[VIPLE-XR] … (PCVR under gamescope: headless, no flat window)`）。
  - **第六輪 c（10-04 12:05，使用者改連 6 GHz 的家用網路後）**：使用者回報「收藏庫開的平面很順、VR 卡又抖」。兩者都走適配器
    （平面 1920×1080@60 23 Mbps、解碼 2.8 ms；VR 4320×2160@120 140～200 Mbps、解碼 6～8.7 ms）。關鍵是 **Frame 的熱點跟著
    家用網路換頻段**：`softapmanager` 一律把熱點放在家用網路沒用的頻段（家用在 2.4／5 GHz → 熱點 6 GHz ch37 160 MHz；家用在
    6 GHz → 熱點 5 GHz ch36／40 80 MHz，正好和路由器的 5 GHz 重疊），**而且每次家用網路換頻段都會整個重建熱點**（適配器斷線
    重連；10-04 11:22～11:31 被路由器頻段導引觸發 5 次，含第六輪 b 開場兩次）。單純中斷家用網路不會觸發重建（熱點留在原頻段）。
    這輪 12:09 一次斷訊（0.5 s 掉 2110 個封包）讓 ABR 每個視窗砍半到 11 Mbps，約 3 分 40 秒才第一次回到 129 Mbps、4 分鐘才穩定。
  - **§VR-ABR-OUTAGE**：VR 的 ABR 遇到斷訊型視窗（丟包 ≥ 30%，或 client 回報連掉 3 幀以上）記下斷訊前的位元率，照常降碼；
    最後一個斷訊訊號後安靜 1 s、2 個乾淨視窗、client LATCH 幀號有前進，就直接拉回（每次斷訊一次）；拉回後 5 s 內又斷、而且
    這次事件在低位元率時沒斷過才把參考值打 75 折（閃斷不打折）；稀釋過的視窗先砍、斷訊訊號 1 s 內才到時用砍之前的值；
    30 s 沒斷訊結束事件（純函式 `src/vr/vr_abr_outage.h`、單元測試 13 項；3 路對抗式審查後修正：沒東西可拉時不消耗拉回、
    閃斷不打折、要求影像送達證據）。驗證：`--vr-emulate` 下 host 端「閃爍黑洞」（10 個 200 ms 洞、間隔約 80 ms，
    重現 87～98%／>100% 丟包與連掉數百幀的 LOSS）兩次，斷訊結束後 1.7 s 都拉回 179 Mbps（修正前約 3 分 40 秒）。單次長黑洞
    （2.7 s 完全不通）client 走 §FRZ-WATCHDOG 重置＋IDR、ABR 本來就幾乎不動，測不到這條路徑。
  - **Frame 管理經 host 跳板**：家用網路中斷後 `ssh -o HostKeyAlias=<frame-lan-ip> -J <host> steamos@<frame-hotspot-ip>`；
    SSH 工作階段沒有 polkit 的 network-control 權限（`nmcli device disconnect` 回 not authorized），改用
    `systemd-run --user --wait --pipe nmcli …` 就能操作。msm kernel bug 已公開回報 ValveSoftware/SteamOS#2882。
  - **兩層介面**：Frame 端——捷徑 `OpenVR=0` 時 Frame 把它當平面遊戲（手把模式＋平面遊戲介面）；改 `OpenVR=1`（「加入 VR
    收藏庫」）後仍在進 VR 後 42～67 ms 被加回手把模式與雷射滑鼠（`vrclient_vrcompositor.txt` 的 `AddSystemBehaviorFlag
    SystemBehaviorFlag_GamepadMode_*`），原因是 client 在 gamescope 裡開了全螢幕平面視窗（`[VIPLE-SF-ENV] session=x11
    display=:1`）。第六輪 a 用無頭模式（`next.env` 放空的 `DISPLAY=`，Qt／SDL 都改 offscreen，dev 驗證用）：開場那層要按一次
    Steam 鍵（從 SSH 啟動時 Frame 首頁本來就開著），按掉之後整場沒有再被加回來，使用者確認。**§VR-HEADLESS**：PCVR 在 gamescope 裡
    （`GAMESCOPE_WAYLAND_DISPLAY` 有值）`main()` 一開始就清掉 `DISPLAY`／`WAYLAND_DISPLAY`，走同一條無頭路徑，log
    `[VIPLE-XR] … SDL offscreen video driver (PCVR under gamescope: headless, no flat window)`；XR 桌面不套用（實體鍵盤要靠
    視窗焦點），`--vr-emulate` 也不套用（它是平面）。host 端——SteamVR 啟動
    就開主控台：`steam://rungameid/250820`（從收藏庫「執行 SteamVR」）與直接跑 `vrstartup.exe`（§VR-NODASH，SteamVR
    179 ms 起來、不會把 Steam 的 RunningAppID 設成 250820，下一個 session 不再被誤判 VRLINK_ACTIVE）都會開，
    `steamvr.startDashboardFromAppLaunch=false` 無效（待查；10-09 補：這裡的主控台就是後文說的儀表板，無人驗測時可以用
    `dashboard.enableDashboard=false` 暫時停用，見「無人驗測和實際遊玩不一樣的地方」）。
  - 控制器外觀：0x5506 `VIPLE_VR_CONTROLLER_INPUT.profile`（原 reserved 低位元組，三份 VipleVr.h＋IPC ABI 同位移）
    帶 client 的 interaction profile；driver 依此設 `Prop_RenderModelName_String`（Touch＝`oculus_quest2_controller_*`、
    Index＝`{indexcontroller}valve_controller_knu_1_0_*`、Frame＝`{frame_controller}frame_controller_*`；driver 目錄
    不存在時退回 Touch），換外觀時送一筆 `deviceIsConnected=false` 讓 app 重新載入。ControllerType／binding 仍是
    oculus_touch。
  - **§VR-LAUNCHER 模式選單（2026-10-04）**：從 Frame 收藏庫開 VipleStream（GUI）時，先選「桌面模式」或「VR 模式」
    （只在 Linux＋OpenXR 建置、`GAMESCOPE_WAYLAND_DISPLAY` 有值時出現；工具列另有按鈕可以隨時切換）。
    - 選 VR 後，主機的 app 清單改抓 `/applist?vr=1`（會多出 SteamVR Home 這類 vr 類 app），格子只列 `<IsVr>` 的 app
      （其他 app 以 `vr=1` 啟動會被 server 回 400）。切換模式時不清空清單（`hidden`／`directLaunch` 這些 client 端屬性
      要靠舊清單依 id 合併），改由各輪詢執行緒立刻重抓；世代號讓切換前送出、切換後才回來的舊模式結果作廢。
    - 點 app 時 GUI 不開串流頁，而是另開子行程
      `viplestream stream --display-target pcvr [--takeover] -- <主機目前的位址> <app>`，走 CLI 實測過的無頭 PCVR 路徑
      （§VR-HEADLESS）。用位址而不是名稱，因為名稱可能重複；位置參數放在 `--` 之後，app 名稱以 `-` 開頭也不會被當成選項。
      子行程跑的期間 GUI 隱藏，結束後再出現並重新取得焦點。
    - 主機沒有宣告 PCVR、只能走 relay、或離線時，GUI 直接說明原因，不開子行程。子行程本身在無頭狀態下也不再「退回平面串流」
      （看不到），一律以啟動錯誤結束，例如 XR runtime 起不來、server 沒有確認 VR session、直接連線失敗。錯誤原因由 GUI 從
      子行程 stderr 的 `Stream failed:`／`Stream error:` 行取出顯示；結束碼 2 表示主機上有別的 app 在跑。
    - 接管：使用者在 GUI 確認接管別的裝置的 session 後，子行程帶 `--takeover`（CLI 新旗標，不寫入設定）。VR 模式下
      「先結束舊 app」確認後，結束完成會直接開 VR 子行程。
    - 單一實例鎖（Linux 的 `/tmp/viplestream-client.lock`）：子行程是同一支程式，GUI 開它之前先放鎖、它結束後再拿回；鎖檔
      fd 一律 `O_CLOEXEC`。子行程的環境中，`QT_QPA_PLATFORM`、`SDL_VIDEODRIVER` 與 `SDL_VIDEO_DRIVER` 還原成 GUI 改寫之前的值。
      子行程設了 `PR_SET_PDEATHSIG`（SIGTERM），GUI 異常死亡時會正常收尾。GUI 正常結束時先等子行程 8 s，再 SIGTERM，
      最後才 SIGKILL：Frame 的「結束遊戲」會對整個行程群組送 SIGTERM，子行程這時多半正在收尾，送第二個 SIGTERM
      會讓它直接 `_Exit`。
    - 子行程只帶上面這幾個參數，因為 CLI 覆寫會在 session 開始時被寫回偏好設定（§K.15），VR 用的值不能污染桌面模式。
      PCVR 不用 MP-QUIC 這件事改在 `session.cpp` 執行期跳過，偏好設定不變（log `[VIPLE-VR-SESSION] PCVR: MP-QUIC off …`）。
    - Flatpak 沙盒有 PID 隔離時（`/proc/1/comm` 是 `bwrap`），VR 選項停用並說明原因，啟動時也不跳出選單：OpenXR 連
      Frame 的 SteamVR 會出錯，2026-09-30 曾讓 SteamVR 重啟。dev 環境的 Steam 捷徑 wrapper 改用不開 PID namespace 的
      bwrap 包裝（`FLATPAK_BWRAP`）啟動 GUI。**正式打包怎麼處理 PID 隔離還沒決定。**
    - 審查（2026-10-04，2 路）：單一實例鎖讓子行程一定以「already running」結束（必修）、切換模式清空清單會洗掉
      `hidden`／`directLaunch`（必修），以及無頭退回平面、非 PCVR 主機、接管、GUI 結束時 SIGKILL 子行程、焦點等應修項，
      都已照上面的設計修正。
    - log：`[VIPLE-VR-LAUNCHER]`。
    - **Frame 實測（2026-10-04 晚）**：GUI 的 VR 模式能在熱點上找到 host，並且只列出 VR app。冷啟動（含重啟 SteamVR）約
      2 s 進入 ACTIVE。用遊戲內選單退出後，§VR-EXIT 的監看器自動收掉串流並回到 GUI。Steam 沒登入時顯示原因並回到 GUI，
      不會停在 loading。
    - **設定頁用控制器操作（§UI-SCROLL，2026-10-11）**：使用者回報在頭盔上用雷射指標沒辦法把設定頁往下捲、看不到下面的選項。
      Frame 把控制器同時當成雷射滑鼠和一支手把交給介面（Steam 對這個捷徑載入的是通用的手把範本
      `controller_generic_gamepad_joystick`）。原本設定頁完全不理右搖桿，左搖桿與方向鍵只會把焦點一項一項往下移
      （頁面跟著焦點捲）；VR 那一區排在左欄最底下，前面有幾十個控制項。搖桿為什麼沒有變成滑鼠滾輪沒有直接查證
      （gamescope 有把 VR 指標的捲動轉成滾輪的選項 `--vr-scrolls-speed`），推測是搖桿已經被當成手把的搖桿。改動：
      - 右搖桿上下直接捲頁（死區 12000；速度跟推的幅度的平方成正比，推到底每秒兩個可見高度），LB／RB 一次捲 0.8 個
        可見高度，鍵盤的 PageUp／PageDown 也一樣。
      - 焦點：有焦點的控制項被捲出畫面時，焦點先收到這一頁本身。留在原地的話使用者看不到焦點在哪——這時按 A 或左右會
        改到畫面外的設定，按上下則是焦點一動、頁面捲回原來的位置。捲動停下 250 ms 之後，或使用者先按了上下，焦點再放到
        看得到的控制項上：原本那一個又看得到就還給它，否則沿 Tab 順序往捲動（或按鍵）的方向找第一個完整看得到的，
        文字輸入欄跳過。用鍵盤捲頁時，焦點在文字輸入欄就不收（使用者可能正在打字）。
      - Frame 上捲軸一直顯示、寬 18 px，可以用雷射抓著拖；VR 模式下設定頁一打開就捲到 VR 那一區，焦點落在
        「每眼解析度」。其他平台這兩項都不變。
      - 設定頁裡還在用一般 `ComboBox` 的 8 個下拉選單（VR 區 4 個、渲染器、Linux 的呈現方式、補幀的後端與品質）改用
        `AutoResizingComboBox`。實測一般的 `ComboBox` 用手把打開之後，按上下送出的是 Tab：選單直接關掉、焦點跳到下一個
        控制項，選不了項目——VR 區的四個都是這樣（用雷射點選不受影響）。VR 區的寬度原本也裝不下「1152 x 1152」。
      - 左搖桿觸發導覽的門檻由 30000 降到 24000（滿量程的 73%）。
      - 捲動由設定頁自己做（C++ 只發 `scrollRequested`），沒有用合成的滑鼠滾輪事件。
      - log：`[VIPLE-UI-NAV]`。改動前的介面行程不印有沒有看到手把，這次從 log 分不出使用者那邊手把輸入有沒有進來。
      - 驗證：Windows 建置用 ViGEm 的虛擬 Xbox 360 手把實際操作並截圖——右搖桿捲到底再捲回頂、捲完焦點跟進再按下繼續往下、
        LB／RB、左搖桿推 80%、渲染器與每眼解析度兩個下拉選單裡上下移動再用 B 取消、鍵盤 PageUp／PageDown；操作前後
        偏好設定逐項比對沒有變動。只在 Frame 成立的那個分支（開頁就在 VR 區、捲軸常駐）另外用一個把條件暫時強制成真的
        建置看過。**頭盔上還沒有驗**：介面行程在頭盔上有沒有拿到手把的輸入、雷射能不能拖捲軸，要等使用者試。
      - 審查（2026-10-11，三個角度＋逐項查核）後修的事：
        - 同時有兩支手把時每次輪詢只取推得最深的那一支（逐支各算會互相吃掉）。
        - 右搖桿的死區由 8000 改成 12000：XInput 建議的右搖桿死區是 8689，8000 落在靜止值可能出現的範圍裡，接著一支
          有點飄移的手把就會一直送捲動要求（滑鼠滾輪被蓋掉、方向鍵走不出畫面）。不到 1 px 的捲動量也改成先累加。
        - 設定頁上開著對話框時不捲背後的頁面（判斷方式：焦點往上走不經過 `stackView`／`toolBar` 就是在 popup 裡；
          這些檔案匯入的是 `QtQuick.Controls 2.2`，沒有 `Overlay` 可用）。
        - 焦點的處理改成上面寫的「捲出畫面就先收到頁面本身」。前兩版是「焦點留在原地、250 ms 後才搬」，再加一段
          「還沒搬之前就按方向鍵時提早搬」的規則，審查陸續找到它的洞：捲完馬上按 A 會切換畫面外的勾選框、在焦點改變的
          處理裡同步搬焦點會被還沒跑完的 `forceActiveFocus` 拉回去、用雷射點選被切到一半的控制項時焦點被搬走、
          相鄰的是文字輸入欄時按鍵沒有反應。收到頁面本身之後這幾種情況都不存在：按鍵由頁面自己接
          （`Keys.onTabPressed`／`onBacktabPressed`；手把的方向鍵上是帶 Shift 的 Tab），點選則直接結束「收著」的狀態。
        - 下拉選單開著時不接 PageUp／PageDown。
        - 逐項查核時對「收到頁面本身」這一版又找到四個邊界，也補了：正被按著的控制項不收（雷射按下到放開之間剛好來
          一次捲動，那一下點擊會沒有作用）；放回去時從原本那一個往畫面所在的方向找（照按鍵或捲動的方向找，反方向會繞完
          整圈 Tab 順序、落在另一欄）；看不看得到用捲動的終點判斷，按鍵接手時先讓捲動動畫到終點（LB／RB 和方向鍵落在同
          一次輪詢時，用動畫途中的位置挑到的控制項接著就被捲出去）；已經在頂或在底時也檢查要不要收（先前用滾輪或捲軸
          捲走、焦點留在畫面外的情況）。
        每一批修正後都在 Windows 上用虛擬手把重驗；最後一版驗了捲兩頁後等焦點自己回來、捲走馬上按 A（控制項與偏好設定
        都沒變）、接著按方向鍵、捲走再捲回來焦點還給原本那一個、選單開著按 PageDown、到底後 LB／RB 再按方向鍵上。
        審查者另外把這一頁的函式原樣搬進獨立的小頁面，用 `qmltestrunner`（offscreen）送真的按鍵與滑鼠事件驗過各種時序。
    - **平台事實：Frame 的桌面模式也架在 SteamVR 上（2026-10-11 唯讀查到）**：桌面（Plasma）與遊戲兩種工作階段都由
      `start-gamescope-session` 啟動；`plasma-session.target` 的說明是「Gamescope VR Session」，`Requires=gamescope-session.service`、
      `Wants=steamvr.service`／`steamvr-plasma.service`。平面視窗一律由 `gamescope --backend openvr` 畫成 SteamVR 的 overlay
      （巢狀 1280×720）。所以「切到桌面模式來避開頭盔的 SteamVR」這條路不存在；頭盔的螢幕、鏡片校正、追蹤與重投影都在那一層。
  - **§FRAME-RADIO：Frame 單一無線電造成的丟包（2026-10-04 晚）**：同一天晚上的 VR 卡頓、破圖、間歇停格，逐輪對照如下
    （Eleven Table Tennis，4320×2160@120）：

    | 條件 | 實際路徑 | 斷訊/分 | ABR 下修 | 位元率中位數 |
    |---|---|---|---|---|
    | 熱點 5 GHz ch40（與家用路由器同頻道）、wlan0 斷線 | 適配器 | 約 7 | — | 斷斷續續 |
    | wlan0 連家用 5 GHz，GUI 選了 LAN 位址 | 家用 Wi-Fi | 11～17 | 74～171 | 44～51 Mbps |
    | wlan0 連家用 5 GHz，熱點 6 GHz ch37 160 MHz | 適配器 | 9.1 | 71 | 46 Mbps |
    | **wlan0 不受管理**，熱點 6 GHz | 適配器 | **0** | **3** | **171 Mbps** |

    - 根因：Frame 的 ath12k 只有一顆無線電，同時服務 wlan0（家用 Wi-Fi）與 wlanap（熱點）時只能分時。負載下送往適配器的
      封包有 1～4% 到不了頭盔，閒置時則不丟。外部有同樣的逐封包量測：ValveSoftware/SteamVR-for-Linux#946、#965。
      Frame 的 `RcvbufErrors` 是 0，所以不是接收端來不及收。
    - Steam 自己的串流在同樣條件下很順，因為它是「多連線」（`driver_vrlink.allowMultipleLinks`）：家用 Wi-Fi 與適配器
      同時有流量，一條路分時造成的空檔由另一條補上。我們的 VR 影像是 `vr_force_rtp` 單一路徑。
    - wlan0 只「中斷」不夠：斷線但仍受管理時，NetworkManager 會背景掃描，一樣要佔用無線電。要
      `nmcli device set wlan0 managed no`（SSH 下經 `systemd-run --user --wait --pipe`；重開機會還原）。
    - GUI 選路：Frame 的設定檔只存 host 的 LAN 位址。兩條路都通時，GUI 走 LAN（也就是家用 Wi-Fi）；家用網路斷掉時才會
      經 mDNS 在熱點上找到 10.35.78.x。選路要依實測的丟包／RTT 決定，不寫死「適配器優先」。
    - 排除項：host 的 Steam 每 12～15 s 重新初始化適配器（`remote_connections.txt` 的 `OnRemoteClientUSBTriggerHotplug`
      → `RTK_Initialize`），16 次斷訊只有 4 次落在它附近，跟巧合沒有差別；NVFBC 只影響 Steam 遠端暢玩的平面擷取。
    - 長期解法：VR 影像也走 MP-QUIC 雙路徑（適配器＋家用 Wi-Fi），見 TODO。（後來改成每張網卡一條 UDP 連線，見 8.7。）
    - **外部回報與檢查（2026-10-05 凌晨，網路與 GitHub 搜尋）**：
      - 同型丟包：SteamVR-for-Linux#946（負載下 1～4% 到不了頭盔的無線電、閒置時不丟；離開家用 Wi-Fi 後中斷少約 75%）、
        #965（遺失發生在 PC 送出到 Frame 無線電之間；同一台 PC 的 Windows 對照約 0 掉包）。
      - SteamOS#2831：Frame 的 ath12k 韌體崩潰並自動復原後，會默默降級到重開頭盔為止。我們的 Frame 在 10-05 00:04:46
        重新連回家用網路時，也記到一次 `failed to transmit frame -108` → `Uploading coredump` → `pdev 0 successfully recovered`。
        **重要測試前先重開頭盔。**
      - Frame 的 Wi-Fi 設定有「保留 Wi-Fi 6E 給適配器」，可以讓熱點一律待在 6 GHz；Remote Play 連線管理可改成 Custom、
        自選 6 GHz 頻道。
      - 適配器的擺放：直接插主機板的 USB 3 孔、不經 hub，用短的 USB 3 延長線拉離機殼與排風、朝向遊玩區。USB 2 延長線的
        症狀就像「高位元率時丟包」。適配器會很燙；`RT_EVENT_RF_THER_RANGE` 依 Realtek 命名推測是溫度事件。host 檢查：
        我們的適配器目前經過一個「Generic SuperSpeed USB Hub」，並非直接插在主機板上。
      - host 每 12～15 s 一次的 `OnRemoteClientUSBTriggerHotplug`→`RTK_Initialize` 迴圈沒有人公開回報過；
        `RF_THER_RANGE`、`RTK_VRFeatureConfig` 查無資料。
      - Frame 穩定版 SteamOS 0.3.0（20260922）；0.4.1 beta 修了「其他網路介面干擾適配器連線」，0.4.3 beta 繼續改善串流
        （PC 端要 SteamVR 2.18.2 以上）。
    - **鏈路乾淨後剩下的「稍微卡」**（wlan0 不受管理那一輪，使用者回報「有改善，但比不上 Steam 串流」）：
      - host 端掉幀：前 50 s 每 10 s 有 94～274 次 `fenceTimeout`，擷取只剩 922～1054／1200 幀。遊戲吃滿 GPU 時，driver
        的合成排在遊戲後面，超過原本固定的 2 ms 上限就被丟掉。**§VR-FENCE-WAIT**：上限改成週期的 70%，夾在 2～6 ms
        （120 Hz 約 5 ms）。丟一幀要多等一整個週期，多等幾 ms 一定比較好。（10-10 補：不必吃滿 GPU，遊戲貼圖直通時
        正常負載的等待就有約 2.3 ms，見「host 擷取端的 fence 晚到」。）
      - 頭盔解碼太貼邊：4320×2160@120 每幀解碼 7.5～8.5 ms（偶爾 21 ms），預算只有 8.33 ms，慢一點就只能重複上一張。
        MTP p95 75～90 ms（10/3 那輪是 55～64 ms），每 10 s 有新畫面的顯示幀 899～1193／1200。待做 A/B：90 Hz（預算
        11.1 ms）或每眼 1920²（解碼約 6 ms）；長期做注視點編碼（Frame 有眼動追蹤，Steam 的串流很可能有用）。
      - **第六～八輪（2026-10-04 23:36～10-05 00:00，wlan0 不受管理、適配器 6 GHz）**：

        | 輪 | 條件 | fenceTimeout | 新畫面比例 | 解碼 p50 | MTP p50／p95 | 預測落後 p50 | 位置誤差 p95 | 使用者回饋 |
        |---|---|---|---|---|---|---|---|---|
        | 6 | 120 Hz，§VR-FENCE-WAIT | 7（第五輪約 750） | 94.3% | 8.7 ms | 57／78 ms | 5.9 ms | 5.0 mm | 再好一點，但每一兩秒抖一下 |
        | 7 | 90 Hz（Frame 捷徑 `preferredRefreshRate=90`） | 0 | 98.6% | 9.6 ms | 71／91 ms | 16.2 ms | 9.0 mm | 沒有改善，還是抖 |
        | 8 | 90 Hz，host 設定 `vr_vsync_to_photons_us = 55000` | — | 98.0% | — | 約 64～75 ms | −0.3 ms | 7.0 mm | 轉頭時撕裂、殘影、抖動，打不到球 |

        - 90 Hz 解碼有餘裕（重複幀大幅減少），但「抖」沒有消失，所以剩下的問題不在解碼。
        - §VR-PREDICT 兩輪都印出「SteamVR did not apply the mid-session change」，但只有第六輪真的探測過：
          - 第六輪的探測移動 3.4 ms（門檻是步長 8 ms 的一半），探測視窗剛好落在遊戲載入、host 還在掉拍的時段
            （driver missed＝183、173，之後才變成 0）。
          - 判定一次「沒採用」之後，整個 server 行程都把自動預測當成不可用（行程層級的狀態），第七輪根本沒有探測：
            那行「16.3 -> 16.3」是 learn 的結果，借用了同一個訊息格式。（10-05 更正：先前寫的「第七輪可能剛好在載入時
            探測」不正確。）
          - 學到的值也沒分更新率：120 Hz 學到 38.7 ms，拿去給 90 Hz 用，實際需要約 55 ms，所以第七輪預測落後 16 ms。
        - 鎖相鋸齒：`[VIPLE-VR-LATCH]` 的 slackEma 在 1～8 ms 之間擺盪，ppm 大部分時間頂在 +200（上限），每 30～40 s
          突然跳高 4～5 ms，再慢慢拉回。預測時間固定、實際顯示時間卻在跳，位置誤差就忽大忽小。
        - 轉頭時的撕裂與殘影，對應 MTP 偏高（65～75 ms，重投影只修旋轉、不修位移與近物視差）以及 overscan＝0
          （快速轉頭會露邊）。
        - 調查與修正計畫見 TODO（鎖相、overscan、控制器姿態預測、延遲拆解、預測依更新率換算）。
        - 測試後的狀態：Frame 捷徑仍是 90 Hz；host 設定 `vr_vsync_to_photons_us = 55000` 還在（配合 90 Hz）；Frame 的
          wlan0 已恢復受管理並連回家用網路。
    - **10-05 夜間的修正（當時頭盔還沒驗；10-07 以無人場次在頭盔上跑過 A1、A2，結果與還沒驗的部分見 8.7。除了 §VR-PREDICT 的修正不需開關、預設就生效，其餘新行為都在開關後面，預設維持舊行為）**：
      - 根因與對應開關：

        | 問題 | 證據 | 修正（設定） |
        |---|---|---|
        | 每 35～60 s 整格滑移、鎖住時約每秒一張重複幀 | LATCH 的 slack 是取模一個週期的相位（晚到 δ 量成 T−δ），server 當線性量處理、輸出飽和時照樣積分；第八輪 ppm 全在 92～200 | §VR-LATCH-V2（`vr_latch_mode = v2`、`vr_latch_target_pct = 40`） |
        | 拍子超前、上行抖動直接變成拍子的時間誤差 | 控制器的 poseTimeOffset 一直是 0（只有 HMD 有設），拍子被 SteamVR 再外插一次，推算超前約 25～31 ms | §VR-CTRL-OFFSET（`vr_ctrl_pose_offset = enabled`） |
        | 拍面角度（與傾斜的頭）繞錯軸外插 | SteamVR 把 driver 給的角速度當**機體座標**（`vr_probe --mode predict` 實測），client／OpenXR 給的是世界座標；傾 30° 時外插軸偏 30° | §VR-ANGVEL-LOCAL（`vr_angvel_local = enabled`） |
        | 頭與拍子約每秒往回跳一次 | 超過 2T 沒新樣本就把速度與 offset 歸零；每輪約 150 次，原因是上行延遲尖峰而不是掉包 | §VR-STALE-HOLD（`vr_stale_policy = hold`） |
        | 轉頭時邊緣露出沒畫面的區域（假設） | overscan＝0；90 Hz 每 10 s 的旋轉誤差 p95 中位數 2.6～2.7° | dev `--vr-overscan <deg>`（client 旗標） |
        | §VR-PREDICT 探測一次失敗就整個行程放棄、學到的值不分更新率 | 見上面第六～八輪 | 暖機、穩定度閘門、不採用只停該 session、依更新率換算（不需開關；固定值時不跑） |

      - 不戴頭盔做過的驗證：
        - LATCH v2：單元測試 22 項（含 90／120 Hz、±60 ppm 時鐘漂移、25 組初始相位的閉迴路）全過；同一個模型下 legacy
          在抖動 ±1.5 ms 以上或漂移 +60 ppm 時大量滑移，v2 全部 0 滑移、0 重複幀。Python 尖峰抖動模型（2% 幀晚 3～8 ms）：
          120 Hz legacy 25 組有 7 組進極限環、重複幀每秒 4.9 → v2 0 組、每秒 2.1；90 Hz 每秒 1.67 → 1.23（剩下的是尖峰本身，
          相位控制修不掉）；代價是平均多等約 1.6 ms（目標從 T/4 改 0.4T）。
        - driver：vr_probe `--mode unit` 149/0（新增 pose-policy 21 項：揮拍 1 m/s 的空窗模擬，舊規則在 44 ms 空窗裡位置往回跳
          36 mm，hold 在 100 ms 內的空窗誤差 < 1 mm）。
        - S0（`<dev-client>` `--vr-emulate --vr-synthetic-motion sine`，90 Hz）：legacy 場 `poseFlags=0x0`、控制器 offset 0；
          全開場 `poseFlags=0x3`、控制器 offset p50＝HMD p50（33.2 ms）。以 `NtSuspendProcess` 暫停 client 製造上行空窗：
          50／80 ms 都以 hold 接回（`tracking recovered … hold=1`），150 ms 超過上限才回到舊規則（`holdExpired`）；server 的
          `[VIPLE-VR-UPLINK]` 與 driver 端的到達間隔一致，session 不斷。LATCH 在 emulate 下不會送（驗不到）。
        - SteamVR 外插語意（新的 `vr_probe --mode predict`＋client `--vr-synthetic-motion tilt30yaw`，SteamVR 2.17.10）：
          - 角速度：外插增量的旋轉軸對世界 Y 的 |dot| 在舊版是 0.866（cos 30°）、對機體 Y 是 1.000＝SteamVR 當機體座標用；
            開 `vr_angvel_local` 後世界 1.000（HMD 與右手都是）。這是拍面角度外插錯誤的直接證據，修正已做（預設關）。
          - poseTimeOffset：legacy 時 HMD 與右手（姿態相同）在 pred=0 差 +33.3 ms＝SteamVR 依文件正號採用 HMD 的 offset、
            控制器沒有 offset；開 `vr_ctrl_pose_offset` 後 0.0 ms。
          - 外插量：約 100 ms 內線性（gain 1.00），之後封頂在距樣本時間約 97～100 ms。
        - S2（`<dev-client>` 本機 SteamVR null driver 當 XR runtime，真的跑 XR 路徑）：每眼 1440² 時 MTP p50 34～36 ms、
          CLIENT_TIMING decode p50／p95 0.41／0.51 ms、錯配每 10 s 1～4 次（約 0.1～0.3%）、XR 漏幀 0；每眼 2160² 時
          `<dev-client>` 解不動（decode 頂到 65535 µs、佇列越積越多），那場的數字不能用。null driver 的 frame loop 實際約
          138 Hz（宣告 90 Hz），latch 與 host 的 90 Hz 沒有固定相位，LATCH（舊版或 v2）在 S2 本來就鎖不住。
      - 當時排定的頭盔 A/B（一次只開一個，每場 4～5 分鐘；先重開 Frame、wlan0 不受管理、熱點 6 GHz、捷徑 90 Hz）：
        A0 新 build 全部 legacy → A1 `vr_latch_mode = v2` → A2 再加 `vr_ctrl_pose_offset = enabled`＋`vr_angvel_local = enabled`
        （兩個都是探測證實的預測正確性修正）→ A3 再加 `vr_stale_policy = hold` → A4 client `--vr-overscan 0` 對 `3`。
        host 端切換：`powershell -ExecutionPolicy Bypass -File C:\ProgramData\VipleStream\vr_ab.ps1 -Preset A0|A1|A2|A3`
        （先備份 log 與 conf、只改這幾個鍵、重啟 service）。看的欄位：LATCH `slips`／`ppm`／`err`、MTP10
        `repeat`／`skip`／`mismatch`、POSEERR `edge`、driver `ctrlOffUs`／`hold`、`[VIPLE-VR-UPLINK]`。
    - **10-06 晚：Steam 多連線怎麼做（實測）與我們的對照**（頭盔照常連著家用 Wi-Fi 5 GHz、熱點 6 GHz 160 MHz；
      Eleven Table Tennis；host 兩張網卡每 100 ms 的計數器＋頭盔 `/sys/class/net` 計數器＋pktmon）：
      - 使用者明講不接受「VR 時要中斷家用 Wi-Fi」——Steam 做得到，我們也要做到。
      - **Steam（vrlink）＝兩條路各送一份完整串流**：

        | | host 送出 | 頭盔收到 | 接近空白的 0.1 s 區間 |
        |---|---|---|---|
        | 乙太網路 → 路由器 → 頭盔家用 Wi-Fi | 平均 216 Mbps（穩定，完整串流） | 平均 207 Mbps | 2.4% |
        | 適配器 → 頭盔熱點 | 平均 133 Mbps（25～234，塞得進多少送多少） | 平均 125 Mbps | 11.2% |
        | 兩條同時 | | | **1.0%** |

        開場兩張網卡的流量完全相同；適配器上抓到的封包有 52% 在乙太網路那邊找得到內容相同的（乙太網路只抓到約 45%，
        換算幾乎全部重複）。封包是 Valve 自己的 UDP 格式：每張網卡一個綁定的 socket、兩邊都是同一個埠、每個封包約 1 KB、
        所有連線共用 16-bit 序號（接收端去重）；上行（頭盔→host）也是兩條都送。vrlink log：`Uber link enabled`、
        `Found primary link`＝適配器那個位址、`nTimedRetryOption = 1`（有時限的重傳）、`HandleFECChange(32)`、120 Hz、
        編碼寬 1152（注視點編碼）、4.3 分鐘內 14 次 stream reset（encoder reset，使用者無感）。
      - **同一晚的 VipleStream（舊行為、只走適配器）**：位元率從 140 Mbps 被掉包壓到 20～30 Mbps；4 分鐘 LOSS 431 次、
        intra refresh 173 次；上行追蹤空檔每 10 s 有 0～61 次超過 2T、最長 352 ms（`[VIPLE-VR-UPLINK]`：同時段控制
        執行緒單輪 ≤ 0.16 ms，空檔來自 Wi-Fi）；LATCH `slips` 4 分鐘 127 次（約每 2 s 一次）、ppm 整場頂在 +200；
        熱點接收有 5 次 200～800 ms 的整段空白。使用者：「非常糟糕，破圖嚴重，轉頭時撕裂感超嚴重」。
      - 判讀：單一無線電輪流服務兩個頻道，任何一條路單獨用都有空檔；兩條都送同一份，空檔只剩兩條同時不通的那 1%，
        再用重傳與 FEC 補。在這個環境家用 Wi-Fi 那條其實比適配器穩，我們之前只走適配器是挑到比較差的那條。
        **聚合（1+1=2）在這裡不存在**：兩條路共用一顆無線電，Steam 也是拿第二條換穩定而不是換頻寬。
      - 量測陷阱：pktmon 對 Wi-Fi 網卡給的是原生 802.11 資料幀（pcapng 標成 Ethernet 是錯的：24／26 B 標頭＋8 B
        LLC/SNAP 才是 IP）；乙太網路 component 每個封包記 4 次、高流量時漏一半以上——流量比例要看網卡計數器。
      - 頭盔的 USB：內建 USB 網路 gadget（`ncm.usb0`，已配一組 /29 位址，沒插線時 down）＋ADB。插 USB-C 線應該會在
        PC 上多一張網卡，是不經無線電的第三條路（當時還沒實測；之後的結果見 8.7「結論」第 4 點：目前不能用）。
      - **下一步（傳輸）**：VR 的影像、音訊、追蹤都兩條路同送、接收端以序號去重；之後補有時限的重傳。（已做，見 8.7。）

### 8.7 連線層實測與多連線（§VR-MULTILINK，2026-10-06 夜）

**為什麼做**：使用者不接受「VR 時中斷家用 Wi-Fi」。同晚量到 Steam 的做法是每張網卡一條綁定的 UDP 連線、
兩條各送一份完整串流、接收端以序號去重（見 8.6 的 10-06 量測）。動手照做之前，先用連線層的合成流量量
「單走一條」與「兩條都送」的差別。

**量測工具**（本機 `scripts/vr/`，不入 git）：

| 檔案 | 位置 | 做什麼 |
|---|---|---|
| `vlpt_host.ps1` | `<host>` | 每條連線一個綁定位址的 socket＋一條送出執行緒，以幀節拍送 1040 B 封包（帶幀號、序號、送出時間）；同時記錄頭盔回送的模擬追蹤。落後超過 1.5 幀的幀直接跳過 |
| `vlpt_frame.py` | 頭盔 | 每條連線跑一份：記錄每個封包的到達時間；子行程用同一個 socket 以 180 Hz 回送 262 B 的模擬追蹤。純 UDP，不碰 SteamVR／OpenXR |
| `vlpt_analyze.py` | `<dev-client>` | 扣掉兩機時鐘差與漂移（每秒最小延遲的直線擬合）後算：封包送達率與遲到時間、到達空檔、幀層（任意 K／N 封包到齊，模擬 FEC；規則與 `RtpVideoQueue` 相同——看到下一幀的封包就放棄上一幀）、上行空檔 |
| `vlpt_run.sh` | `<dev-client>` | 串起一輪並把兩端的紀錄拉回 `temp/steamlink/poc/` |

**量測條件**：頭盔放在路由器旁邊，不是遊玩位置。家用 Wi-Fi（5 GHz、80 MHz）訊號約 −33 dBm、PHY 1441 Mbps；
適配器那條（6 GHz、160 MHz）在頭盔端量到的 ack 訊號只有 −69～−81 dBm，PHY 在 17～432 Mbps 之間跳。
**下表的適配器數字只代表「弱鏈路」的情況**，遊玩位置要另外量。

| 測法（200 Mbps、90 Hz、30 s） | 家用 Wi-Fi 那條 | 適配器那條 | 兩條合併（先到先用） |
|---|---|---|---|
| 只送家用 | 送達 99.83%；遲到 p99 6.0 ms；FEC 10% 下 2700 幀壞 14 幀 | 不送 | — |
| 只送家用、每幀分散 5 ms 送出 | 送達 99.96%；FEC 10% 下壞 4 幀 | 不送 | — |
| 兩條都送 | 送達 **91.5%**；16 次 > 100 ms 的到達空檔；FEC 10% 下壞 295 幀（最長連續 14 幀） | 只送出 5.5%（送出端一次最長阻塞 3.1 s）；送到的晚 0.5～4 s | 與家用那條幾乎相同 |
| 兩條都送、20 Mbps | 送達 99.93% | 0.6 s 後送出端一次阻塞 **10 s**；送到的晚 6～12 s | 與家用那條相同 |

上行（模擬追蹤，兩條各自 180 Hz）：只在家用那條送影像時，兩條的上行都乾淨（最大空檔 18 ms／40 ms）；兩條都送影像時，
家用那條的上行出現 37 次 > 50 ms 的空檔，適配器那條的上行延遲中位數 233 ms、最大空檔 0.9 s。

**第二個位置（10-07 00:20，頭盔被移到離 `<host>` 較近的地方）**：家用 Wi-Fi −58 dBm；適配器那條的 ack 訊號 −61 dBm、
PHY 1.7 Gbps。同樣 200 Mbps、90 Hz、30 s：

| 測法 | 家用 Wi-Fi 那條 | 適配器那條 | 兩條合併（先到先用） |
|---|---|---|---|
| 只送家用 | 送達 **64%**；141 次 > 50 ms 的到達空檔（最長 174 ms）；FEC 10% 下準時（一個幀週期內）到齊的幀 **42%** | 不送（它的上行很乾淨：最大空檔 18 ms） | — |
| 只送適配器 | 不送 | 送達 99.98%，但每秒約 1 次 50～100 ms 的停頓（26 次 > 50 ms）；準時到齊的幀 **92%**、100 ms 內 98.9% | — |
| 兩條都送 | 送達 70.9%；103 次 > 50 ms 空檔 | 送出 94%（送出端跳過 6% 過期的幀）、送達 99.6%；45 次 > 50 ms 空檔 | 送達 **99.89%**；**沒有任何 > 20 ms 的空檔**（最長 15.6 ms）；準時到齊的幀 **98.2%**；現行佇列規則下 2700 幀壞 33 幀 |

上行（模擬追蹤）：單一條各有 54～107 次 > 50 ms 的空檔；兩條合併最長 42 ms、沒有 > 50 ms 的。

這個位置的結論與第一個位置相反：**兩條的空檔互補，合併遠勝任何單一條**（Steam 多連線的原理）。單走適配器時
「每秒約一次 50～100 ms 的停頓」正是先前「每一兩秒抖一下」的來源；單走家用 Wi-Fi 在這裡不能用。
兩個位置合起來看：哪條好、要不要兩條都送，取決於當下每條鏈路的速率，不能寫死——這就是 `vr_multilink = auto` 要解決的。

**結論（改變了做法）**

1. **兩條連線不是互相獨立的。** 它們共用頭盔的無線電。往一條送不動的鏈路硬送影像，會把無線電時間耗在那條路上，
   原本乾淨的另一條也被拖垮（送達率 99.8% → 91.5%）。「每條都送一份完整串流」只有在兩條的鏈路速率都夠高時才划算
   （Steam 當晚在遊玩位置：家用 207 Mbps、適配器 125 Mbps）。所以多連線必須能判斷「這條送不動」並停止在它上面送影像。
2. **Windows 對送不動的 Wi-Fi 網卡做阻塞式 UDP 送出，一次可以卡 10 秒。** 現行單一路徑的影像執行緒就是這樣送的：
   session 走適配器那條、鏈路一變差，整條影像管線跟著停。每條連線必須有自己的佇列與送出執行緒、非阻塞送出、過期就丟。
3. **整幀一口氣送出會造成成串掉包。** 家用那條單走時掉包只有 0.17%，但集中在少數幀（每約 2 秒壞一幀，與先前
   「每一兩秒抖一下」的主觀描述相符）；把一幀分散成 5 ms 送出，壞幀少約 3 倍。各只量一輪，待重複確認。
   server 現行的節拍是固定約 800 Mbps（`ratecontrol_packets_in_1ms`），200 Mbps 的一幀約 2.8 ms 送完，等於沒有分散。
4. **USB 網路目前不能用。** 頭盔內建 NCM gadget（`usb0`，有自己的 DHCP），接上 `<host>` 後 Windows 以內建的
   「UsbNcm Host Device」驅動自動取得位址（USB 2.0、連線速率 426 Mbps），ping 正常；但量測的 UDP 流量（5 Mbps 也一樣）
   數秒內就讓整條鏈路卡死——兩端的介面計數器不再前進，之後 ping 也不通。`pnputil /restart-device` 重啟複合裝置第一次
   可以恢復，第二次留下「Unknown USB Device (Port Reset Failed)」，只能實體重插。觸發條件還沒定位（先別對它灌流量）。

**§VR-MULTILINK 實作（2026-10-06，預設關閉）**

- 開關：server `vr_multilink = disabled|auto|all|primary`（`Sunshine/docs/configuration.md`）。client 以 `vrCaps` 的
  `VIPLE_VR_CLIENT_CAP_MULTILINK` 宣告支援，server 在 `<VipleStreamVRSession>` 回 `multilink=1` 才啟用；任何一邊不支援，
  行為與之前完全相同。線上格式見 `docs/vr_protocol.md` §4.10。
- client（common-c `VrMultiLink.c`）：session 本身那一組位址是主連線；其餘每張網卡只配「同子網路」的 server 位址
  （清單來自 `/serverinfo` 的介面通告）。每條連線一組專用的影像／音訊 UDP socket（Linux 另加 `SO_BINDTODEVICE`）。
  影像與音訊的接收執行緒同時等原本的 socket 與所有連線的 socket，餵進同一個佇列（重複的由既有的 RTP 序號檢查丟掉；
  多連線時關掉「依目前缺包數提早判定掉幀」的推測）。追蹤（0x5506）以 AES-GCM 加密後在每條已確認的連線各送一份；
  沒有已確認的連線時照舊走 ENet。
- server（`src/vr/vr_multilink.{h,cpp}`、純邏輯在 `vr_multilink_logic.h`）：每條連線一組綁在對應位址的 socket、
  自己的影像佇列與送出執行緒（非阻塞；批次超過兩個幀週期還沒送完就丟——起播的 IDR 一個週期送不完）；全部連線共用一條接收執行緒
  （PING／PONG、追蹤解密與去重）。**飽和偵測**：1 s 視窗內至少 24 個批次、其中 40% 以上送不完，就暫停在這條連線送影像
  （2 s 起、連續發生加倍到 30 s；最後一條可用的連線不暫停），連線本身保持（追蹤、音訊、ping 照送）。
  只要有一條連線可用，原本的單一路徑就暫停；全部不可用時自動退回。
- **`auto`（依量測決定哪條送影像）**：session 本身那條先送影像，其餘連線先「探測」——每秒只送一批影像（約 64 個封包）。
  client 量這一批到達的速率並在 PING 回報；連續 2 次量到 ≥ 影像位元率的 1.5 倍（至少 50 Mbps），server 才開始在那條
  連線送影像。送不動的連線退回探測，而不是時間到就盲目重試（上表「兩條都送」的情況：每重試一次，另一條就被拖下去一次）。
  `all` 則是一開始每條都送、送不動的暫停一段時間後直接再試。
- 存活判定是雙向的：client 收得到 PONG 才在 PING 帶 CONFIRMED，server 最近 2 s 內收過 CONFIRMED 才把這條當可用。
- log：兩端各有 `[VIPLE-VR-LINK] 10s:`（`docs/log_tags.md`）。

**S0 驗證**（`<dev-client>` 只有一張網卡，用 dev 選項 `--vr-link-selftest` 在同一張網卡開兩條連線；
`scripts/vr/s0_ml.sh` 一輪）：

| 輪次 | 條件 | 結果 |
|---|---|---|
| run1 | `vr_multilink = all` | 兩條都確認（RTT 0.9 ms）；啟動 200 ms 後影像改走連線（原本的 socket 只收到 10 個封包）；兩條各送 179 Mbps、client 每條每 10 s 收約 16.2 萬個封包、採用比例約 6：4；追蹤每條 180/s、server 端另一條那一份全部被去重；60 s 內 LOSS 0、wave 0；結束時 client rc=0、server 正常收尾 |
| run2／run3 | 另加 `vr_multilink_fault = 1:60:40,2:40:60:40`（兩條互補地各斷一段，任何時刻至少一條通） | server 端每一批都確實在其中一條送出（L1 送出數＝L2 被丟數），client 也全部收到，但 33 s 內仍有 LOSS 8、wave 3。原因在 client：兩個 socket 各自先進先出，「先讀哪一個」會把跨幀的順序打亂——連線 A 還留著第 N 幀的尾巴、連線 B 已有第 N+1 幀的開頭時，先讀到 B，佇列就放棄第 N 幀 |
| run4 | 同上，client 改成「把已經到的封包全部收進暫存、依幀號排序再交給佇列」（不等待、不加延遲） | 起播交接時掉 1 次（故障注入從第一刻就在丟），之後 25 s LOSS 0 |
| run5 | `vr_multilink_fault = 1:70:30,2:70:30`（兩條同時斷 30 ms） | LOSS 442、wave 221——確認注入器真的在丟、run4 的乾淨是靠互補與去重 |
| run6 | `vr_multilink = all`、不帶 selftest（只有一條連線） | 一條連線 178 Mbps、LOSS 0、追蹤全走連線；單一連線也能用（送出不阻塞影像執行緒） |
| run7 | `vr_multilink = disabled` | client 沒有任何 `[VIPLE-VR-LINK]`、server 沒有收到 LINK_HELLO、LOSS 0——舊路徑不受影響 |
| auto1 | `vr_multilink = auto`（第二條連線先探測） | 第二條送了 2 批探測，client 量到 614 Mbps（需求 243），約 5 s 後開始送影像；LOSS 0 |
| auto2 | `auto`＋`vr_multilink_fault = r2:20`（第二條限速 20 Mbps） | 第二條每批探測只送得出約 20 個封包、量不到足夠的速率，整場停在探測；第一條 179 Mbps 不受影響，LOSS 0 |
| auto3 | `auto`＋`r1:20`（session 本身那條限速 20 Mbps） | 起播約 2 s 內畫面在掉（主連線送不動）；第二條量到 978 Mbps 後接手，0.2 s 後主連線退回探測（量到 19 Mbps），之後 LOSS 0 |

**首次頭盔實測（2026-10-07 01:17，遊玩位置、`vr_multilink = auto`、Eleven Table Tennis）**

- 兩條連線 0.5 s 內都確認；session 位址在適配器那條（L1），家用 Wi-Fi（L2）2 s 後量到 250 Mbps 開始送影像。
- 前約 2 分鐘：兩條各 170～180 Mbps；12 個 10 s 視窗裡 10 個 LOSS 0；追蹤到達的最大間隔 10～37 ms、超過 2T 每 10 s 0～1 次
  （同晚單一路徑：最長 352 ms、每 10 s 0～61 次）；位元率維持 150～180 Mbps（單一路徑掉到 20～30）。
  先到的幾乎都是適配器那一份，家用那條被採用的只有 2～5%（補洞用）。使用者：「進步很多，大部分都順，偶爾抖一下」。
- 01:19:21 起頭盔的 wlan0 在家用路由器同一個網路名稱下的不同無線電之間來回漫遊（路由器回 status 30 拒絕，約每 45 s 一次）；
  第三次漫遊時頭盔的 ath12k 驅動卡死（`failed wait for peer deleted`，之後所有 WMI 指令 -11），wlan0 與熱點全斷，只能重開機。
  同一次開機先前 4.5 小時沒有任何漫遊，觸發原因未定。處置：依使用者要求把頭盔的家用連線設定檔鎖定在 5 GHz
  （`802-11-wireless.band a`）。
- 這一場暴露的規則問題（都已修、S0 回歸通過）：
  - 漫遊那幾秒家用那條斷線、適配器連續卡超過 0.4 s，被判成「送不動」而暫停，只剩一條不通的。現在暫停一條連線之前，
    另一條必須「最近 0.7 s 內還有 PING」而且有實際送達率的證據（≥ 80%，或比被暫停那條這一秒實際送得出去的比例高 30 個百分點以上）。
  - 送達率＝頭盔在 PING 回報的累計收到數 ÷ server 的累計送出數，每秒結算，印在 10 s 統計行的 `deliv=`。
  - 「正在送影像」的判定改用 0.7 s 的 PING 新鮮度：唯一在送的那條一沒聲音就立刻在所有可用連線上送（保底），不白等 2 s。
  - auto：退回探測後，如果沒有任何一條送達率夠好的連線在送，量到夠快就立刻回來，不等最短探測時間；
    最短探測時間 2 s 起、連續發生加倍、封頂 8 s。探測成績以「之後又送了幾批探測」計算時效，不能拿舊量測放行。
  - 位元率很低（每幀不到 32 個封包）時等不到可量的批次：等 2 s 後直接開始送（審查發現）。
  - 連線的 socket 補上 QoS 標記；primary 模式下主連線被暫停時待命連線會接手。

**程式審查（兩輪背景 workflow，唯讀）**：第一輪五個視角共 30 項、去重後查證 16 項，14 項成立（1 高、5 中、8 低）；修正後第二輪三個視角
再找到 8 項（2 高），都與「證據過期」和「暫停後不重新評估」有關，已一併修掉。

**第三輪：頭盔端修正與跨幀等待（2026-10-07）**

- 頭盔端（要重建 Flatpak）：
  - USB 網路 gadget（`usb*`／`rndis*`／`ncm*`）不拿來當連線（一有 UDP 流量就卡死，見上面第 4 點）。
  - 到達速率改用核心的接收時戳（`SO_TIMESTAMPNS`＋`recvmsg`）：在 user space 讀取的時刻會把已經排在 socket 裡的封包擠成
    同一瞬間，量到的是讀取速度。一次量測＝同一幀在這條連線從第一個封包到最後一個花了多久（至少 32 個封包）；不以封包間的
    空檔分段——鏈路被擠到一小撮一小撮送時，分段會把每一小撮都量成「很快」。整幀在 300 µs 內到（一個聚合框就送完）
    回報 65535＝「快到量不出來」，server 當成合格的量測；不回報的話 server 等不到新量測，連線會永遠停在探測。
  - 「已確認」的判定修掉無號數相減下溢，逾時從 2 s 縮到 1 s。
  - 影像加密時，暫存排序的次要鍵改用 IV 計數器（server 每送一個封包加一），同一幀內也排得出順序。
- server 端：
  - 一次探測改成 6 ms 內的所有批次（約一整幀）：只送一批（64 KB）在快的鏈路上是一個聚合框，頭盔端量不出速率。
  - 探測中的批次不進飽和偵測，連線從探測轉為送影像時重新開始統計（否則整幀的探測會湊滿視窗，把還在探測的連線再判一次
    「送不動」、退避加倍；或把探測期算成健康期而提早把退避歸零）。
- **跨幀等待（§VR-MULTILINK-HOLD，common-c `RtpVideoQueue.c`）**：原本佇列只要看到下一幀（或同一幀的下一個 FEC block）
  的封包就放棄還沒收齊的這一個。兩條連線的延遲不一樣時，領先那條剛好掉了一段、落後那條稍後才送到同一段，就全被當成過期——
  兩條合起來收得齊，這一幀還是掉了。現在多連線時，超前的封包先照到達順序排著（最多 2048 個），目前這個 block 收齊就依序放行；
  最前面那個等超過一個幀週期（4～14 ms）才照原本的方式放棄。多連線時取代 §K.17 的寬限期
  （只處理差一個 shard、最多 4 個封包）；單一路徑的行為不變。統計印在 `[VIPLE-VR-LINK] hold 10s:`。
  - 佇列等的對象一換（上一個 block 收齊或被放棄）就把整個環掃一遍，把屬於新對象的封包照到達順序先交出去——
    下一幀有兩個 FEC block 時，領先那條的 block 1 會排在落後那條送來的 block 0 前面，只看最前面那個會把 block 0 等到放棄。
  - 沒有封包到的時候也檢查期限：接收的 poll 逾時縮短到最前面那個的剩餘期限（VR 沒有新畫面時 server 100 ms 才送一幀）。
  - 等不到（到期或排滿）時放行的是環裡「幀號、block 最小」那個對象最早到的封包，不是最前面那一格：最前面的可能是
    領先那條送來的更後面的幀，直接放行它會把中間那一幀（落後那條已經送到、排在後面）整個跳過。
  - 只有在「至少兩條連線實際在送影像」時才等（每 200 ms 結算，收到的影像封包數達到最多那條的四分之一才算；探測中的連線
    每秒只來一幀，不算；曾經有兩條之後要連續 5 個視窗都不足才算只剩一條——一條連線停頓 150～200 ms 是常態）。
    單網卡、`primary`、另一條還在探測時沒有第二份可等，等只會讓掉幀回報晚一個幀週期。
  - 排過隊的封包放行後被採用才計入那條連線的「採用數」（`used=`）。
  - 兩條連線走同一條實體路徑（selftest 對真的很差的鏈路）時救不回來，只會多等：那是測試條件的特性。
- S0（`vr_multilink_fault` 新增 `d<id>:<ms>`：這條連線的每一批晚這麼久才送）：

| 輪次 | 條件 | 結果（30 s） |
|---|---|---|
| base | `all`＋`1:100:25,d2:8`（連線 1 每 125 ms 斷 25 ms、連線 2 落後 8 ms），舊 client | LOSS 378、wave 189，位元率被壓到 14～17 Mbps |
| hold | 同上，新 client | LOSS 4、wave 2，位元率約 135～178 Mbps；每 10 s 約 80 次等待、99% 救回（81/82、76/77、78/79），最長等 10.7 ms（上限 11.1 ms） |
| auto | `auto`、不注入 | LOSS 0、沒有觸發任何等待；第二條 2 次探測後量到 573 Mbps 開始送 |
| auto-slow | `auto`＋`r1:60` | 第二條量到後接手、主連線退回探測並量到 60 Mbps（限速 60，量測不再高估）；掉幀只在接手前那 1 s |
| linux | linux-builder（Wi-Fi）的 x86_64 dev Flatpak 當 client、`--vr-emulate --vr-link-selftest`、`auto` | 兩條都確認、核心時戳量到 57～170 Mbps（那台的 Wi-Fi 本來就只有這個速度；同一條鏈路以前用讀取時刻會量到上千）、第二條在位元率降下來後被放行；Linux 才有的 `recvmsg`／`SO_TIMESTAMPNS`／`SO_BINDTODEVICE` 路徑都走過 |

- 第三、四、五輪審查（各一輪多個視角＋逐項反證）：第三輪成立 4 項（跨幀等待、量不出速率時卡在探測、探測批次進了飽和偵測、
  速率分段只看讀取時刻）；第四輪針對等待佇列與上一輪的修法成立 9 項（多 FEC block 的幀等不到、期限只在有封包時檢查、
  單一連線也在等、採用數灌水、送達率後備會放行慢鏈路等），其中「量不出速率」原本在 server 用送達率當後備，
  改成由頭盔明確回報後整段拿掉；第五輪複查重寫後的等待佇列，沒有記憶體或狀態損壞，成立 5 項低嚴重度的邊角
  （到期時的放行順序、排滿時要重新判斷、「幾條在送」缺遲滯等）。都已修。

**頭盔家用 Wi-Fi 的頻段（2026-10-07 03:25 發現）**

- 01:32 鎖 5 GHz 的那個設定檔在 1 分鐘後就不是現用的了：使用者在頭盔介面重新加入了家用網路，產生新的設定檔
  （Steam 給它的 `band` 是 `no6`），舊的鎖定不會跟過去。之後整晚 wlan0 在路由器的 2.4 GHz 與 5 GHz 之間來回
  （每 10 分鐘 2～5 次連線嘗試、路由器回 status 30），每 10～25 分鐘整條重新連線一次；從 2.4 GHz 換回 5 GHz 時
  softapmanager 會重啟 hostapd，熱點（`<host>` 適配器那條）跟著斷約 7 s。
- 03:25 把現用的設定檔也設成 `802-11-wireless.band a`：wlan0 回到 5200 MHz、熱點 6 GHz ch37。03:26:48 路由器主動把頭盔
  踢下 5 GHz（reason 47）並拒絕了約 30 s（status 1），頭盔堅持 5 GHz、03:27:21 連回；之後觀察 9 分鐘沒有任何重連。
  路由器的頻段導引會想把它推到訊號比較強的 2.4 GHz（2.4 GHz −43 dBm、5 GHz −61 dBm），**治本要在路由器上對這台關掉
  頻段導引或綁定 5 GHz**。
- 查法：`journalctl -b -u NetworkManager | grep 'audit: op="connection-'`（誰在什麼時候加／改／啟用哪個設定檔）、
  `journalctl -b | grep "wlan0: Trying to associate"`（依頻率統計）、`iw dev wlan0 link`。測試前後都要看。

**第二次頭盔實測（2026-10-07 05:54，新 client、`auto`、5 GHz 鎖定依使用者要求拿掉；約 3 分鐘）**

- 兩條連線 0.5 s 內確認，家用那條 2 s 後量到 339 Mbps 開始送（核心時戳的量測值 124～660 Mbps）。整場 wlan0 沒有漫遊。
- 使用者在約 85 s 時刻意走到適配器收不太到的位置：之前兩條各約 180 Mbps（適配器送達 98～100%、家用 68～100%，
  掉幀事件 2 次）；之後適配器那條送不動被退回探測（探測只量到 20～198 Mbps、門檻約 225～285，沒再回來），
  家用那條單獨送 145～190 Mbps、送達 97～100%（另一條不送之後它反而變好）。換線前後掉幀事件 9 次，沒有斷線。
- 跨幀等待整場只觸發 1 次：領先那條幾乎都自己收得齊。這一場的抖動主要不是掉幀。
- 使用者的感受：移動前「震動抖動比較嚴重」，移動後「跳格、幀數變少」。
- 移動前的抖動對得上 LATCH（舊控制）的鋸齒：slack 每 60 s 被積分項推過目標、滑掉一整格（05:55:16、05:56:16、05:57:16，
  `ppm` 頂在 200、`integ` 頂在 150），slack 小於約 2.5 ms 的 10 s 視窗有 19～148 張重複幀，3.8～6.6 ms 的視窗只有 0～29 張。
- host 端 `fallback`（沒對到算圖姿態的幀）全場 18%；頭盔靜止的場次只有 0.08%（見下），與頭部運動有關，留給 A2。

**LATCH v2 的頭盔 A/B（同日 07:16／07:21，無人配戴、頭盔靜止、從 SSH 啟動同一個遊戲各 4 分鐘；兩場都零掉幀、兩條各約 180 Mbps）**

| | 舊控制（`legacy`） | 新控制（`vr_latch_mode = v2`） |
|---|---|---|
| 開頭 30 s 的重複幀 | 343 | 82 |
| 之後 3.5 分鐘的重複幀 | 341（其中 01:13～01:43 一段 321 張） | 1 |
| 滑格次數（`slips`） | 49，`ppm` 前 1 分鐘頂在 +200 | 15（全在開頭 20 s），之後不再增加 |
| 鎖定後的 slack | 2.5～3.3 ms（目標 2.8 ms），鎖定前曾掉到 0.6～1.6 ms | 4.3～4.9 ms（目標 4.4 ms） |

- 靜止、無人的條件下舊控制最後也鎖得住；有人在玩、到達時間抖動比較大時它每 60 s 一輪（見上一場）。v2 在 30 s 內鎖定後沒有再滑。
- 無人配戴時 XR session 停在 VISIBLE、MTP 的數字（p50 約 90 ms）不能和配戴時（63～72 ms）比，只比兩場之間。
- host 已留在 `vr_latch_mode = v2`（`vr_ab.ps1 -Preset A1`）。下一步 A2（控制器 offset＋角速度座標）需要使用者配戴。
- 從 SSH 啟動的做法：寫 `~/viplestream-galpha/next.{args,duration,flatpak,host,app,env}` 後
  `steam steam://rungameid/<捷徑 id>`；跑之前備份、跑完還原頭盔的 `VipleStream.conf`（CLI 參數會被存）。

**控制訊息走連線、補包（§VR-LINK-CTRL／§VR-LINK-REPAIR，2026-10-07 上午；預設關閉）**

線上格式與規則在 `docs/vr_protocol.md` §4.10 最後兩段，開關是 server 的 `vr_multilink_ctrl`、`vr_multilink_repair`
（`Sunshine/docs/configuration.md`）。這裡記為什麼做與量到什麼。

- **為什麼做**：第二次頭盔實測裡，使用者走到適配器收訊差的位置之後「跳格、幀數變少」。log 對得上兩件事：
  (1) 影像已經改由家用那條送，LOSS 與 REFRESH_START 卻還只走 session 本身那條（適配器）——第 10803 幀的 LOSS 等了
  832 ms 才退回 IDR，第 11628 幀的 LOSS 沒送到；(2) 只剩一條在送時，掉的比 parity 多的 block 只能整幀放棄。
  前者把時間敏感的控制訊息改成每條連線各送一份；後者由 server 重送原封包。
- **分工**：頭盔只回報「正在等哪個 block、手上有哪些 shard」；補哪些、走哪條、補不補、要不要告訴頭盔別等了，
  都在 PC 這端決定——它留著原封包，也看得到每條鏈路的送出狀況、往返時間與位元率。頭盔端不做任何估計以外的運算。
- **S0**（`<dev-client>` 模擬頭盔；每輪 30 s、90 Hz、約 180 Mbps；注入規則見 `vr_multilink_fault`）

| 情境 | 兩個開關都關 | 都開 |
|---|---|---|
| 兩條同時斷 60 ms（每 960 ms 一次）＋ENet 上的 VR 訊息全丟（`1:900:60,2:900:60,c:0:1000`） | 33 次掉幀全部等到逾時退回 IDR | 34 次全部 2～4 ms 內收到 REFRESH_START、約 100 ms 恢復，0 次 IDR。這種長度補包幫不上（超過兩個幀週期），server 回 GONE、頭盔立刻放棄不空等 |
| 單一連線，每 500 ms 連掉 45 個封包（`b1:500:45`，比 parity 多） | 掉幀 112 次、位元率被壓到 17 Mbps | 穩定後 0 次掉幀、181 Mbps；每 10 s 回報約 21 次、全數補回，一次補約 30 個封包 |
| 單一連線，每 500 ms 斷 12 ms（`1:488:12`，整幀沒到） | 掉幀 54 次、16.7 Mbps | 0 次掉幀、181 Mbps；每次斷訊回報一次、整幀補回（約 150～170 個封包） |
| 兩條都在送，只有一條掉（`b1:500:45`） | 另一條的那一份補上，0 次掉幀 | 相同，而且完全沒有回報（不該補的不補） |
| 兩條都在送、同一段都掉（`b1:500:45,b2:500:45`） | 掉幀 | 兩條都送過這個 block 就立刻回報（block 開始後 2～4 ms），全數補回、181 Mbps |
| 一條每 100 ms 斷 25 ms、另一條落後 8 ms（`1:100:25,d2:8`） | 跨幀等待救回，0 次掉幀、181 Mbps | 第一版規則太早回報：補包全是多餘的，又被記成丟包，位元率掉到 47～62 Mbps。改成「每條連線都走過這個 block 才報」之後 0 次回報、181 Mbps |
| 壓力：每 20 ms 連掉 45 個（`b1:20:45`） | 位元率降到下限、每秒數次掉幀 | 補包額度用完後回 GONE，退回原本的 FEC＋refresh＋降碼，行為與關閉時相近、沒有惡化 |
| 乾淨的 `auto`、舊版 client | — | 沒有任何回報；舊版 client 協商結果是兩個功能都關，行為不變 |

- **做的過程中發現的幾件事**
  - **service 行程的睡眠精度是 15.6 ms**：Windows 11 上沒有可見視窗的行程拿不到調高的系統計時解析度，
    `std::this_thread::sleep_for(0.3 ms)` 實際睡到下一個 tick（要求 4 ms，量到 7～15.6 ms）。hub 的送出執行緒在
    would_block 之後「稍等再試」因此變成一次等十幾毫秒，一批的期限（兩個幀週期）內只試得了一兩次——鏈路只是
    短暫塞住也會被當成送不動。改用高解析度 waitable timer（`platf::create_high_precision_timer`）。這也修正了
    先前「落後 8 ms」那幾輪 S0 的延遲注入（當時實際延遲是 8～16 ms）。
  - **補得回來的不該算丟包**：一開始把補的量記進 ABR 的丟包，「每 500 ms 斷 12 ms」這種和位元率無關的空檔
    就一路把位元率砍到下限（零掉幀、畫質卻剩 17 Mbps）。改成不算丟包、量大（影像封包的 3% 以上）時只擋回升；
    補不回來的照舊走 LOSS 與降碼，補包額度（15%）是上限。
  - **鏈路整個斷一小段時，回報次數會在空檔裡用完**：三次回報都落在 12 ms 的空檔裡，恢復後反而沒得報。
    改成鏈路恢復（一個幀週期以上沒有任何封包之後又有封包）時退還空檔裡的那幾次、馬上再報。
  - **往返時間太長的鏈路不該回報**：linux-builder 的 Wi-Fi（RTT 18～39 ms）上每 10 s 回報約 30 次，補包到的時候
    那一幀早就被放棄了，只是在已經很擠的鏈路上加碼。RTT＋2 ms 超過一個半幀週期就不回報（一開始訂一個幀週期，
    頭盔上適配器那條的 RTT 偶爾到 10 ms，另一條剛好卡住時補包被關掉、掉了本來救得回來的幀，才放寬）。
  - **「靜默多久」在真實鏈路上量不準**：退還空檔裡的回報次數，一開始只看「一個幀週期以上沒有任何封包」。頭盔上
    12 ms 的空檔量到的靜默常常不滿一個幀週期（空檔前送出的封包晚幾毫秒才到），約 1.5% 的空檔沒退還、三次回報
    全用完。加一條不看時間的判斷：回報送出後一路沒有封包、而第一個到的是後面的封包（不是補包）。
    （那台筆電另有硬體解碼器跟不上 4320×2160@90 的問題，掉幀事件是解碼錯誤造成的，和連線層無關。）
  - **補包的年齡上限要看頭盔等到什麼時候，不是看送出多久**：12 ms 的空檔剛好吞掉連續兩整幀時，頭盔要等第三幀的
    封包到了才知道缺，第一幀這時已超過兩個幀週期。上限放寬到三個幀週期；頭盔端另外在「等待期間有進展」時把
    期限從那一刻起再延一個幀週期（從後面的封包到達起算不超過兩個）。S0 用 `1:489:12`（週期不是幀週期的整數倍，
    空檔會掃過每一個相位）45 s：0 次掉幀。
- **第六、七輪審查**（各一輪多個視角＋逐項反證）：第六輪成立 12 項（補包規劃多送了用不到的封包又重複記帳、
  對象剛換時送出「什麼都沒收到」的回報、控制資料報讓等待重新起算、GONE 排在同一批補包前面、primary 模式下
  待命的連線也收到補包、排定中的 wave 回覆多算一幀等）；第七輪針對這些修法與雙連線的回報時機成立 13 項
  （等的應該是「還沒送到的那幾條」比已經送到的晚多久、時間差只看幀頭所以速率低的連線要另外判斷、單一離群
  樣本會把估計值推高、幀號取自未驗證的標頭所以要防跳號、GONE 與慢連線上的補包的先後、整個 block 沒到時回報
  太晚等）。第八輪只看最後幾個改動，成立 4 項低嚴重度：補包額度的突發量要補得起連續兩整幀（400→600；72／80 Hz
  或高位元率時 400 只夠第一幀）、合成運動 `fast` 的角速度改成姿態真正的導數、放棄 log 的「第一個封包多久以前到」
  改用到達時刻、以及「期限減一個重報間隔」那個上限的說明——審查用實際程式碼驗過，把它改成「等落後那條該到的時刻」
  反而會多掉幀，所以時機不動、只把取捨寫清楚。都已修；單元測試 `test_vr_multilink.cpp` 42 則、`test_vr_params.cpp`
  20 則通過。
- **頭盔無人驗證**（2026-10-07 12:30～12:50，頭盔靜止、插電、沒有人戴；從 SSH 啟動 Eleven Table Tennis，
  `scripts/vr/hs_run.sh`；資料在本機 `temp/launchertest/ml/hs4/`，不入 git）

| 場次 | 條件 | 結果 |
|---|---|---|
| H0 | 舊版 client（沒有這兩個功能）× 新 server，`auto`，3 分鐘 | 協商結果兩個功能都關；0 次掉幀、兩條各 186 Mbps——新 server 對舊 client 沒有退步 |
| H1 | 新 client，`auto`、兩個功能都開，4 分鐘 | 協商成功；0 次掉幀、兩條各 186 Mbps、送達 98～99%；開場 30 s 之後重複幀 0；LATCH 回授 9 成由家用那條先送到；兩條連線實際只差 0.6～1.4 ms（`lagMs`）；鏈路乾淨，沒有任何回報 |
| H2 | 同上＋兩條各自每 489 ms 連掉 45 個（相位錯開） | 幾乎都被另一條的那一份蓋掉；剛好重疊的 1 次以「每條都走過」回報、補回 |
| H3 | `primary`（只走適配器那條）＋每 489 ms 連掉 45 個，100 s | 回報後 block 開始 2.5～5.4 ms 內補包送出，正常時全數補回。中間有一段適配器**真的**卡住約 90 ms（server 端 `dropAge 312／dropBlock 604`、`sendMaxMs 22.7`）：補包被正確拒絕（block 已 38～88 ms），3 ms 內轉成 REFRESH——只走一條的弱點，也是多連線存在的理由 |
| H4 | `primary`＋每 501 ms 斷 12 ms（掃過所有相位），100 s、200 次斷訊 | **0 次掉幀、186 Mbps**；每次斷訊只多一張重複幀；等待最長 7～9 ms |
| F3／F4 | `all`（兩條都送）＋兩條同時每 489 ms 連掉 45 個，80～100 s | 以「每條都走過」回報（block 開始後 4.7～7 ms）、全數補回；F3 另有 2 幀是另一條剛好卡住、往返時間門檻把補包關掉而掉的，門檻放寬後的 F4 只剩開場那一次 |
| F6 | 最終版，`primary`＋每 501 ms 斷 12 ms，120 s、約 240 次斷訊 | 掉 1 幀（那一次鏈路實際斷得比注入的久，回報 4 次仍沒補到）；等待最長 5.5～5.7 ms（退還回報次數的判斷改掉之後） |
| F7 | 最終版，`auto`、不注入，120 s | 0 次掉幀、兩條各 183 Mbps；期間鏈路自己掉了一次超過 parity 的 block，兩條各補 7 個封包補回——第一次在真實掉包上看到補包生效 |

- 結論：兩個功能在真頭盔（arm64、核心時戳、實際的往返時間 1.4～10 ms）上行為和 S0 一致。host 留在
  `vr_multilink = auto`、`vr_multilink_ctrl = enabled`、`vr_multilink_repair = enabled`，等使用者實際配戴驗證後再決定預設值。

**合成頭部運動與「對不到算圖姿態」（2026-10-07 下午）**

- 起因：無人配戴時頭盔是靜止的，host 端「沒對到算圖姿態」（`[VIPLE-VR-SESSION] fallback`）只有 0.08%，使用者實際在玩時是
  18%——只有頭在動才出現的現象，無人測試驗不到。
- 新增 dev 選項 `--vr-synthetic-hmd`：真的 XR 連線上，頭的姿態改送 `--vr-synthetic-motion` 的合成運動（取樣時機、
  `predictNs`、控制器仍來自 XR runtime）；另加運動 `fast`（左右 ±30° @1.2 Hz、峰值約 226°/s，接近實際遊玩的量級；
  原本的 `sine` 只有約 31°/s）。畫面會在頭盔裡甩動，不可以戴著用；不寫入設定。
- 結果（每場 25～100 s）：

| 條件 | `sine`（慢） | `fast`（快） |
|---|---|---|
| S0 模擬、host A1（只開 LATCH v2） | fallback 0.7% | **77%** |
| S0 模擬、host A2（另開 `vr_angvel_local`＋`vr_ctrl_pose_offset`） | 0.6% | **0.6%** |
| 頭盔實機（`--vr-synthetic-hmd`）、A1 | — | **78%**（1781 幀對到、6487 幀沒對到） |
| 頭盔實機、A2 | — | **0.3%**（8231／25） |

- 解讀：client 送的角速度是世界座標，SteamVR 的 `DriverPose_t` 要的是機體座標（`docs/steamvr_driver.md`
  的外插語意實測）。頭水平、轉得慢時兩者幾乎相同；頭有俯仰又轉得快時，SteamVR 外插出來的姿態和 driver 自己照同一份樣本
  外插的差超過 1°，`pose_history` 就對不到，那一幀只能用後備的姿態重投影。`vr_angvel_local` 把角速度轉成機體座標
  之後兩邊一致。這很可能是「轉頭時畫面抖」的主因之一，而且和連線層無關。
- host 目前留在 A2（`vr_ab.ps1 -Preset A2`）。`vr_ctrl_pose_offset`（控制器的時間偏移）還沒有用真的控制器驗過；
  手部若有異樣，`-Preset A1` 可以退回。

**第三次頭盔實測（2026-10-07 18:24～18:33，使用者配戴、家用 Wi-Fi 連著、`auto`＋ctrl＋repair、A2；90 Hz、每眼 2160）**

- 使用者回饋：比上次好；走到適配器收訊差的位置時頓了兩次比較嚴重的，約 5～10 秒恢復。頭不動時都很好，
  頭一晃動就破圖、抖動、有浮動感。
- 兩條連線一開始都在送影像（家用那條量到 548 Mbps）。全程 45454 幀，掉幀事件 27 次、0 次退回 IDR，
  `lossToRefresh` 2～9 ms，恢復約 100～300 ms。
- **host 端對得到算圖姿態**：`fallback` 46／45454（0.1%），A2 在真人配戴下成立。追蹤上行 180 筆/s、沒掉。
- **頭動時的問題在預測距離**：MTP p50 64～68 ms。`[VIPLE-VR-POSEERR]` 頭在動的幀，算圖姿態對實際顯示姿態的
  旋轉誤差 p50 約 1.2°、p95 3～6°，落後量 p05 −20 ms、p95 ＋15 ms（中位數超前 2～8 ms：host 固定在 55 ms、
  略多）。用當下的角速度往前推 60 多毫秒，頭一加速就推錯；頭盔的重投影修得回轉動、修不回平移（浮動），
  而 overscan 是 0，修正後邊緣沒有畫面（`edge` 幾乎 100%，破圖）。治本是壓低延遲，治標是 overscan。
- **兩次卡頓（18:29:00～18:29:25）**：適配器那條被判送不動而暫停；同時家用那條瞬間掉 40% 以上
  （位元率被砍到 11 Mbps，§VR-ABR-OUTAGE 4 秒內拉回 92 Mbps）。路由器 log 這段沒有任何連線事件、也沒有
  mesh 節點——不是漫遊；家用那條為什麼掉還沒有量測可以解釋（猜測：共用無線電被差的那條拖累）。
  第二次是探測放行太樂觀：27 秒後量到 151 Mbps 就放行（門檻只有 50 Mbps，因為位元率剛被砍到 11），
  一送影像就掉 46%，4 秒後再被暫停。
- **修正（同日晚，server）**：恢復門檻改用近 30 秒的位元率峰值算；放行前探測期間送出的封包要有 85% 以上
  送到（到達速率只說明到的那些來得多快）；放行後 10 秒內又被判送不動，下一次要連續 4 次、再下一次 8 次
  合格量測才放行（撐過 10 秒歸零）。單元測試 `test_vr_multilink.cpp` 46 則。
- **為下一輪做的設定**：頭盔捷徑的 `preferredRefreshRate` 90 → 120（使用者的目標是 120 Hz）；host 拿掉
  固定的 `vr_vsync_to_photons_us`（回到自動）；`--vr-overscan` 改成會保存的設定（`vroverscandeg`），
  從 GUI 啟動的 VR 也吃得到，頭盔上設 3°。

**同日晚的後續幾輪與 Steam 內建串流的對照（20:37～22:48，使用者配戴、同一個遊戲、同樣的走位：原本位置 →
適配器收訊差的位置；使用者沒有戴眼鏡，清晰度的比較不算數）**

| 輪 | 條件 | 原本位置 | 適配器差的位置 | 使用者回饋 |
|---|---|---|---|---|
| 4 | 120 Hz、overscan 3°、每眼 2160、`auto`＋修正後的探測放行 | MTP p50 55～59 ms；頭動時旋轉誤差 p50 0.3°／p95 約 1°（上一輪 1.2°／3～6°）、落後量約 ±10 ms、`edge` 0～0.3%；重複幀每 10 s 0～9 | 適配器那條 20:42:47 被暫停後沒有再放行（量到 40～280 Mbps、要 210～270）；只剩家用那條，成串掉包 2～8%，補回來的幀晚到 → 重複幀每 10 s 20～45 | 進步很多，抖和浮減輕但沒消失；差的位置只卡不到兩秒，但之後抖和浮比原位置嚴重 |
| Steam | Steam 內建串流，每眼算圖 2160、120 Hz | 兩條各約 200 Mbps（全複製） | 家用約 206、適配器約 181 Mbps（平均） | 原位置非常流暢、幾乎不抖不浮；差的位置有一點點掉幀和抖、不浮、不破圖 |
| 5 | 同第 4 輪但 `vr_multilink = all` | 同第 4 輪 | 適配器那條連續被判送不動（暫停 2→30 s 加倍），等於只剩家用那條；22:15:13 家用那條 174 ms 空檔＋20～35% 掉包時沒有東西可頂，10 s 內 380 張重複幀 | 稍稍改善；第二個位置測到一半突然掉幀、浮動嚴重 |
| 6 | 同上但 `vr_multilink = always`（不暫停、送不完就丟） | 同第 4 輪 | 適配器那條仍送出 50～90 Mbps，但到達比家用那條晚 30～50 ms，被採用的封包接近 0；頭盔等落後那條才回報缺包 → 補包幾乎失效（掉幀 59 次、只恢復 11 次、1 次退回 IDR） | 沒有改善 |
| 7 | 每眼 **1152**（對齊 Steam 的編碼寬度）、`auto` | MTP p50 41～45 ms（預測收斂前；後段 51 ms）、解碼 3.5 ms（2160 時 7.5～8.5 ms）、頭動時旋轉誤差 p50 0.06～0.3° | 沒測到：75 秒後適配器那條突然雙向不通，ENet 控制連線跟著斷，session 結束 | —（使用者摸到適配器很燙） |

- **Steam 的 log 與設定給的線索**（`Steam\logs\vrserver.txt`、vrlink driver 的 `default.vrsettings`）：
  `Default automatic encoded video size: 1152`、`Using 10bit mode: 1`、`Render Target: 2160 2160`、`Uber link enabled`、
  兩張網卡各綁一個 UDP 10400、`nTimedRetryOption = 1`；設定預設值 `encodeWidth 1536`／`renderWidth 2048`
  （編碼尺寸小於算圖尺寸）、`targetBandwidth 200`、`allowMultipleLinks true`。它的 log 沒有每幀的延遲或掉幀統計；
  設定裡有 `showAdvancedGraphs`（頭盔內的進階效能圖表），要比延遲只能靠它。
  第一次嘗試時 Steam 只走家用那條（適配器 0 Mbps），第二次連不上（host 每秒約 100 筆
  `Null VTE_CLIENT_SESSION_ID`、30 秒後 SteamVR 結束），第三次才是兩條全送——它也不是每次都全複製。
- **結論一：我們為解析度付的延遲代價太高。** 每眼 2160 原尺寸傳，像素量約是 Steam 編碼尺寸的三到四倍
  （1152 是每眼寬度還是搭配注視點壓縮後的尺寸，log 沒有說明）。降到每眼 1152 後 MTP 少 10 ms 以上、解碼有一半以上的餘裕。
  方向：算圖維持高解析度，編碼時縮小並把畫質集中在視線中央（頭盔有眼動追蹤）；10-bit。
- **結論二：差的那條連線要「不排隊」才有用。** 我們在每條連線上排隊最多等兩個幀週期，送不動的那條送出去的
  都是過期的；Steam 那條差的連線顯然不是這樣（從結果推的）。要改成只送當下這一幀、上一幀沒送完的直接丟；
  頭盔回報缺包時不等明顯落後的那條。改好之前 `always` 不要用。
- **結論三：控制連線不能換路。** ENet 只走 session 開啟時的那個位址（這幾場都是適配器那條）；第 7 輪那條一斷，
  影像還在家用那條上送，session 卻因為控制連線中斷而結束。Steam 沒有這個問題。
- 適配器那條在第 7 輪為什麼整條不通沒有查到原因：host 的網卡計數器沒有錯誤、頭盔的熱點服務沒有異常紀錄，
  幾十秒後自己恢復。使用者摸到適配器很燙；網路上有實測約 58°C 的影片（結論是「會燙但不用慌」），沒找到把過熱
  和斷線連起來的討論。當晚它連續跑了六場，其中兩場是 Steam 兩條各 200 Mbps、一場是 `always`。
- **同日晚的程式改動**：探測放行修正（見上）；`vr_multilink = always`（實驗用）；client 的 `--vr-overscan` 與
  `--vr-eye` 改成會保存的設定（`vroverscandeg`、`vreyewidth`／`vreyeheight`），GUI 啟動的 VR 也吃得到。
  路由器 log 的對照做法：只記連上、不記斷開，要先濾掉每 2 秒一行的洗版訊息。

**算圖尺寸與串流尺寸分開（§VR-RENDER-SCALE，2026-10-08 凌晨；server `vr_render_scale_pct`，預設 100＝舊行為）**

- 使用者定的優先順序：**流暢第一，解析度可以降，但一定要贏過 Steam 內建串流**。
- 做法：driver 回報給 SteamVR 的建議算圖尺寸＝串流的每眼尺寸 × 百分比（100～250），合成時縮到串流尺寸。
  session config 用掉原本的 `reserved2`（版面不變、不升 ABI 版號，0＝100%）。改了要等 SteamVR 重啟才生效。
- 驗證：串流每眼 1152、`vr_render_scale_pct = 188` → 遊戲的貼圖 2164×2164（`[VIPLE-VR-DRV] layer0 … size=`），
  `<dev-client>` 模擬存下的畫面球網、磚牆的細節都在。頭盔無人場次（120 Hz、合成頭部運動、4 分鐘）：
  MTP p50 41～45 ms、開場之後 0 張重複幀、0 次掉幀、解碼 3.2 ms、兩條各 82 Mbps。
  同條件每眼 2160 是 MTP 55～59 ms、解碼約 8 ms。
- MTP 每兩三分鐘會從 41 慢慢升到 45 再跳回（相位慢慢滑、滑過一格），2160 時也有同樣的形狀；還沒查。
- dev 選項 `--vr-synthetic-hands`（不寫入設定）：左右控制器換成在眼前揮動的合成姿態（右手像揮拍，左右 ±25 cm @1.3 Hz、
  拍面 ±40°；左手小幅上下），按鍵不動；`--vr-emulate` 與真的 XR 連線都能用，可以和 `--vr-synthetic-hmd` 併用。
  桌球一啟動就在對打，無人場次的畫面裡因此有會動的近物。`<dev-client>` 模擬存下的畫面看得到球拍掃過視野；
  頭盔無人場次 2 分鐘，控制器全程被 host 視為有在追蹤、零掉幀。

**延遲拆解與三項修正（2026-10-08 上午；頭盔無人場次，120 Hz、每眼 1152、合成頭部與手部運動）**

- **MTP 約 42～45 ms 的構成**（從兩端既有的 log 湊出來）：頭盔取樣到 host 約 1～2 ms；host 的遊戲拿到姿態到 Present
  約 3.5 ms（`[VIPLE-VR-CAP] echoAge`）；擷取＋編碼約 2.7 ms（`hostLatencyAvgMs`）；網路約 2 ms；解碼 3.2～3.5 ms；
  等 latch 的餘裕約 3.3 ms；**剩下約 26 ms 是頭盔的 XR runtime 在顯示前就把 frame thread 叫醒的提前量**
  （`[VIPLE-XR] 10s … predictAhead`）。也就是 host、網路、解碼加起來只有約 16 ms，最大的一塊在頭盔的合成管線。
- **§VR-LATE-LATCH（client `--vr-latch-delay <ms>`，會保存，預設 0）**：frame thread 在 `xrWaitFrame` 返回後先等幾毫秒
  才挑影像。runtime 會把提前量加大一部分來補償，淨效果要看「提前量 − 等待」：

  | 等待 | predictAhead | 提前量 − 等待 | 漏幀 |
  |---|---|---|---|
  | 0 ms | 26.3 ms | 26.3 ms | 0% |
  | 4 ms | 28.0 ms | 24.0 ms | 0% |
  | 6 ms | 28.5 ms | 22.5 ms | 0% |
  | 8 ms | 36.3 ms | 28.3 ms | 0.5%，新畫面掉到每 10 s 約 1090 張 |
  | 10 ms | 31.1 ms | 21.1 ms | 19% |

  6 ms 以內穩定省約 2～4 ms，再多 runtime 就把整幀往後排。頭盔目前設 5 ms（10-10 起是 6 ms）。約 21～22 ms 是這個 runtime 的下限，
  app 這邊壓不下去。
- **§VR-LINK-GRACE（控制連線斷了不結束 session）**：10-07 晚適配器那條整條不通時，影像還在另一條上送，session 卻在
  3 秒內結束——client 的 lossStats 執行緒送不出 ping／FEC 狀態就直接終止。現在只要多連線還有一條活著（client：1 s 內
  收過 PONG；server：2 s 內收過帶 CONFIRMED 的 PING），兩端都撐住，沿用既有的 §Q-ENET-RECONNECT 等 ENet 重連。
  ENet 斷線期間 FEC 狀態、ping、觸覺回饋不送；LOSS、LATCH、NACK、追蹤本來就走連線。
  驗證：host 用防火牆擋掉控制埠 28 秒，session 不斷、每 10 s 新畫面 1193～1199 張，解除後 24 秒重連成功。
  已知：正常結束時 server 會多等到連線全部失效（最多約 5 秒）才收尾；重連仍然連回同一個 host 位址（不換位址）。
- **§VR-LINK-FRESH（`always` 模式下差的連線不排隊）**：另有一條送達率正常的連線在送時，這條的批次只留四分之一的
  期限（約半個幀週期），送不完就丟。S0（一條限速 30 Mbps、另一條每 489 ms 連掉 45 個封包）：限速那條的落後從
  30～50 ms 變成 0，缺包回報 21／21 全數補回、0 次掉幀（改之前同樣的情境補包幾乎失效）。真頭盔上還沒試。
- host 的 **Smart App Control** 在 10-08 08:28 從評估模式變成強制模式，沒有簽章的新 server 執行檔全部被擋
  （`blocked by your organization's Device Guard policy`，exit 4551；服務外殼在跑、主程式起不來、`sunshine.log`
  不更新）。使用者把它關掉後恢復。開發用的 host 要關著；要開著就得簽章。

**半速串流與補幀（§VR-HALF-RATE、§VR-SYNTH，2026-10-08；使用者提出，參考 Virtual Desktop 的做法；都是 client 設定、預設關閉）**

- **§VR-HALF-RATE（`--vr-half-rate`，保存為 `vrhalfrate`）**：client 向 host 要求的更新率是頭盔顯示更新率的一半
  （120 Hz 顯示 → 60 Hz 串流；低於協定下限 60 Hz 時不生效）。host 的 SteamVR、遊戲、編碼都跑 60 Hz；頭盔照樣每
  8.3 ms 顯示一次，沒有新影像的那一格重送上一張，轉動由頭盔的重投影補。追蹤樣本仍照顯示更新率的兩倍送。
  頭盔無人場次：每 10 s 新影像 600 張、重送 600 次，零漏幀、零掉幀；每條約 40 Mbps（全速時約 80）；
  MTP p50 約 43～50 ms（全速時 41～45；每張影像的間隔變長）。
- **§VR-SYNTH（`--vr-synth`，保存為 `vrsynth`，要同時開半速串流）**：沒有新影像的那一格不重送，改顯示合成的影像。
  做法（`XrVideo::renderSynth`，全部在頭盔端的影像 render thread）：
  1. swapchain 的高度加倍。每張解碼好的影像先畫到自己的一張 RGB 材質，再縮成 1/8（每個像素取 8x8 區塊的平均）。
  2. 位移估計（1/8 尺寸，fragment shader）：頭部轉動造成的整張影像位移，由前後兩張的算圖姿態算出來（小角度近似，
     用串流的每眼視角換算成 UV），先扣掉——那一部分本來就由頭盔的重投影處理。剩下的用 3x3 稀疏區塊在 ±4 格內比對，
     拋物線取到小數格；比「沒有位移」好不到 40% 就當成 0。所以靜止的場景原樣保留，只有真的在動的物體有位移。
  3. 最後一個 pass 寫 swapchain：上半原樣複製這一張，下半把這一張沿（平滑過的）位移往前推半個串流週期。
  4. frame thread：一張新影像第一次顯示用上半；同一張再被顯示時改用下半（`imageRect` 往下移一張的高度），
     layer 的姿態不變。前後兩張不連續（掉過幀）、沒有算圖姿態、或兩張之間轉超過 10° 時不合成，照舊重送。
- 頭盔無人場次（合成頭部與手部運動、桌球對打）：每 10 s 合成 600 張、顯示 600 次，零漏幀；影像 render thread 等 GPU
  的時間中位數從 1.4 ms 變成 2.1 ms（p95 3.5 ms），所以真的那一張也晚約 0.7 ms 發布。
  用 `--xr-dump-frame` 存下 swapchain（上下兩半）比對：靜止的牆面、窗框、選單文字兩半一致，在動的手把被往前推。
- 做的過程：第一版把整個位移場照單全收、姿態用外插的，文字與窗框明顯扭曲；第二版改成扣掉頭部轉動、只留有把握的
  位移，靜止場景不再變形，但細字還會被誤判；第三版把縮圖從雙線性縮 8 倍改成區塊平均（細字的鋸齒不再隨畫面閃爍）、
  門檻從 25% 調到 40%，才乾淨。
- **第四版（2026-10-08 下午）：搜尋範圍加大、物體邊緣、每眼 2160**
  - 位移估計改成兩層：先在 1/16 尺寸找 ±6 格（全尺寸 ±96 像素），通過同一個 40% 門檻才當成 1/8 那一層的起點，
    再找 ±3 格（±24 像素），合計約 ±120 像素（原本 ±32）。粗的一層像素少 4 倍，比對次數和原本差不多。
    合成手把揮動的峰值約每幀 47 像素（1152 每眼），原本在峰值附近不外插，現在整段都會。
  - 物體邊緣：原本把位移場取四點平均，邊緣的位移被背景的 0 拉成一半。現在四點裡最大的明顯大於平均時（＝在邊緣上），
    如果「往回取樣的那一點在物體上」或「這一點現在就在物體上」就用最大的，兩邊都是背景就不動。第一次直接用最大的，
    手把旁邊的文字被拖歪，才加上這個條件。
  - 頭盔無人場次：每眼 1152 時每 10 s 合成 600 張、零漏幀，影像 render thread 等 GPU 的中位數 2.35 ms（p95 2.8；
    第三版 2.1／3.5）。**每眼 2160**（host 算圖比例暫時設 100%）：一樣每 10 s 600 張、9227 幀漏 1 幀，GPU 中位數
    4.3 ms（p95 5.7）——跑得動，但這 4 ms 會加在真的那一張的延遲上。
  - 抽幀看：靜止的背景兩半一致；揮動中的球拍在合成那一格往前移了約 18 像素（半個串流週期），球拍握把與影子的邊緣
    有鋸齒（位移場一格是 8 像素，邊緣是「整格用或不用」）。
- **第五版（2026-10-08 傍晚）：真的那一張先發布、頭部轉動逐像素算**
  - 原本真的與合成的寫在同一張 swapchain 影像，要等位移估計與合成都做完才 release，真的那一張因此晚 1～3 ms
    才能顯示，半速串流時每 10 s 有幾 % 的幀趕不上、晚一格。現在每張串流影像 release 兩次：第一張只畫真的
    （和沒開合成時一樣），發布；第二張畫「上半真的、下半合成」，做完後以同一個 seq 再發布一次。第二張來不及時
    那一格照舊重送真的。
  - 頭盔無人場次（半速＋補幀＋鎖相）：真的那一張等 GPU 的中位數 2.35 → 0.75 ms（p95 3.9 → 0.85）；合成那一張另外
    3.3 ms（不影響真的那一張）。MTP p50 42.9 → 41.4 ms；p95 原本常常是 51（晚一格），現在大多和 p50 一樣，
    只剩 p99 還看得到晚一格。每 10 s 合成 600 張、零漏幀。
  - 頭部轉動造成的影像位移改成逐像素算（含繞視線軸的轉動）。原本只用影像中心的值，邊緣每幀會差好幾個像素；
    第四版把搜尋範圍加大之後，窗格這類重複的圖樣在頭轉動時被配到隔壁一格，合成那一格的窗框歪掉。改完後抽幀看，
    頭在轉的時候窗框與牆面兩半一致。
  - 觀察：其中一場（同樣設定）整場 MTP 高了 13 ms，多出來的時間在 host——遊戲讀姿態到 Present 從 3.4 ms 變成
    18.8 ms（多一整幀），下一場又恢復。和這次 client 的改動無關，原因未查。
    （10-10 補：這是 host 直通時的兩種時序，見「host 直通時的兩種時序」。）
- **2026-10-10：使用者第一次配戴（半速＋補幀＋注視點 150%），畫面整個不對、一直閃——第五版的 bug，已修**
  - 原因：每張串流影像 release 兩次，先發布的那一張目標裁切是兩倍高 swapchain 的上半。注視點開著時這一張走
    `renderUnwarp`，它把同一個裁切原封不動帶到中間材質（解碼尺寸，比裁切小）；libplacebo 把目的範圍夾回材質大小、
    來源跟著裁，中間材質只剩原始影像左上角一塊再放大（注視點 125% 是 80%、150% 是 2/3、200% 是一半），右眼那一半
    有一大塊是左眼的內容。第二次發布的那一張上半是另外重畫的、是對的，所以只有「第一次顯示時第二次發布還沒完成」
    的幀會顯示到錯的那一張：配戴那兩場佔串流幀的 16～32%，每秒十幾格整張錯位的畫面。
  - 為什麼先前沒發現：第五版的驗證場次都沒開注視點；`--xr-dump-frame` 在補幀開著時只存第二張；而收幀數、MTP、
    重複格數這些統計都看不到畫面內容。
  - 修正：畫中間材質時裁切歸零（畫滿整張）。同時補上一個原本就有的空窗：這一格決定顯示合成那一半之後、
    `xrEndFrame` 之前，影像 render thread 剛好 release 了下一張影像的第一次發布——runtime 用的是最後 release 的
    那一張，而它的下半不是合成畫面（開注視點時是最後一列拉出來的直條紋，沒開時整片黑）。現在遇到就改顯示上半
    （任何一張的上半都是真的），姿態用新發布的那一張。`[VIPLE-VR-MTP10]` 行尾的 `synthGuard=N` 是這個視窗
    遇到幾次（開著補幀才印；穩態每 10 秒 0～1 次）。防護觸發的那一格記帳沒有跟著改：下一格會再顯示一次
    真的那一張，合成那一格晚一格才出現（待辦）。
  - **驗證方法（之後動到這條路徑都要做）**：`--xr-dump-frame <路徑>` 在補幀開著時每組存兩張——`<路徑>` 是第二次發布
    的那一張（上半真的、下半合成），`<路徑>-first.png` 是第一次發布的那一張（上半真的；下半不會被顯示）；
    從第 2400 張起每 480 張存一組、一共六組（第 2 組起檔名加 `-2`、`-3`…），只看一組抓不到偶發的錯。
    兩張的上半必須逐像素相同，而且要用實際會用的設定組合跑（注視點開著）。頭盔無人場次（半速＋補幀＋注視點
    150%）：修正前兩張上半的 PSNR 只有 10.1 dB，目視就是放大錯位的畫面；修正後逐像素相同。合成那一半與真的
    那一半只在移動中的球拍與手把附近不同（約 0.4% 的像素），球拍邊緣有碎塊與鋸齒。
  - **在動的東西有沒有被往對的方向、推對的距離（2026-10-10 加的檢查）**：上面那一組影像只看得出合成的那一張和真的差在哪，
    一張靜止的圖分不出「往前推」和「往後拉」。補幀開著時 `--xr-dump-frame` 每組另外存三個檔：`<路徑>-prev8.png`、
    `<路徑>-cur8.png`（估位移用的上一張與這一張的 1/8 縮圖）、`<路徑>-flow.bin`（shader 算出來的位移場：寬×高×4 個 float16，
    xy＝扣掉頭部轉動之後的位移，UV 單位，上一張的位置 − 這一張的位置）。`moonlight-qt/scripts/steamframe/synth-check/flow_check.py`
    用兩張縮圖另外算一份參考位移（numpy 區塊比對，和 shader 無關），再比兩件事：A＝shader 的位移場和參考位移的方向與大小；
    B＝合成的那一張相對於真的那一張實際被移了多少（同一張存圖上半對下半），和「參考位移的一半、往前」比。方向的定義先用
    `--selftest` 驗過。頭盔無人場次（半速＋補幀，合成的手每秒揮 1.3 次、最快一幀約 150 個像素；每場六組）：

    | 場次 | A：方向（cos）／大小比 | B：方向（cos）／大小比 | 靜止的地方被判成在動 | shader 判成在動的像素 |
    |---|---|---|---|---|
    | 頭不動、不開注視點 | 0.98～1.00／0.98～1.03 | 0.98～1.00／0.92～0.97 | 0.00% | 0.8%（最多 2.2%） |
    | 頭不動、注視點 150% | 0.92～1.00／0.94～1.01 | 0.82～1.00／0.92～0.99 | 0.00% | 1.8%（最多 2.3%） |
    | 頭照 sine 轉動與平移、注視點 150% | （參考位移含頭部轉動，不適用） | （同左） | 最多 0.62% | 2.1%（最多 2.8%） |

    也就是：在動的東西被往它原本移動的方向推，推的量是該推的九成多（邊緣四點平均會少推一些）；頭在轉的時候整個背景
    沒有被當成在動（頭部轉動的位移有先扣掉，判成在動的只有手那一小塊，和頭不動時一樣多）。參考位移說在動、shader 沒有
    跟上的像素約一成（物體邊緣，以及一幀移動超過搜尋範圍——1/8 縮圖上約 15 格——的部分；沒跟上的地方就照原樣重複）。
    目視：快速揮動的球拍在合成那一張的邊緣是碎的、旁邊的桌線被拉出一段殘影（每張只顯示 8.3 ms）。
- **還沒驗的**：有人配戴時看起來如何（合成那一格的邊緣鋸齒明不明顯、快速揮動時的殘影、頭部平移造成的視差）；
  手把以外的動態物體（球、對手）。位移是在 1/8 縮圖上以約 4.5 格見方的區塊估的，桌球這種在畫面上只有一兩格大的小東西
  多半估不到——估不到就不外插，那顆球在半速時會照 60 Hz 重複顯示（眼睛跟著球看時會覺得一頓一頓或有疊影）；這一點
  無人場次的畫面裡沒有球、沒有驗到，配戴時要特別看。合成只做「往前推」（外插），不是在兩張真的影像之間內插，所以不增加延遲。

**2026-10-10 配戴後查到的另外兩個問題（都已修；第一個只影響半速，第二個全速也有）**

1. **半速時 `<host>` 的 §VR-PREDICT 不調整，起始值又算錯：整場姿態超前約 23 ms。**
   - 配戴的那兩場（半速＋補幀）：client 的 `[VIPLE-VR-POSEERR]` 姿態落後中位數 −19.7～−25.5 ms（負值＝超前；同一個 server
     行程先跑的全速場次是 0 ms 上下），位置誤差中位數 3.9 mm（全速 1.3 mm），轉動誤差超過 3° 的幀 14～34%（全速 6～15%）。
     頭盔的重投影只修轉動、不修平移，所以頭一動近處的東西就跟著晃，轉頭時也比較容易露出黑邊。
   - 原因一：`on_pose_lag()` 丟掉一秒內樣本不到 60 筆的視窗。client 只對「第一次顯示的新影像、而且頭部有移動」取樣，
     60 Hz 串流每秒最多 60 筆、配戴實測 41～45 筆，所以半速時沒有任何一個視窗被採用，整場不探測也不追蹤。
   - 原因二：這個週期沒學過時，起始值由別的週期學到的值換算（v' = F + (v − F)·T'/T，F＝14 ms）。這個式子是用頭盔的
     顯示更新率 120 與 90 Hz 兩點定出來的；半速只有 `<host>` 的週期加倍、頭盔的顯示週期沒變，不適用——120 Hz 學到的
     33.7 ms 被換成 53.4 ms。由那兩場反推，半速該用的值約 31～36 ms，和全速學到的 34～36 ms 接近。
   - 修正：(1) 樣本數門檻跟著串流幀率，取半秒的幀數、夾在 20～60（120 Hz 仍是 60、90 Hz 是 45、60 Hz 是 30）；
     (2) `/launch` 新增選填的 `vrDispHz`＝頭盔的顯示更新率，client 只在半速串流時帶（`vr_protocol.md`）；(3) 學到的值依
     「host 週期、顯示週期」分開存；起始值先找完全相符的，沒有就取顯示更新率最接近的那一筆——顯示更新率相同就直接沿用
     （全速與半速互相沿用、不換算），不同才照原本的式子用顯示週期換算；什麼都沒學過時的預設是「顯示週期＋30 ms」
     （半速由 46.7 變成 38.3 ms）。全速、不帶 `vrDispHz` 的場次，起始值與每一步調整都和以前相同（有逐步比對的單元測試）。
     舊 client 不帶 `vrDispHz`：起始值照舊，但追蹤會動作，約 20 秒收斂。
   - `<host>` 落在多一格的時序（見「host 直通時的兩種時序」）時需要的值比較大（60 Hz 估計約 44 ms），同樣靠追蹤補上。
   - **原本在無人場次驗不到**：合成的頭部姿態量不到姿態落後（`lag n=0`），所以半速時追蹤壞了一直沒被看到；修正當時的依據
     只有單元測試（含照配戴那一場的數字重播：樣本 43 筆、落後 −22.8 ms，暖機後收斂到 30.6 ms 上下）。
   - **2026-10-10 中午補上量測並整條驗過**：`--vr-synthetic-hmd` 的場次，姿態誤差與落後改對「合成軌跡在該幀顯示時刻的值」量
     （10 s 行行尾有 `ref=synthetic`；`log_tags.md`）。順帶修掉合成樣本本身的一個偏差——它送的是取樣當下的姿態、卻帶著 runtime 的
     預測長度（頭盔上約 26 ms），`<host>` 把它當成那麼久之後的姿態，等於整場固定落後 26 ms。頭盔無人場次（sine、各 150 秒）：

     | | 開場 | `<host>` 的動作 | 之後每 10 秒的落後中位數 |
     |---|---|---|---|
     | 半速 60 Hz＋補幀 | `session start vsync_to_photons_us=38333 (default: display period + 30 ms) stream=60.0Hz display=120.0Hz minSamples=30`；落後 −6.5 ms | 中途 host 轉成多一格的時序（落後變 +6.9 ms）→ 探測 38333 → 46333 µs（落後 9.6 → 1.7 ms）→ 追蹤，停在 47770 µs | −0.2～+0.7 ms |
     | 全速 120 Hz | 38333 µs；落後 +0.5 ms | 探測後追蹤，停在 40733 µs | −0.6～−0.9 ms |

     半速時 `<host>` 的探測、追蹤與 10 s 行都出現了，落後收斂到 1 ms 以內，多一格的時序也追得上。無人場次收斂的值預期比配戴
     小一點（合成樣本沒有 runtime 那一段預測誤差；慢動作差 0～1 ms），配戴時仍要看一次 `[VIPLE-VR-POSEERR] … lag … p50=`。
     **副作用**：無人場次從此會改到 `<host>` 行程內學到的預測時間（同一個 server 行程的下一場從它起跑）；無人場次跑完要重啟
     server（本機的 `scripts/vr/hs_run.sh` 收尾時會做）或把 `vr_vsync_to_photons_us` 固定，再讓人戴。
2. **注視點還原：右眼的垂直中心用了左眼的值（10-08 加注視點編碼時就有，全速也有）。**
   - `<host>` 的合成器每隻眼各自算「正前方在本眼影像裡的位置」再壓縮；client 還原時水平中心每眼各自算，垂直中心卻只用
     左眼的上下視角算一個值、兩眼共用。Steam Frame 兩眼的上下視角不同：垂直中心（從上緣算）左眼是影像高度的 39.78%、
     右眼 40.77%。右眼用左眼的值還原，畫面中央垂直偏（中心差）×(1−a) 個影像高度，往上下邊緣遞減到 0；只有右眼偏，
     等於兩眼之間多了一個垂直視差。
   - 實測（頭盔無人場次、合成的頭部姿態固定不動、畫面靜止；注視點 150% 與關閉各存一張比對）：左眼的垂直位移 0.00～0.03 px，
     右眼中央 1.97 px（存圖每眼高 640 px，也就是影像高度的 0.31%、約 0.57°）。算出來的值是 0.33%、0.61°。
     125% 約 0.37°、200% 約 0.92°。
   - 修正：垂直中心每眼各自算、各自傳給 shader（還原、補幀的合成、補幀估位移三個 pass 都改）。修正後同樣的比對，
     兩眼都在 0.03 px 以內（六組存圖都一樣）。
   - 量法留著用：`--vr-synthetic-hmd --vr-synthetic-motion still`（不要帶 `--vr-synthetic-hands`）各跑一場注視點開與關，
     用 `--xr-dump-frame` 的存圖逐眼、逐高度做垂直方向的對位。之後動到注視點的壓縮或還原都要做這個比對。

**10-08 晚配戴實測：重複顯示的格數（repeat）才是「不夠流暢」的主因**

使用者配戴三輪，同一個位置、同一個遊戲（桌球實際對打），每眼 1152、120 Hz、延後挑影像 5 ms、鎖相開、host LATCH v2。
三輪都零丟包（穩態）、頭盔端 XR 漏幀 0、Wi-Fi 整晚沒有漫遊、client 沒有崩潰。穩態＝開場 40 s 之後、結束前 10 s 之前。

| | 第 1 輪 | 第 1b 輪 | 第 2 輪 | 無人對照（N26） |
|---|---|---|---|---|
| host `vr_latch_target_pct`（目標 slack） | 40%（3.33 ms） | 50%（4.17 ms） | 50%（4.17 ms） | 40%（3.33 ms） |
| 注視點編碼 | 關 | 關 | 1.5x | 關 |
| repeat／10 s（佔顯示格數） | 40.7（19～74），3.39% | 13.2（0～26），1.10% | 1.15（0～4），0.10% | 1.35（0～4），0.11% |
| skip／10 s | 40.7 | 13.2 | 1.15 | 1.35 |
| MTP p50（ms） | 45.0 | 45.4 | 45.7 | 40.6 |
| client 等 GPU p50／p95（ms） | 0.93／2.2～3.0 | 0.94／1.00～1.06 | 1.55／1.60～1.63 | 0.93／0.97 |
| host echoAge p50／p95（ms） | 5.82／7.42 | 5.44／6.86 | 5.2／5.8～6.3 | 2.69／3.19 |
| 使用者體感 | 跟以前差不多 | 抖動與掉幀稍稍改善 | 非常好，幾乎沒有抖動與掉幀 | — |

從 log 確定的事（細節與行號在分析紀錄裡，這裡只留結論）：

- **repeat 不是缺幀**。穩態每 10 s 收 1201 張、畫 1201 張、被蓋掉 0 張，repeat 與 skip 一對一相等：每一次重複，都有一張已經畫好的
  影像沒趕上 latch、下一格和後一張同時到、被跳過。MTP 的統計不含重複的那幾格，所以 MTP 看起來一樣、體感卻差很多；
  之後驗收要看 repeat／skip，不能只看 MTP。
- **頭盔端有一段和顯示同步的 GPU 忙碌期**。延後 5 ms、目標 40% 時，slack 低於約 2.2 ms 的影像不會平順地變少，而是整批多等約 2 ms：
  17 場 301 筆 slack 樣本沒有一筆落在 latch 前 0.47～1.89 ms，被拖住的影像不是擠在 latch 前 0.5 ms 內、就是錯過。
  目標調到 50% 之所以有效，主要是讓我們畫那一張的 GPU 工作避開這一段（等 GPU 的 p95 由 2.2～3.0 降到 1.0 ms），不只是多 0.83 ms。
  等 GPU 的 p95 偏高是相位的結果而不是原因（把握約八成五；無人、延後 6 ms 的場次 p95 一樣 3 ms，repeat 卻是 0）。
  當時忙碌期是什麼在用 GPU 還沒有直接證據（推測是頭盔 runtime 的合成；頭盔的 vrcompositor 統計：合成器每格 GPU 約 0.9～1.2 ms）。
  （10-09 補：已用 gpu-trace 確認就是頭盔的合成器，多等的時間實測是 1.4～1.55 ms，見「頭盔的「GPU 忙碌期」定案」。）
- **餘裕再大，repeat 就接近 0**：第 1b 輪收斂途中有 10 s slack 落在 4.6～7 ms，那 10 s 在實際對打下是 1201 張全新、repeat 0，
  代價是 MTP 多約 2.5 ms。
- **第 2 輪比第 1b 輪少 10 倍，原因沒有定論**。設定只差注視點 1.5x；那一場 host 的 echoAge p95−p50 也比較小（0.6～0.9 對 1.1～1.6 ms），
  頭盔合成器的 GPU 時間也比較低（0.9 對 1.17 ms）。兩種組合各只跑一場、各約 2 分鐘，場與場的差異比要量的效果大——
  之後任何配戴比較都要用同一場內切換。
- **開場 30～40 s 很差，大部分不是相位**。第 1b 輪頭三個視窗 repeat 共 298 張，其中 249 張是 host 根本沒送（SteamVR 啟動與遊戲載入，
  遊戲在第 9～12 s 才開始交幀）；相位造成的每 10 s 只有 12～19 張。遊戲開始出幀時 host 管線多 4～5 ms，v2 只能以每秒 0.15～0.25 ms
  移相位，要約 40 s 才追上，這段期間主要是延遲多 2～3 ms。
- **v2 控制器的幾個性質**（讀程式確認）：它只調虛擬 vsync 的週期（±200 ppm），不是相位命令，driver 現成的一次性相位對齊從沒用過；
  輸入是每 100 ms 一筆「最近一張新影像」的 slack，被跳過的影像沒有樣本；目標 50% 時折返點剛好落在 latch 邊緣，「剛錯過」的樣本
  一律被判成太早（同一個開場讀值，40% 判成晚到、50% 判成早到）；host log 的 slackEma 在 v2 是「目標＋折返後誤差」的平均，
  樣本被折返時會顯示得很小，不是真的 slack 只剩那麼多。
- **配戴比無人多出來的約 4.5 ms MTP 都在 host**：echoAge p50 多 2.7～3.1 ms（遊戲讀姿態到 Present），擷取事件到送進編碼器
  （evtToPush）由 0.97 變 2.38 ms。和流暢度無關，是另一個可以省的地方。

**依這三輪做的改動（2026-10-09；設計與實作各經過一輪對抗式審查；除了量測之外全部在開關後面、預設關）**

| 代號 | 在哪一端 | 開關（預設） | 做什麼 |
|---|---|---|---|
| §VR-LDIAG | client | 一律開（只多 log） | 每張發布的影像相對 latch 的直方圖、晚到／缺幀／早到的分類、等 GPU 對顯示相位的分佈、latch 抖動、每個顯示格的內容年齡（含重複的格）；開場印出這一場的設定值 |
| host 量測 | server | 一律開（只多 log 欄位） | driver 實際採用的週期（periodPpm）、每一幀 Present 落在格內的位置（presentPhase）、client 每 10 s 的新影像數 |
| §VR-LATCH-V3 | server | `vr_latch_mode = v3`（預設仍是 legacy；host 目前 v2） | 相位用最近 7 筆樣本的環狀中位數、誤差走最短路徑（沒有折返點）；host 沒在出幀或影像停住時不取樣本、輸出只留積分；積分在同一個 server 行程內跨場沿用。目標另有 `vr_latch_target_v3_pct`（0＝沿用 `vr_latch_target_pct`，可到 65） |
| §VR-LATCH-ACQ | server | `vr_latch_acquire = enabled`（預設關，要 v3） | 誤差超過 0.15 個週期時，把虛擬 vsync 的週期暫時改成 ±`vr_latch_acquire_ppm`（預設 4000）一段算好的時間，一次把相位移到位；不夠再補一步，失敗只退避、不停用。driver 與 vripc ABI 沒有改 |
| 同場 A/B | server | `vr_latch_ab = <秒>:<模式>:<target%>`（預設空） | 每隔這麼多秒在設定檔的模式／目標與指定的那一組之間切換，給「同一場內比較」用 |
| §VR-TRACK-RELOCK | client | `vrtrackrelock`（預設關，要鎖相） | 鎖相維持中有 15% 以上的幀用到前一筆樣本、連 5 s → 重新搜尋（host 的相位一移，原本只看中位數的偵測會漏掉） |
| §VR-LATCH-RELATCH | client | `vrlatchrelatch`（預設關） | xrEndFrame 前再確認一次：這段期間又到一張新影像時，改用新那一張自己的姿態（原本只計數成 mismatch） |
| §VR-LATCH-WAIT | client | `vrlatchwaitms`（預設 0，最多 1.0） | 這一格該有的新影像還沒到就等它，延後＋等待的總和上限 6.0 ms；runtime 的提前量一跳就自動暫停 |
| 同場 A/B | client | `vrlatchabsec`（預設 0） | 每隔這麼多秒把 relatch 與等待在開／關之間切換 |
| dev | client | `--vr-test-jitter <spec>`（不保存） | 影像 render thread 畫每一張之前照規則延後，模擬到達抖動（worn／light／early／scan，可加 shift） |

- CLI 的 `--vr-track-relock`、`--vr-latch-relatch`、`--vr-latch-wait` 只覆寫這一次、不寫回設定（無人場次的參數不會留在頭盔上）；
  設定頁這一輪沒有加，配戴場用 `scripts/vr/hs_set.sh` 設保存鍵。
- 沒有採用的：依晚到比例自動調整餘裕（就算餘裕很大也有零星晚到，會被這個底噪一路推到上限——鎖相功能同一天犯過一樣的錯）、
  driver 現成的一次性相位對齊（正式路徑從沒跑過）、client 自己追相位、LATCH 加長（相位用現有的 12 B 就夠，線上格式不變）。
- **驗證到哪裡**：
  - 單元測試：控制器 54 個（既有 22 個原封不動；legacy／v2 的輸出與改動前逐位相同）、client 的分類器自測 27 個案例
    （`VipleStream.exe xr-probe --selftest-latch`）。Windows server／client、x86_64 與 arm64 的 Linux client 都建置通過。
  - **沒有頭盔時用 Monado 驗（linux-builder 當 XR 端連 host 的真 driver；顯示 20 Hz、串流 60 Hz）**：
    舊的 v2 花約 50～60 s 才對齊（ppm 頂在 −200 約 50 s）；v3 不開步進約 20 s；**v3＋步進一步到位**——
    `step why=acq err=4330us … burst=4000ppm x 1082ms` → `step result: moved=5038us of 4330us err=-708us -> track`，
    從 driver 啟動算起約 3 s 進入追蹤。driver 沒有拒收（pacingRejected 0、periodPpm 那個視窗 −86／4000、bursts 1），
    有步進的開場視窗漏的 vsync（45）和沒步進的（47）一樣，之後每個視窗都是 0；穩態 100 s 沒有再步進。
  - client 的新開關（relatch、等待、鎖相重找、抖動注入一起開）在 Monado 上跑完一場沒有當掉或卡死，CLI 覆寫確認沒有被存下來。
  - **還沒驗的（都要頭盔）**：配戴下的任何一項（無人、120 Hz、XR 狀態 FOCUSED 的 `[VIPLE-VR-LDIAG]` 在 10-09／10 收過；
    頭盔 GPU 忙碌期的位置也已定案，見「頭盔的「GPU 忙碌期」定案」）；
    步進對 120 Hz 的 SteamVR 與遊戲的影響；relatch 與等待在真的 runtime 上的效果；v3 在配戴下和 v2 的同場比較。
- **下次配戴的順序**（一次只改一項，同位置同遊戲；跨場的比較不可信，能同場切換就同場切換）：
  1. 現有設定（host v2／50%、注視點 1.5x、鎖相；「延後挑影像」10-10 起已是 6 ms）玩 3 分鐘，收 `[VIPLE-VR-LDIAG]`
     ——late／gap／early 各佔多少——與 runtime 的重投影。
  2. （原本的「延後挑影像 5 → 6 ms」已在 10-09／10 的無人場次量過、頭盔已改成 6 ms；配戴下要不要再和 5 ms 同場比，看第 1 步的結果。）
  3. host 改 `vr_latch_mode = v3`＋`vr_latch_acquire = enabled`，看開場的對齊時間與穩態的 repeat；穩態用 `vr_latch_ab` 和 v2 同場比。
  4. 同場 A/B 注視點開／關（第 1b 輪與第 2 輪差 10 倍的原因）。
  5. 看 1 的資料再決定要不要開等待（`vrlatchwaitms`）與 relatch。
  6. 全部做完後，和 Steam 內建串流在同位置、同遊戲對照一次（驗收條件）。

**MTP 每兩三分鐘漂移再跳回的原因，與追蹤樣本鎖相（§VR-TRACK-LOCK，2026-10-08；client 設定、預設關閉）**

- 現象（頭盔無人場次 400 s）：MTP 中位數每 10 s 降 0.3 ms，從 42.6 降到 38.8，然後一次跳回 +4.17 ms，週期約 140 s。
  4.17 ms 正好是一筆追蹤樣本的間隔（120 Hz 顯示 × 2＝240 Hz）。
- 原因：追蹤執行緒用自己的計時（名目 1/240 s）送樣本，和顯示更新的節拍差了約 30 ppm；host 的虛擬 vsync 鎖在顯示的
  節拍上（LATCH 回授），所以「host 那一幀讀姿態時，最新那筆樣本有多舊」一直在 0～4.17 ms 之間滑。
  姿態的新舊由 host 的預測補償，所以這不是抖動的主因，但它讓延遲不固定，也讓每一場的 MTP 起點不一樣
  （同樣設定的兩場可以差 4～5 ms，之前以為是別的改動造成的）。
- 做法（`--vr-track-lock`，保存為 `vrtracklock`）：XR frame thread 每幀把「預測的顯示時刻」（runtime 排好的等間隔格點）
  交給追蹤執行緒，樣本改在 `顯示時刻 + 相位 + k·T` 送出。相位由 MTP 找：開場先等 20 s（host 的節拍還在收斂，
  太早找會鎖錯位置），然後每秒往後移 0.25 ms，看到 MTP 中位數跳高一個 T 就是撞到邊界，退回邊界前 2 ms 維持；
  連 3 秒比鎖定時高過半個 T 才重新找。
- 為什麼退 2 ms：樣本的到達時刻有 ±1.5 ms 左右的抖動，「用到前一筆的比例」對相位是一段約 3 ms 寬的斜坡。
  離邊界 1 ms 時有 2～3 成的幀用到前一筆（MTP 在兩個值之間跳，比鋸齒更糟），退到 2 ms 才降到 1～2% 的底噪。
  也試過依晚到比例自動調整距離：底噪本身就有 1～2%，控制器會一直往前退、繞一圈再撞上邊界，變成反向的鋸齒，
  所以改回固定距離。
- 結果（頭盔無人場次 300 s，每眼 1152、120 Hz）：41 s 鎖定，之後 MTP p50／p95 都是 40.6 ms，一路平的；
  用到前一筆樣本的幀約 1.3%；每 10 s 新影像 1197～1201 張（沒鎖時 1188～1200）、零漏幀。
  沒鎖的對照組是 38.8～43.0 ms 的鋸齒（平均約 40.9）。所以平均只少約 0.3 ms，好處是延遲固定、p95 不再週期性
  升到 43.5。
- 預設關閉，等使用者配戴確認沒有副作用再決定要不要預設開。`[VIPLE-VR-MTP10]` 尾端多了
  `trk search|hold phase=… ms late=…% researches=N`。

**半速串流多出來的延遲拆解（2026-10-08）**

- 之前記的「半速比全速多約 5 ms」有一部分是上面那個鋸齒造成的（兩場剛好落在不同的相位）。都開鎖相之後重量：
  全速 MTP p50 40.6 ms，半速＋補幀 42.9 ms，差 2.3 ms。
- 這 2.3 ms 的來源：補幀那幾個 pass 讓真的那一張晚約 1.1 ms 發布（影像 render thread 等 GPU 的中位數 0.9 → 2.1 ms）；
  其餘約 1.2 ms 在 host（60 Hz 時遊戲讀姿態到 Present 比較久，host 量到的樣本年齡 2.5 → 4.4 ms 裡扣掉相位的部分）。
  兩者都是這個做法本身的成本，沒有可以調掉的排程問題。
- 另外看到的尾巴：半速時每 10 s 有幾個百分點的幀晚一格顯示（MTP p99 51.2 ms＝多 8.3 ms）。原因是補幀的運算
  拖住真的那一張，補幀第五版把兩者分開之後大幅減少（見補幀那一節）。
- **2026-10-10 更正：上面的 40.6 與 42.9 ms 不是配戴時的延遲。** 那兩場（以及本節其他 10-08 的頭盔無人半速場次）
  `<host>` 的儀表板都開著，第 0 層是 SteamVR 合成器自己合成的貼圖，量到的姿態年齡是合成器那一次合成用的姿態，
  裡面的遊戲畫面是上一格交的，MTP 沒有含那一格。「host 量到的樣本年齡 2.5 → 4.4 ms」也對不上原始 log
  （兩場的 p50 是 2.70 與 3.16 ms，只差 0.46 ms；2.5 與 4.4 是一場的最小值配另一場的 p95）。
  直通（配戴的狀態）下半速的延遲見「host 直通時的兩種時序」。

**控制連線改連 host 的另一個位址（§VR-LINK-REHOME，2026-10-08）**

- §VR-LINK-GRACE 讓 session 在 ENet 控制連線斷掉時撐住（多連線還有一條活著就不結束），但重連只會試 session 起點
  的那個位址——頭盔上通常是熱點／適配器那一側。那條路一直不回來的話，控制連線就一直沒有（只走 ENet 的訊息，例如
  CLIENT_TIMING、震動，都不通）。
- 現在重連迴圈（`ControlStream.c`）每一輪先試 session 位址；**沒有路由、或連著兩次沒連上**，而多連線裡有另一個
  host 位址的連線還收得到 PONG（`vrmlOtherAliveServer`），就改連那個位址；再連著兩次失敗就換回來，如此輪流。
  server 以 connect data 認 session、不看來源位址，所以 server 不用改。下一次斷線時仍從 session 位址開始。
- 頭盔無人場次：串流中途在 host 擋掉熱點那一側進來的控制埠（防火牆規則，測完移除）。ENet 斷線後兩次重連沒有回應
  （各等 5 s），第三次改連家用 Wi-Fi 那一側的 host 位址，1 s 內連上；從斷線到連回共 14.3 s，這段期間影像每 10 s
  仍是 1193～1197 張新影像、零漏幀。
- **還沒驗的**：適配器真的拔掉／訊號掉光時（沒有路由而不是沒有回應的那條分支）；換回 session 位址的那條分支。

**注視點編碼（§VR-FOVEA，2026-10-08；client 設定、預設關閉）**

- 目的：串流每眼 1152、算圖 2160 時，每個編碼像素要代表 1.9 個算圖像素，正前方也一樣糊。注視點編碼把編碼的像素
  多分給正前方、少分給邊緣：編碼尺寸、位元率、解碼負擔都不變，正前方的清晰度接近算圖解析度。
  固定以「正前方」（每隻眼 tangent 0 的位置）為中心，不追蹤眼球。
- 做法（每軸各自、可分離）：以正前方為 0、每一側各自正規化成 e ∈ [−1, 1]，
  `t = a·e + (1−a)·e³`（t 是均勻取樣時的座標），`a = 100 / 強度%`。正前方的斜率是 a（強度 200% 時同樣的視角佔 2 倍
  像素），邊緣的斜率是 3 − 2a（200% 時邊緣只剩一半）；斜率等於 1 的位置在 e ≈ 0.58，換成視角約是正前方 ±25° 以內
  比原本清楚、以外比原本糊。
- **host**：SteamVR driver 的合成器（`compositor_ps.hlsl`）對每個輸出像素先套這條式子再去取遊戲的貼圖。同一次改動
  順手把縮小取樣改成四點平均（一個輸出像素涵蓋超過一個來源像素時才分開取，不然等於原本的單點）——算圖尺寸大於
  串流尺寸時原本只取一點，細線會閃；這一項不論有沒有開注視點編碼都生效。
- **頭盔**：解碼出來的影像先轉成 RGB（`m_Full`），再用一個 pass 解三次式（Cardano＋一次牛頓法，float 誤差約 1e-7）
  還原後寫進 swapchain（`XrVideo::renderUnwarp`）。swapchain 放大成 `min(強度, 200%)` 倍、每眼最高 2304，還原後正前方
  的細節才有地方放。layer 的視角與姿態都不變。和補幀一起開時，還原併進補幀的最後一個 pass；位移估計在壓縮過的
  影像上做，頭部轉動的整張位移依每個位置的斜率換算。
- **協商**：client `/launch` 帶 `vrFovea=<強度%>`（101–250；關閉時不帶），真的 driver 才會在 `<VipleStreamVRSession>` 回
  `fovea=<強度%>`，client 看到才還原（舊 server 不認得→不回→不還原）。`/resume` 沿用進行中那次編排的強度（driver 的
  設定只在編排開始時寫一次），回應照實際的值。
- 設定：`--vr-foveation <100–250>`（保存為 `vrfoveation`）、設定頁「正前方更清晰（注視點編碼）」（關閉／1.25x／1.5x／2x）。
- 驗證：
  - host 送出的串流（`<dev-client>` 用 `--vr-emulate`＋`--dump-bitstream` 抽一幀）：強度 200% 時正前方的選單放大、
    邊緣壓縮，上下左右沒有顛倒。
  - 頭盔無人場次（每眼 1152、120 Hz）：強度 200% 時 swapchain 4608x2304，還原後的畫面和沒開時幾何一致
    （`--xr-dump-frame`）；10 s 1200 張、零漏幀；影像 render thread 等 GPU 的時間中位數 0.9 → 1.75 ms（p95 1.9 ms）。
    和半速串流＋補幀一起開（強度 150%、swapchain 3456x3456）：直線還原後是直的、合成那一格正常；GPU 中位數
    2.75 ms（p95 4.1 ms），9226 幀漏 1 幀。
  - 單元測試：`vrFovea` 的解析、範圍、round-trip、協商回應（`test_vr_params.cpp`）。
- **還沒驗的**：有人配戴時看起來如何（正前方是不是真的比較清楚、邊緣的糊會不會被注意到、轉動眼球看邊緣時的感受）；
  哪個強度當預設；每眼 1440 以上時頭盔 GPU 的負擔。清晰度沒辦法從縮小的截圖判斷，要請使用者戴眼鏡看。

**10-bit 編碼（§VR-10BIT，2026-10-08）：host 端做好了，Frame 的解碼路徑還不行**

- 原本 VR session 一律 8-bit：client 在 PCVR 不列 10-bit 的格式、server 把 SDP 的 `dynamicRange` 覆寫成 0、VR 的編碼器
  探測不測 10-bit。Steam 內建串流用的是 10-bit。
- **host**：VR 探測多測一項「HEVC 10-bit、來源是 display_vr」（`[VIPLE-VR-ENC] probe: HEVC 10-bit supported`）；
  RTSP ANNOUNCE 時 client 要求 10-bit、codec 是 HEVC、而且探測通過才用 10-bit（SDR，HEVC Main10），否則照舊 8-bit。
  `<dev-client>` 用 `--vr-emulate --vr-10bit` 驗過：送出來的串流是 `Main 10`／`yuv420p10le`，顏色正確。
- **client**：`--vr-10bit`（保存為 `vr10bit`，預設關）。PCVR 要不要 10-bit 只看這個設定，不看一般串流的「HDR」
  （HDR 開著不會讓 PCVR 變 10-bit）。
- **Frame 不行的原因（頭盔實測）**：iris 解碼器（`/dev/video-dec0`）列出的輸出格式只有 8-bit 的
  （`Q08C`、`NV12`、`NV21`、`AB24`、`QC24`），FFmpeg 的 v4l2m2m 對 Main10 的串流照樣要求 `NV12`。測試幀解得過，
  但真的串流第一幀進來時 dmabuf 匯入失敗（libplacebo：`shared_mem.stride_w >= params->w` 不成立），接著 client
  SIGSEGV 結束。頭盔本身沒事，重開 client 就好。
- 所以目前在真的頭盔（Linux arm64、不是 `--vr-emulate`）上 `vr10bit` 不生效：照舊要求 8-bit，log 一行
  `[VIPLE-VR-SESSION] 10-bit was requested (vr10bit) but this device's hardware decoder path only outputs 8-bit`。
  設定頁沒有放這個選項。
- 要讓 Frame 用 10-bit，得先弄清楚 iris 在 10-bit 串流的 source change 之後給哪些輸出格式（P010 或 Qualcomm 的
  壓縮 10-bit 格式），再改 FFmpeg 的 v4l2m2m 與 dmabuf 匯入——是自寫 V4L2 解碼器（L1）等級的工作，排在後面。
  另外，dmabuf 匯入失敗不該讓 client 崩潰，這個要另外修。

**VR 設定進 GUI 設定頁（2026-10-08）**

- 設定頁最下面多一組「VR 串流（Steam Frame）」：每眼解析度（1152／1440／1728／2160）、額外視角（關閉、2～5°）、
  延後挑影像（關閉、3／5／6 ms）、注視點編碼（關閉、1.25x／1.5x／2x）、半速串流、補幀（勾了半速串流才能勾）。
  對應保存的 `vreyewidth`／`vreyeheight`、`vroverscandeg`、`vrlatchdelayms`、`vrfoveation`、`vrhalfrate`、`vrsynth`。
- 從 GUI 啟動的 VR 串流是另一個行程（§VR-LAUNCHER），讀的就是這些保存的值，所以在頭盔裡改完直接開下一場就能比較，
  不必再用 SSH 帶 CLI 參數。CLI 參數照舊可用，而且一樣會被存下來。
- Windows 與 arm64 都建置通過、qmllint 沒有語法錯誤；**版面還沒有人實際看過**（頭盔裡與桌面各看一次）。

**host 停在登入／鎖定畫面時整片單色（2026-10-07 深夜）**

- host 重開機後沒有人登入，PCVR 的每一幀都是完全相同的藍綠色（解出來 Y=103 U=139 V=64）。host 的 log 全部正常，
  只有 `[VIPLE-NVENC] bitstream_size` 平均約 210 B 看得出畫面沒有內容；一般桌面串流不受影響。使用者一登入就恢復。
- 從 SSH 看不出這個狀態（`quser` 顯示 console Active，explorer 與 Steam 都在跑）。
- **§VR-HOST-LOCKED（2026-10-08）**：VR `/launch` 的 PRECHECK 用 `WTSQuerySessionInformationW(…, WTSSessionInfoEx)` 看主控台
  session 的 `SessionFlags`，是 `WTS_SESSIONSTATE_LOCK` 就不啟動，回 STATE ERROR `VIPLE_VR_STATE_CODE_HOST_LOCKED`（22）；
  client 顯示「主機停在 Windows 的登入或鎖定畫面，請先在主機上登入」。舊 client 不認得 22，顯示成一般錯誤。
  頭盔無人場次確認：host 已登入時不會誤判，session 照常（MTP、漏幀與之前相同）。
  **鎖定狀態下會不會真的擋下來還沒驗**（要有人把 host 鎖起來再連一次）。
- 查的方法：`<dev-client>` 用 `--vr-emulate` 連 host、`--dump-bitstream` 存下串流，用 ffmpeg 抽一幀看。

**host 擷取端的 fence 晚到：遊戲的 GPU 負載一重，舊規則就丟影像、再重就整場黑畫面（§VR-FENCE-LATE，2026-10-09；server `vr_capture_late`，預設關閉）**

事故（2026-10-09 的配戴場次，桌球遊戲）：

- 使用者在遊戲的圖形設定把超取樣調到最高（遊戲 log：算圖倍率 1 → 2），每眼算圖尺寸由 2164 平方變成 4328 平方
  （host 的 `vr_render_scale_pct = 188` 把建議尺寸放大到 2164，遊戲自己的 2 倍才會落在 4328；SteamVR 的設定沒有變）。
  17 毫秒後 driver 收到新的貼圖組。
- 從那一刻起，server 擷取端每一張影像等 driver 的 fence 都逾時（120 Hz 的上限是 5 ms）：每 10 秒 `copied=0`、
  `fenceTimeout` 約 620。「連續逾時滿 1 秒就拆線」→ driver 0.5 秒後重連 → 新的 generation 第一張又逾時 → 再拆，
  共 39 次、週期約 1.52 秒，頭盔全黑。遊戲本身仍有每秒 95～99 幀。
- 拆線完全沒有幫助，還讓使用者沒辦法自救：driver 每次斷線進 standby 半秒（頭盔與控制器的姿態失效、按鍵放開），
  頭盔每秒只收到約 10 張黑幀或重送幀，遊戲選單改不回來，最後只能從 Steam 的介面把遊戲關掉。
  那個設定存進了遊戲的設定檔，但事後實測它在遊戲啟動時不會套到眼睛貼圖（設定檔是最高、啟動後每眼仍是 2164，一次），
  要在遊戲裡當場調才會變大，所以重開遊戲不會一開場就重演；設定檔已改回使用者原本的值。

根因：

- **fence 是「晚到」，不是「不來」。** driver 每一張都有合成、有 Signal、沒有任何錯誤。同一條連線只要使用者打開 SteamVR
  儀表板，就立刻恢復每秒 120 張、逾時 0——四段沒有循環的時間，和儀表板開著的四段時間完全重合。
- **只有「遊戲貼圖直接當第 0 層」的直通狀態會晚到。** 直通時 driver 的合成排在遊戲這一幀的 GPU 工作之後，等 fence 等於在等
  遊戲這一幀算完：2164 平方時約 2.4 ms（已經是上限的 48%），4328 平方時每一張都超過 5 ms。儀表板開著時第 0 層是合成器自己
  的貼圖，和遊戲的幀脫鉤，同樣的 GPU 負載下一次都沒有逾時。那一場實際晚多久沒有量到（正式路徑不記完成值），只知道大於 5 ms
  而且都有做完；GPU 端確切的排程機制也還不知道。（同樣的負載在 2026-10-10 重量：fence 等待 p50 8.4～8.6 ms、
  `presentToFence` 最大約 10.6 ms，見下面的實測。）
- 不是 10-09 凌晨那批 latch 改動造成的：1 秒規則是 09-28／09-29 寫的，5 ms 的上限是 §VR-FENCE-WAIT 由固定 2 ms 放寬後的值
  （10-07 提交）。
- **以前的無人驗測幾乎都沒有走到這條路。** 沒有人戴的場次（開發機模擬頭盔、Monado、頭盔放著）host 的 SteamVR 儀表板多半
  整場開著，第 0 層是合成器的貼圖；少數幾場儀表板自己關了、走的是直通，但都是正常負載（等待約 2.4 ms），沒有踩到 5 ms。
- 過載時別的控制迴路也被帶偏：那一場 LATCH v2 的 ppm 頂到 +200、積分累積到 +118。

事後重現（2026-10-09，`<dev-client>` 當模擬頭盔連 `<host>`，server 還是舊行為）：

| host 的狀態 | 每 10 秒複製 | 每 10 秒逾時 | evtToPush | 連線 |
|---|---|---|---|---|
| 儀表板關（直通），每眼 2164 平方、每秒 120 幀 | 1200 | 0 | p50 0.2 ms | 正常 |
| 儀表板關（直通），每眼 4328 平方（GPU 壓滿，遊戲約每秒 107 幀） | 0 | 約 700 | — | 40 秒內拆線重連 27 次（和事故相同） |
| 儀表板開，遊戲同樣每眼 4328 平方（約每秒 91 幀） | 1200 | 0 | p50 1.4／p95 3.1 ms | 正常 |

**無人驗測要走直通，得先讓 host 的儀表板不出現**：場次前暫時在 `<host>` 的 `steamvr.vrsettings` 加上
`dashboard.enableDashboard=false`，結束後還原（細節與理由見下面「無人驗測和實際遊玩不一樣的地方」）。client 另有 dev 參數
`--vr-synthetic-menu-at <秒>`（在那一秒按一下左手 MENU，host 當成系統鍵），但它只是切換、關掉之後的時序也和配戴不同，
不建議用來驗測（上表的直通兩列就是用它關的，所以 evtToPush 只有 0.2 ms；和配戴相同的時序是約 2.3 ms）。凡是和擷取、fence、
host 管線延遲有關的無人驗測都要先停用儀表板；儀表板開著量到的 evtToPush 不能代表實際遊玩
（`log_tags.md` 的健康門檻已改成有條件的說法）。

修正（§VR-FENCE-LATE；只改 server，driver、IPC 的版面、client、線上協定都沒有動；設定見 `Sunshine/docs/configuration.md`）：

| 項目 | `vr_capture_late = disabled`（預設） | `vr_capture_late = enabled` |
|---|---|---|
| 第一段等待（`fence_wait_ms`＝700 ÷ 幀率取整數毫秒、夾在 2～6 ms：120 Hz 是 5 ms，100 Hz 以下是 6 ms）內到值 | 複製 | 複製（和關著時走同一條路徑） |
| 第一段逾時 | 丟這一張 | 繼續在 CPU 端等同一個 fence 值，到值就照常複製（計入 `late`）；從被叫醒起累計超過等待預算才丟 |
| 等待預算 | — | 3 個發布間隔（driver 相鄰兩張的 present 時刻差，指數平均），不低於 3 個顯示週期（120 Hz 是 25 ms）、不超過 `vr_capture_late_max_ms`（預設 100） |
| 拆線 | 連續逾時、距第一次逾時滿 1 秒 | 卡死（完成值 3 秒沒有增加）或餓死（連續沒有一張複製成功、有影像在來的時間累計滿 5 秒），而且這段期間 driver 發布了至少 30 張 |
| 拆線的退避 | 沒有（driver 0.5 秒後重連，可以每 1.5 秒一次） | 下一次最早 5、15、30 秒之後（之後都是 30 秒）；連續 10 秒健康才歸零；階梯跨得過 display 的重建與 60 秒內接著開的下一場 |
| 過載旗標 | 恆為關 | 最近 1 秒「晚到＋放棄」≥ 30% 設起來、低於 5% 持續 2 秒才清；設著時 LATCH 樣本不進頻率鎖（輸出只留已經學到的時鐘差）、§VR-PREDICT 不取樣 |
| 沒有影像可交的期間（每一張都放棄、過期…） | 連續逾時 1 秒就拆線，之後每 100 ms 檢查一次停止（每一張都過期、或 ring 開不起來時，舊路徑不會檢查停止——既有的洞，見 `docs/TODO.md`） | 最長可以到退避的 30 秒；每 100 ms 讓擷取執行緒檢查一次停止，session 才結束得了 |

- 全程在 CPU 端確認 fence（`GetCompletedValue()`／`SetEventOnCompletion`），到值之後才下 CopyResource——「不在 GPU 上等對方行程的
  fence」這條不變式沒有變。等待每 2 ms 醒來一次，generation 被換掉就不再等。
- 準時到值的那條路徑，開關開或關都一樣：交出影像之前只多讀一次時鐘；新的量測樣本與判定用的整數運算都放在交出影像之後。
- 擷取執行緒只有在交影像（或專門的檢查）時才看得到「該停了」。卡死／餓死的退避期內每一張都放棄，如果這段時間完全不檢查，
  session 結束時 server 會卡在等擷取執行緒、10 秒後被自己的 Hang detector 中止——所以開關開著時，所有不交影像的路徑都會每
  100 ms 檢查一次（開關關著的路徑沒有動：逾時的情形本來就是 1 秒內拆線；過期與 ring 開不起來的情形仍然停不下來）。
- 仍然每次取最新發布的那一張（`read_latest()`）；影像的時間戳仍取自 driver 的 Present 時刻，所以多出來的等待會如實算進
  `presentToFirstPkt`、頭盔的 hostLatency 與 MTP。等完一張之後迴圈自然拿到當時最新的一張。
- 修正之後預期的樣子：遊戲把 GPU 壓滿時 `copied` 跟著遊戲實際的幀率走（「事後重現」那張表第二列的遊戲約每秒 107 幀）、`late` 接近
  `copied`、`fenceTimeout` 與 `teardownReq` 是 0、沒有 `DRIVER_LOST`——變慢但不斷。直通時合成器的 Present 節拍跟著遊戲走，
  所以 server 最多只收得到遊戲實際的幀率，不會是 120。2026-10-10 在 `<host>` 上量到的就是這個樣子（見下面的實測）。
- 開關開或關都多了量測：`[VIPLE-VR-CAP] 10s` 行尾的 `late`／`fenceWait`／`teardownReq`／`overload`／`budgetMs`／
  `presentToFence`／`overPeriod`、進擷取迴圈時印實際設定的一行、每個 generation 第一次逾時的一行、每次要求拆線的一行
  （從 `sinceAdvanceMs` 與 `sinceCopyMs` 就能分辨「晚到」與「不動」）。**「發布後實際晚多久」要看 `presentToFence`**：
  `fenceWait`（被叫醒 → 到值）在晚到超過一個發布間隔時會貼在發布間隔上，不代表只晚那麼多。欄位說明在 `log_tags.md`。
- **驗證到哪裡**：判定邏輯（等待預算、卡死、餓死、退避、過載旗標）與 LATCH 的 hold 入口有單元測試；擷取迴圈「卡死時仍然
  每 100 ms 檢查停止」只用模型推演過；`<host>` 上的實測（四個負載、開關開與關）在 2026-10-10 做了，見下面。
  **預設關閉**，等有人配戴驗過才改預設。
- 開關關著時遇到的避開法：從 SteamVR 的介面把遊戲關掉再開（設定檔裡的超取樣在啟動時不會套用），不要在遊戲裡把它調高。
  打開 SteamVR 儀表板的期間串流是正常的，但那時操作不到遊戲內的選單，只能用來確認連線還活著。
- 已知的限制：
  - fence 晚到比等待預算長很多時（遊戲慢到一幀要好幾百毫秒，或 GPU 整個停住）放棄的會佔多數；連續 5 秒沒有一張複製成功
    就以餓死拆線，之後依退避最多每 30 秒一次；頭盔端這時還不會顯示原因（下一輪做）。
  - **過載旗標的「晚到」是第一段等待逾時（120 Hz 是 5 ms），這個定義太寬。** fence 等待穩定落在 5 ms 與一個顯示週期之間、
    仍然跑滿更新率的遊戲，每一張都算晚到：旗標整場設著，LATCH 只照已經學到的時鐘差走、§VR-PREDICT 不追蹤。2026-10-10 量到了
    （見下面的實測）：解析度倍率 300% 時遊戲仍是每秒 120 幀、擷取端每一張固定晚約 6 ms，旗標整場設著。那種狀態下的相位樣本
    推論是可用的（影像取模一個週期之後只差 1 ms 上下），但還沒有讓 LATCH 在這個負載實際追蹤過；§VR-PREDICT 該不該取樣則沒有
    任何實測（300% 時 `<host>` 送出的時刻是差一個週期的雙峰，見實測）。
  - **旗標該表達的是「Present 鎖不住虛擬 vsync」。** 350%（每秒約 118 幀）與 400%（約 106 幀）時發布節拍慢於顯示節拍，相位樣本
    會掃過整個週期，那才是該停的時候（同樣是推論：350% 沒有頭盔場次，400% 的頭盔場次旗標設著、樣本沒有進控制器）。原本想的
    兩個收窄方式（等超過一個顯示週期才算；晚到而且發布間隔明顯變長）在 350% 都不會設旗標（`overPeriod` 只有 12～17%、節拍只慢
    約 1.5%）。也不能用「最近 1 秒發布的張數」：每秒 120 幀在 1 秒的視窗裡是 119～121 張、118.4 幀是 117～120 張，兩者重疊
    （現在 `vr_capture_health.h` 的視窗實際只涵蓋 0.9～1.0 秒，穩定 120 幀也會讀到 108～119）。driver 已經在送分得出來的量：
    每 10 秒跳過的虛擬 vsync 數（`[VIPLE-VR-DRV] timing 10s: presents=…` 那一行的 `missed`：100%／300% 是 0，350% 是 9～22，
    400% 是 134～191）與 Present 落在格內的位置（`presentPhase`）。
  - **定案與實作（§VR-CADENCE，2026-10-10；`vr_capture_overload = late | cadence`，預設 `late`＝上面的舊定義）**：新的訊號是
    driver 的 timing ring 裡每一幀「這次 Present 跨了幾個虛擬 vsync 格」（準時是 1）。bridge 執行緒上的節拍偵測器
    （`Sunshine/src/vr/vr_present_cadence.h`，純標頭）把它歸成 locked／slip／unsure／unknown：10 秒內 4 次單格滑動就是 slip
    （最後一次之後 12 秒才清）、3 秒內滑掉十五分之一以上是重度（結束後 3 秒清）、卡頓與它之後 3 秒的餘波不算但也不敢說 locked、
    剛連上要看滿 10 秒。三件事的結論：(1) 被 SteamVR 固定在整數分之一而且 driver 說是節流 → 算鎖得住；(2) 臨界負載不來回切換——
    設起來就留 12 秒；(3) LATCH 與 §VR-PREDICT 分開判：cadence 模式下 LATCH 只在「鎖不住」（或放棄的影像太多）時停，偵測器說不準
    時照舊旗標；§VR-PREDICT 仍照舊旗標（晚到就不取樣）。預設模式（`late`）兩個決定和之前逐步相同（差分測試約 2,000 萬步 0 個
    不同），偵測器照算、照印（影子資料）。另加診斷 `lockR`（`[VIPLE-VR-LATCH] 10s` 行尾：相位樣本的一致性）。
    實測（2026-10-10 下午，AA 3）：

    | 負載 | 模擬頭盔、預設模式：舊旗標／偵測器（每 10 秒滑動數＝driver 的 `missed`） | 模擬頭盔、cadence 模式：擋 LATCH／擋 PREDICT |
    |---|---|---|
    | 100% | 0／locked（0） | 否／否 |
    | 300% | 1（每 10 秒晚到 750～850 張）／locked（0） | **否**／是（過載設起來 12 秒後放開 LATCH） |
    | 340% | 1／slip、臨界（4～9；兩簇之間最久 4.4 秒） | 是／是（13 秒內判定） |
    | 350% | 1／slip、臨界（20～24） | 是／是（3 秒內） |
    | 400% | 1／slip、重度（136～143） | 是／是（2 秒內） |

    真頭盔無人場次、300%、同一時段交替各 150 秒：舊規則 LATCH 整場被擋（`overloadHeld=100`、週期修正釘在 −19 ppm），
    **重複顯示 12.4%、`late` 每 10 秒 79 張；cadence 模式 LATCH 照常追蹤（`overloadHeld=0`、修正在 −178～+138 ppm 之間），
    重複顯示 2.8%、`late` 18 張**。`lockR` 0.20～0.68（追蹤中相位有約 ±1 ms 的擺動，來自編碼段的雙峰）。
    300% 時 `<host>` 送出的時刻仍是雙峰（`presentToFirstPkt` 中位數 8 ms、多數視窗的 p95 是 16 ms），但 LATCH 追得上之後
    重複顯示已經和輕負載同一個水準（同一時段輕負載是 2.7%）。linux-builder 上 server 建得起來，正式的測試執行檔
    （`test_sunshine`）372 個 VR 測試全過。
    **還沒有人配戴驗過；配戴驗過才檢討 `vr_capture_overload` 與 `vr_capture_late` 的預設。** 已知的限制：臨界的門檻來自
    10 秒彙總與重建的序列；由鎖得住升到臨界負載有 1～7 秒照常取樣的空窗；不到約 1.7 秒的短過載每次要 12 秒才清；
    只量過一個遊戲、120 Hz、一張顯示卡。

**§VR-FENCE-LATE 在 `<host>` 上的實測（2026-10-10；`<dev-client>` 當模擬頭盔，另有兩場頭盔無人場次；`<host>` 的儀表板整場停用——
和 10-09 配戴那一場同一種狀態，比過的範圍見表後第一點）**

遊戲的負載用 SteamVR 對這個遊戲的解析度倍率調（面積百分比；100% 是每眼 2164）。數字是遊戲載入之後各個 10 秒視窗的範圍：

| 倍率（每眼） | 遊戲每秒幀數 | fence 等待 p50／p95 | Present → 第一個封包 p50／p95（開關開） | 開關關（舊規則） | 開關開 |
|---|---|---|---|---|---|
| 100%（2164） | 120 | 2.3／2.5 ms | 4.5／4.9 ms | 沒有另外跑；修正前使用者配戴的場次就是這個負載：每 10 秒複製約 1200 張、逾時 0～5 | 每 10 秒複製 1200 張；`late=0`、`overload=0`、`budgetMs=25` |
| 300%（3748） | 120 | 5.7／6.3 ms | 8.0／16.7 ms | **每 10 秒只複製 400～490 張、逾時 710～800 張**；沒有拆線 | 每 10 秒複製 1200 張，其中 600～740 張算晚到；`fenceTimeout=0`、`overPeriod` 0～1 |
| 350%（4048） | 118～119 | 7.1～7.5／8.6～8.8 ms | 16.2～16.9／19.4～19.7 ms | **51 秒拆線 33 次**，幾乎沒有一張複製成功 | 每 10 秒複製約 1180 張，九成七算晚到；`overPeriod` 佔 12～17% |
| 400%（4328） | 105～109 | 8.4～8.6／9.5～9.7 ms | 17.1～17.5／21.2～21.5 ms | 24 秒拆線 17 次（和 10-09 的事故相同）；拆線那一行印出 `completed` 只比 `expected` 少 1、`sinceAdvanceMs=0`——fence 一直在前進，只是每一張都晚 | 每 10 秒複製約 1060 張（等於遊戲的幀率），全部算晚到；`presentToFence` p50 8.7、p95 9.8、最大 10.6 ms；`overPeriod` 佔 55～63% |

開關開的四列都沒有拆線，串流都正常結束。

頭盔無人場次（經 Tailscale、延後挑影像 6 ms、頭盔的選單關掉、開關開）：

| 倍率 | `<host>` | 頭盔 |
|---|---|---|
| 100%（對照；下面「延後挑影像」那張表的同一組場次） | `late=0`、LATCH 正常追蹤；Present → 第一個封包 4.5／4.8 ms | 重複顯示 3.4%；MTP p50 約 50 ms |
| 300% | 每 10 秒複製 1200 張，其中 930～1010 張算晚到（等待 p50 6.1～6.2 ms）；`overload=1`；`[VIPLE-VR-LATCH] 10s` 是 `overloadHeld=100`、`ppm` 等於 `integ`（只照學到的時鐘差走）；Present → 第一個封包各視窗的 p50 在 8.5 與 16.2 ms 之間跳、p95 約 17 ms | 重複顯示 7.2%（啟動 30 秒之後的 11 個視窗，每 10 秒 44～176 格）；MTP p50 各視窗在 54～57 與 61～65 ms 之間跳 |
| 400% | 每 10 秒複製約 1010 張；`overPeriod` 佔 75～82%；`overloadHeld=100`；Present → 第一個封包 18.4～19.3／21.8～22.0 ms | 每 10 秒顯示 720～850 張新影像（其餘重複或沒顯示就被下一張蓋掉），MTP p50 68.6 ms |

- **「和配戴相同」比過的範圍**：`<host>` 端三個量（evtToPush、Present 落在格內的位置、echoAge），在正常負載、對一場配戴
  （10-09 下午）、一個遊戲。沒有對照到的：(1) 100% 以上的負載——同樣每眼 4328，配戴那一場遊戲是每秒 95～99 幀，無人場次是
  105～109，上表的倍率搬到配戴時可能提早踩到；同一個倍率在模擬頭盔與頭盔無人場次之間，等待也差 0.4～0.6 ms；(2) 配戴中途
  開過再關儀表板之後落在哪一種時序；(3) 無人場次 §VR-PREDICT 整場沒有動作，配戴那場有探測與追蹤。
- **舊規則在遊戲還跑滿更新率的時候就開始丟影像，再重一點就全黑。** 直通時 fence 等待在正常負載就有 2.3 ms（上限 5 ms 的將近
  一半），而且跟著遊戲每一幀的 GPU 負載上升：300% 時遊戲仍是每秒 120 幀，舊規則丟掉六成多的影像（client 每秒只收到 40 多張，
  沒有任何提示）；350% 時遊戲每秒 119 幀，舊規則已經是拆線循環。**踩到的條件是「Present 之後還要等的 fence 時間超過第一段
  等待」（120 Hz 名義上 5 ms，實際等到 5.0～6.8 ms 才放棄），不必等到遊戲掉到更新率以下。** 這段等待不等於遊戲整幀的 GPU
  時間：遊戲的 GPU 時間沒有直接量，由 350%／400%（幀時間約 8.4／9.4 ms、`presentToFence` p50 約 7.5／8.7 ms）估計，等待比整幀
  少 1 ms 上下——開始丟幀大約在遊戲每一幀的 GPU 時間到 6 ms 上下（120 Hz 一格的七成左右）時。
- 10-09 晚上一度以為「遊戲掉到更新率以下才會踩到」，那是錯的：那一輪是用按鍵把儀表板關掉的（另一種時序：正常負載的 fence 等待
  只有 0.1 ms、evtToPush 0.2 ms；300% 時等待 p50 1.3 ms、零逾時，要到 400% 才踩到）。換成儀表板整場停用重量，才看到上面
  兩張表的結果。
- **fence 之後還有第二段排隊：編碼（表上「Present → 第一個封包」那一欄）。** 負載一重，影像交給編碼器之後也變慢：100% 是
  4.5 ms；300% 是雙峰——多數約 8 ms、其餘約 17 ms，差一個週期；350%／400% 是 16～17.5 ms（p95 19～21.5 ms）。編碼器回報的
  單張時間（`[VIPLE-NVENC] encode_frame total`）在頭盔場次由平均 1.8 ms 變成 300% 的 3.1～5.7 ms、最大 9～17 ms。300% 的頭盔
  場次，各個 10 秒視窗的 p50 在兩邊跳，MTP p50 跟著在 55 與 64 ms 上下跳；頭盔上看到的是「整條晚一格再回位」
  （`[VIPLE-VR-LDIAG]` 的 lateShift 每 10 秒約 35 次，正常負載 4～7 次）。這不是這次的修正造成的（用按鍵關儀表板的 300% p95
  也有 12 ms；儀表板開著、每眼 4328 是 7.5／15 ms）；舊規則在 300% 只收早到的那幾張，所以看不到（p95 8.4～8.9 ms）。雙峰是
  量到的；原因推測是複製與色彩轉換的 GPU 工作排在遊戲下一幀的後面，GPU 的排程沒有直接量。400% 的頭盔場次，`<host>` 每
  10 秒送約 1010 張，頭盔只顯示 720～850 張。
- **第二段排隊的原因找到了：`<host>` 的 server 在 VR 擷取期間 GPU 排程優先權是 HIGH，搶不過遊戲（§VR-GPU-PRIO，2026-10-11）。**
  - 直接讀各行程的 GPU 排程優先權類別（340% 的場次中）：SteamVR 的合成器、vrserver（我們的 driver 在裡面）、桌面視窗管理員
    都是 REALTIME；遊戲是 NORMAL；server 是 HIGH（擷取期間由 REALTIME 降下來的，當初的顧慮是 REALTIME 會搶走遊戲的 GPU）。
    顯示卡的搶佔粒度是像素層級、硬體加速 GPU 排程開著。各行程佔 3D 引擎的比例：遊戲 85%、server 4.7%（另有編碼引擎 16%）、
    vrserver 2.5%、合成器 0.8%。
  - 同一場（340%）從外部把 server 行程在 HIGH 與 REALTIME 之間來回切，各 30 秒、三輪，切過去的那個 5 秒視窗就變、切回來就還原：

    | server 的 GPU 優先權 | 進 NVENC 的兩個呼叫（平均） | 等編碼完成（各視窗最大值的中位數） | `encode_frame` 平均 | `presentToFirstPkt` p50／p95 | 遊戲的 `presentToFence` p50／p95 | driver 每 10 秒出幀／missed |
    |---|---|---|---|---|---|---|
    | HIGH（現行） | 5.05 ms | 9.17 ms | 6.79 ms | 16.77／19.02 ms | 7.07／8.72 ms | 1195／6 |
    | REALTIME | 0.61 ms | 3.55 ms | 2.12 ms | 9.49／10.88 ms | 7.27／8.45 ms | 1195／7 |

    HIGH 和遊戲在同一個排程層級，不會搶佔：server 在 fence 到值之後才送出的複製、色彩轉換與編碼器的前處理，要排在遊戲
    下一幀已經排著的工作後面，等它整幀做完。REALTIME 才會插到前面。
  - 新增設定 `vr_gpu_priority = high｜realtime`（預設 high＝行為不變）。整場用 `realtime` 的負載階梯（各 80～100 秒，同一個
    小時內；`high` 那一欄 300%、400% 是同時段的對照，340% 是切換實驗前的那一場，100% 是前一天下午量的）：

    | 遊戲負載 | `high`：`presentToFirstPkt` p50／p95 | `realtime`：p50／p95 | 每 10 秒送出的張數（high → realtime） | `presentToFence` p50（high → realtime） |
    |---|---|---|---|---|
    | 100% | 4.55／4.9 ms | 4.55／4.9 ms | 1200 → 1200 | 2.5 → 2.45 ms |
    | 300% | 8.1／16.1～16.7 ms（雙峰） | 8.15／8.7 ms | 1200 → 1201 | 6.02 → 6.01 ms |
    | 340% | 16.9／19.0 ms | 9.5／10.9 ms | 1192 → 1194 | 7.07 → 7.3 ms |
    | 400% | 18.1／21.5 ms | 11.4／13.4 ms | 1053 → 1038 | 9.00 → 8.95 ms |

    每一種負載下 `presentToFirstPkt` 都回到「fence 到值＋約 2.1～2.4 ms」，多等的那一個遊戲幀不見了；遊戲的代價在 340% 以下
    看不出來，400%（遊戲本來就掉到約 105 fps）每 10 秒少約 15 張（1.4%）。
  - 耐久：340%、整場 `realtime` 連續 10 分鐘（60 個視窗），`presentToFirstPkt` p50 9.5 ms、p95 11.0～11.8 ms，沒有編碼逾時或卡住的訊息。只有這一場，不能代表顯示記憶體吃緊或更長時間的情況。
  - 真頭盔的無人場次（340%＋反鋸齒 3、全速、`vr_capture_overload = cadence`，各 200 秒，依序 high、realtime、realtime、high；
    2026-10-11 02:10～02:26，經 VPN）：

    | 場次 | `<host>` 的 `presentToFirstPkt` p50／p95 | 頭盔的 MTP p50（各視窗的範圍） | 重複顯示 | `late`／10 s |
    |---|---|---|---|---|
    | high（第 1 場） | 16.92／18.88 ms | 66.6 ms | 15.88% | 107 |
    | realtime（第 2 場） | 9.14／10.72 ms | 60.8 ms（58.8～63.9） | 10.29% | 61 |
    | realtime（第 3 場） | 9.09／10.57 ms | 59.6 ms（58.8～62.9） | 5.66% | 39 |
    | high（第 4 場） | 16.96／18.89 ms | 68.4 ms | 7.82% | 52 |

    `<host>` 這一段少 7.8 ms，頭盔量到的 MTP 少 6～8 ms（MTP 的中位數是 4.17 ms 一階的階梯，只能讀到這個精度）。
    重複顯示這四場之間本身變動很大（那一段時間路徑的狀況在變），相鄰的兩組都是 realtime 較少，但不能當成定量的結論。
  - 還沒做：配戴驗證、第二個比較重的遊戲、更長的耐久。NVIDIA 在硬體加速 GPU 排程＋REALTIME 有編碼凍結的案例
    （`nvenc_realtime_hags` 那個選項的由來），所以預設先不改；`nvenc_realtime_hags` 關著時這個鍵不會把優先權拉到 REALTIME。
- 歷次實測只用過這一個很輕的遊戲（正常負載時 fence 等待 2.3 ms，離門檻還有一倍多）。門檻是絕對的毫秒數（120 Hz 是 5 ms、
  100 Hz 以下是 6 ms），把畫質調到剛好跑滿更新率的遊戲，每一幀的 GPU 時間通常就在這附近，所以推論比較重的遊戲在開關關著時
  一進去就會丟幀或全黑。這是推論：還沒有用第二個遊戲驗，也沒有量過任何遊戲實際的 GPU 時間；前提是 `<host>` 現在的設定
  （120 Hz、每眼 2164）與這張顯示卡。
- 開關開著時四個負載都不拆線，複製的張數等於遊戲實際的幀率，遠好過舊規則（300% 時舊規則丟掉六成多）。頭盔場次 300% 的重複
  顯示是 7.2%（正常負載 3.4%），多出來的每 10 秒約 45 格有兩個來源（`[VIPLE-VR-LDIAG]` 的分類），要分開看：
  - 約三分之二是上一點說的「整條晚一格再回位」（lateShift 約 35 次，正常負載 4～7 次），相位好壞的視窗都一樣多，**和過載旗標
    無關**，改旗標不會好。
  - 約三分之一是「沒趕上自己那一格」（late），集中在開場 30～70 秒：旗標在遊戲開始出幀時設起來（開場第 19 秒左右；那之前
    控制器已經對到目標，`slackEma` 4017 µs 對目標 4167），之後遊戲載入帶來的管線延遲沒有人補，頭盔端 slack 的中位數停在
    0.4～1.0 ms，late 每 10 秒 80～120 張、重複顯示約 11%。這一段後來結束，是因為自動位元率由約 50 Mbps 掉到 5～12 Mbps、
    頭盔解碼與繪製各快約 1 ms，slack 回到約 5 ms——不是控制器的功勞；之後 late 每 10 秒約 20 張（不比正常負載多）、重複顯示
    約 5%。
  只有一場。旗標的定義要改（見上面已知的限制），但改完預期只拿得掉第二項。
- `<host>` 目前的設定是 `vr_capture_late = enabled`。卡死／餓死的拆線與退避只有單元測試與模型，沒有假 driver 可以實測；
  改預設之前要用假 driver 實跑一次（停止 Signal → 拆線 → 在退避期內結束串流，不能觸發 Hang detector）。
- Linux 的 server（`.deb`）在 `<builder>` 上重建過：建置通過，正式測試檔裡 VR 的單元測試 190 個全過（VR 在 Linux 是 stub，
  新的判定邏輯與 LATCH 的 hold 有編進去測）。

**頭盔的「GPU 忙碌期」定案：就是 SteamVR 合成器每一格的那一筆 GPU 工作（2026-10-09／10，頭盔內建的 gpu-trace）**

10-08 配戴實測推測頭盔端有一段和顯示同步的 GPU 忙碌期，撞上去的影像會錯過顯示那一格，但沒有直接證據。10-09 晚用頭盔內建的
gpu-trace（kernel 事件：vblank、每一筆 GPU 工作的送出與完成、佇列切換、排程，加上 SteamVR 合成器與 Mesa 自己寫的標記）擷取了
六次：無人、經 Tailscale 從 `<host>` 串流、120 Hz、每眼 1152、注視點 1.5x、延後挑影像 5 ms。四次是頭盔自己的選單開著
（我們的程式停在 VISIBLE），兩次是選單關掉、程式有焦點（FOCUSED）。三路分析加一路獨立查核，關鍵數字重現到 0.01 ms 以內。

一個顯示格（8.33 ms）裡的時間表（相位＝距這一格開頭那次 vblank 的毫秒數，中位數）：

| 相位（ms） | 發生什麼 |
|---|---|
| 0.25 | runtime 放行 `xrWaitFrame`；我們的 XR frame thread 醒來，接著睡「延後挑影像」的毫秒數 |
| 2.7 | 合成器讀取應用程式交來的影像（推論；範圍 2.2～3.2），開始組這一格 |
| 3.5 | 合成器向 GPU 提出要求。**從這一刻起我們的 GPU 工作不能開始** |
| 4.0～5.0 | 合成器的工作在 GPU 上執行：頭盔選單開著 1.03 ms、關掉 0.92 ms；使用者 10-09 下午配戴時 runtime 自己統計 0.93 ms |
| 5.1～5.25 | GPU 切回來，我們的工作最早能開始的時刻 |
| 5.4 | 延後 5 ms 時我們挑影像（latch）的時刻；延後 6 ms 是 6.4 |
| 5.6～5.75 | 合成器把新畫面交給顯示硬體，等 8.33 的 vblank 換頁 |

- 合成器（`vrcompositor` 的 RenderThread）每個顯示格恰好送一筆 GPU 工作，用優先權最高的佇列（ring 0）；我們在 ring 2。
  兩千六百多個顯示格沒有缺、沒有一格兩筆。頭盔的選單開或關，次數與位置都一樣，選單只讓它多約 0.1 ms。
- 我們每張影像是三筆 GPU 工作：解碼影像轉 RGB 0.32 ms、注視點還原 0.79 ms、交還 0.01 ms，共 1.13 ms（GPU 時脈 578 MHz 時；
  時間和時脈成反比）。client log 的「等 GPU」中位數 1.53 ms＝執行 1.13＋GPU 從省電狀態醒來 0.32＋kernel 與執行緒接力 0.08。
  GPU 只要閒約 0.33 ms 就斷電，叫醒要 0.32 ms，算在叫醒它的人頭上。
- 把影像交給 GPU 的時刻落在相位 2.0～5.2 ms 就會撞上合成器：不是排在它後面、就是做到一半被它插隊，多等 1.4～1.55 ms
  （最多 1.9 ms；10-08 估的 2 ms 偏大），做完的時刻落在 4.8～6.8 ms。等超過 2.2 ms 的影像全部是撞上的，沒有例外。
  延後 5 ms 時 latch 在 5.4 ms，被擋的影像幾乎都錯過這一格——這就是 10-08 看到的「latch 前 0.47～1.89 ms 沒有樣本」。
- 撞上的比例取決於影像到達的相位（網路抖動、host 的相位控制），每一場不同；機制、相位窗與每一次的代價可以帶到別場。
- 我們也會拖到合成器：它提出要求時我們正好在畫的那幾格（3～8%），它晚約 0.5 ms 才開始，之後會把自己的排程往前挪。
  所以**不要提高我們的 GPU 優先權**——那等於讓我們的 1.1 ms 擋在系統合成器前面。

延後挑影像 5 ms 對 6 ms 的對照（無人、經 Tailscale、頭盔的選單關掉且程式有焦點、新版 client 的 `[VIPLE-VR-LDIAG]`；每場約
140 秒，取啟動 30 秒之後的 10 個視窗）。`<host>` 的儀表板有兩種關法，時序不同（見下一節），兩種都跑了：

| `<host>` 的儀表板 | 延後 | 重複顯示（佔顯示格數，兩場） | late／heldMiss（每 10 秒，兩場的範圍） | runtime 重投影（兩場；整場／其中啟動之後） | MTP p50 |
|---|---|---|---|---|---|
| 整場停用（和 10-09 配戴那一場同一種狀態） | 5 ms | 6.5%、5.1% | 30～79／19～78 | 2、1 格／1、0 格 | 49.4～53.6 ms |
| 整場停用 | 6 ms | 3.4%、3.4%（第二場後 80 秒 2.6%） | 18～84／0～11 | 5、4 格／3、3 格 | 49.9、51.4 ms |
| 開著、中途用按鍵關掉 | 5 ms | 3.6%、2.6% | 7～61／6～54（另有第一個視窗含按鍵與關選單的瞬間，heldMiss 到 283） | 3、1 格／1、0 格 | 53.9～58.0 ms |
| 開著、中途用按鍵關掉 | 6 ms | 1.9%、1.3% | 7～24／1～10 | 0、3 格／0、3 格 | 54.1～55.0 ms |

（重複顯示取啟動 30 秒之後的 10 個視窗；第一個視窗有六、七秒還在關選單之前。runtime 的重投影很少，但不是都在啟動的頭一百格：
啟動之後，5 ms 的場次是 0～1 格、6 ms 的場次是 0～3 格（連同頭盔選單開著的兩場共六場，五場是 2～3 格），約佔 1.6 萬格的
0.02%——6 ms 多的這幾格配戴時要一起看。「整場停用、5 ms」的第二場另有一個 10 秒視窗（第 83～93 秒）因網路中斷到 42%，
沒有計入；算進去那一場是 8.8%。使用者 10-09 下午實際配戴、延後 5 ms 的那一場是 6.1%（同樣去掉一個因網路中斷到 43.5% 的視窗；算進去是
8.1%），和「整場停用、5 ms」的無人場次接近。頭盔選單開著時另外量過 6.5 ms 一場：1.2%（那一場的選單在第 86 秒自己關了），
runtime 重投影 2 格。這些場次和使用者下午配戴的那一場，頭盔的系統追蹤（全系統的 perf 取樣）都開著——那個設定開機後就在跑，
每次啟動 VR 程式也會把取樣重新開起來（頭盔上 gpu-trace 服務的 log 有每一次的紀錄）；條件相同所以可以互相比較，關掉之後的
絕對值要重量。）

- 延後 5 ms 時 late（畫好了但沒趕上）大多是 heldMiss（等 GPU 被拖長而錯過），gap（這一格根本沒有影像可用）每 10 秒只有
  個位數到二十出頭。也就是說，經網際網路時重複顯示的主因不是影像沒到，而是影像到了、畫的時候被合成器擋住。延後 6 ms 之後
  heldMiss 剩個位數到十出頭，剩下的 late 主要是真的晚到的（網路抖動，加上 `<host>` 直通時 Present 時刻本身的抖動）。
- 兩種 `<host>` 狀態下 6 ms 都把重複顯示減少四到五成；絕對值在「整場停用」的狀態下高一倍左右。
- 頭盔的保存設定已在 2026-10-10 改成 6 ms（設定頁「延後挑影像」）。有人配戴還沒驗。7 ms 沒試：查核算出那時離 runtime 下一次
  放行只剩不到 1 ms，要 6.0 → 6.5 → 7.0 逐步試，同時看 predictAhead 與 runtime 的重投影；10-08 量過 8 ms 會有 19% 被重投影
  （runtime 自己的統計）。
- 驗收「延後挑影像」要看 runtime 的重投影（合成器的 `Cumulative stats` 或 perfrecording CSV），不能只看 client 的 missed：
  延後 8 ms 那場 runtime 記 19% 重投影，client 自己只看到 0.53%。

還能做的（都要改 client；一次只改一項、同場切換，同時看 repeat／skip、predictAhead、runtime 的重投影與合成器的掉格）：

1. 影像一送進 GPU 佇列就交還給 runtime 並發布，不等 GPU 做完。現在等 GPU 的 1.5～3 ms 整段壓在 latch 之前，而合成器要到下一格
   的 2.7 ms 才讀這張影像，最壞情形 GPU 做完離那一刻也還有 3.4 ms 以上。預期省 1.5 ms 以上、不再怕被插隊；要驗有沒有撕裂或
   閃舊影像，host 的目標 slack 也要跟著重調（「發布」的定義變了）。
2. 把「解碼影像轉 RGB」併進「注視點還原」的 shader，少 0.32 ms 的 GPU 時間（補幀開著時仍走原路）。
3. 我們在 5.5 ms 交出影像，合成器下一格 2.7 ms 才讀，中間白等約 5 ms。用另一條執行緒先呼叫下一格的 `xrWaitFrame`、把交幀挪到
   合成器讀取之前，理論上可以省掉大半；runtime 允不允許這麼晚交，要實驗才知道。
4. 解碼執行緒是每 1 ms 輪詢一次輸出，平均晚 0.2 ms 才取到影像；改成被叫醒可以省掉。

限制：六次擷取都是無人、合成的頭部運動，追蹤時全系統的 perf 取樣也開著（GPU 自己的時間是硬體計數器量的，不受影響）；每次
事件齊全的只有最後 5～6 秒。有人配戴時合成器會不會多做別的事（手部追蹤、邊界、透視）還沒量。

**無人驗測和實際遊玩不一樣的地方（2026-10-09 整理；第 2 項是使用者戴上去看才發現的）**

1. `<host>` 的 SteamVR 儀表板。沒有人戴時它多半整場開著（SteamVR 啟動時自己打開，沒有人按鍵就不關；偶爾也會自己關掉），
   而它的狀態決定 host 走哪一條路，三種狀態的時序都不同（遊戲同樣每秒 120 幀、每眼 2164）：

   | 狀態 | 第 0 層的來源 | Present 落在每格的 | host 等 fence（evtToPush） | 姿態到 Present 的年齡（echoAge） |
   |---|---|---|---|---|
   | 儀表板開著 | SteamVR 合成器自己先合成一次 | 約 1.3 ms | 約 1.0～1.4 ms | 約 2～5 ms |
   | 開著、中途用按鍵關掉 | 遊戲的貼圖（直通） | 約 0.7 ms | 約 0.2 ms | 約 10～12.6 ms（比同一場儀表板開著時多一格） |
   | 整場停用，或像配戴時一開始就關掉 | 遊戲的貼圖（直通） | 約 3.3 ms（p95 約 5） | 約 2.3 ms | 約 5～7 ms |

   使用者實際配戴的場次是第三種（10-09 下午：evtToPush 2.4 ms、echoAge 約 6.2 ms、Present 3.1～3.8 ms）。**無人驗測要用第三種**
   （比過的只有這三個量、正常負載、一場配戴；見「§VR-FENCE-LATE 在 `<host>` 上的實測」表後第一點）：
   場次前把 `<host>` 的 `steamvr.vrsettings` 加上 `dashboard.enableDashboard=false`、結束後還原（要等 SteamVR 結束、server 的編排器
   回到 IDLE 之後才改檔，否則會和它還原設定的動作搶寫）。client 的 `--vr-synthetic-menu-at <秒>`（按一下左手 MENU）只是「切換」，
   儀表板本來關著時反而會把它打開，而且關掉之後是第二種時序，不能代表配戴；host 的 `vrcmd --hidedashboard` 從 SSH 連不上 SteamVR
   （工作階段不同）。確認方式：host 的 `[VIPLE-VR-DRV] layer0 eye=0 src pid= size=` 是遊戲的尺寸，而且 `presentPhase` p50 約 3 ms。
   10-08 以前的無人場次大多是第一種，但不是全部：當時沒有記第 0 層、也沒有控制儀表板，事後從 `<host>` 的 evtToPush 與
   client 收到的震動回饋次數（儀表板開著時每 10 秒十幾到幾十次，關著是 0～1 次）回推，10-08 的 36 場頭盔無人場次有 29 場是
   第一種、4 場是第三種的時序、3 場是別的；模擬頭盔與 Monado 留下來的場次都是第一種。同一批對照裡混著不同狀態，這是
   「無人場次重現不了配戴時的現象」的主要原因之一。
2. 頭盔自己的 SteamVR 選單：從 SSH 啟動的場次，選單整場開在最上面，我們的程式停在 VISIBLE（看得到但沒有焦點），合成器每格
   多畫一層（GPU 多約 0.1 ms）。實際配戴、從選單啟動時，選單約 4 秒就自己關了。無人場次要在串流起來之後用頭盔上的
   `vrcmd --hidedashboard` 關掉，並確認 client log 出現 `session state VISIBLE -> FOCUSED`。
3. 頭盔 Steam 設定裡的「系統追蹤／圖形側錄」開著時，開機後就有一個全系統的 perf 取樣在背景跑（每次啟動 VR 程式也會被重新
   開起來），而且並沒有在錄 GPU 事件
   （gpu-trace 服務在追蹤已啟用時收到 start 只做停止，之後回報「追蹤中」）。驗測前確認它關著；要擷取時自己先 stop 再 start、
   擷取完 stop。追蹤開著時量到的 repeat／skip 只能拿來診斷，不能當驗收成績。
4. 頭盔的系統版本：系統更新會同時換掉系統映像、kernel 與 SteamVR（10-09 上午 SteamVR 由 2.17.10 換成 2.18.2）。跨版本的數字
   不能直接比，每一場要記版本。
5. 場次中途插拔充電線會讓 Steam 重新套用效能設定、Tailscale 重新綁定網路；要比較的場次全程插著或全程不插。

**host 直通時的兩種時序（2026-10-10 查核；上表第二、三列的由來）**

直通（第 0 層是遊戲的貼圖）時，`<host>` 會落在兩種時序之一。driver 被呼叫的順序與次數兩種完全相同（每個 Present 各一次
SubmitLayer／Present／GetNextSwapTextureSetIndex／PostPresent／GetFrameTiming），差別只在 PostPresent 返回的那一刻，
SteamVR 的合成器手上有沒有一張「遊戲上一格已經交、還沒送給 driver」的幀：

| | A：等遊戲這一格 | B：多一格 |
|---|---|---|
| 合成器的動作 | 手上沒有待送的幀，等遊戲交這一格再送 | 手上已有遊戲上一格交的幀，立刻送出；遊戲這一格排到下一格 |
| `presentPhase` p50 | 約 3.3～3.6 ms | 約 0.7～0.8 ms |
| `evtToPush`（等 fence） | 等遊戲這一幀的 GPU 工作（反鋸齒開著約 2.3 ms、關著約 0.9 ms） | 約 0.2 ms（GPU 早就算完） |
| `echoAge` p50 | 約 5～6 ms | 120 Hz 約 10～12.6 ms；60 Hz 約 19～21 ms |
| 姿態到第一個封包（輕負載） | 約 7.8 ms | 120 Hz 約 11.4 ms（多 3.5）；60 Hz 約 19.8 ms（多 12） |
| Present 的相位 | 跟著遊戲交幀的時刻抖（p95−p50 約 1.4 ms） | 鎖在虛擬 vsync 上（約 0.5 ms） |

把握：「一格交接」的結構中高（另有獨立的佐證：SteamVR 自己量的遊戲 CPU 時間在 A、B 場次相同，多出來的一格不在遊戲裡；
`missed=0` 可推得 SteamVR 沒有要求節流）。進出的規則在 2026-10-10 用實驗直接驗過（下一點）。

- **進出規則的實驗（模擬頭盔，60 與 120 Hz 各一場；用 `NtSuspendProcess` 把遊戲行程暫停指定的毫秒數）**：

  | 步驟 | 60 Hz | 120 Hz |
  |---|---|---|
  | 儀表板開著啟動 | 合成器自己合成（`presentPhase` 約 1.5～2.1 ms） | 同左（約 1.2～1.3 ms） |
  | 串流第 30 秒用 MENU 鍵關掉儀表板 | **進 B**（0.84～1.11 ms；`echoAge` 由 3 變 18.6 ms） | **進 B**（0.68 ms；`echoAge` 10.7 ms） |
  | 遊戲暫停 35 ms（120 Hz 是 20 ms）：超過一個週期、不到 80 ms | **回 A**（3.5～4.3 ms；`echoAge` 約 6 ms），之後一直是 A | **回 A**（2.9～3.1 ms；`echoAge` 4.5 ms） |
  | 遊戲暫停 150 ms：超過合成器等待的上限 | 那個視窗 `presentPhase` 最大 80.8 ms、少 7 張；**留在 A** | 最大 81.0 ms、少 17 張；**留在 A** |
  | 遊戲暫停 1.5 秒 | 最大 81.3 ms、少 12 張；**進 B**，之後一直是 B | 最大 80.9 ms、少 28 張；之後的視窗是 **A** |

  結論：(1) 遊戲有一幀比一個週期還晚交（但合成器還在等，不到約 80 ms）就回 A——這是第一次直接看到 B→A，先前只是推論。
  (2) 超過 80 ms 的短停頓不會進 B：合成器逾時後重新等，遊戲回來時落在 A。(3) 更長的停頓（1.5 秒）之後 60 Hz 留在 B；
  120 Hz 以 10 秒的視窗看是 A——推測同樣先進了 B，但恢復後遊戲很快有一幀超過 8.3 ms，馬上又回到 A（10 秒的統計分不出來）。
  所以 120 Hz 自己會回到 A，**60 Hz 一旦進 B（開過 SteamVR 的選單、較長的載入或卡頓之後）就留在 B，直到遊戲有一幀超過
  16.7 ms**。
- 其他會進 B 的情形：儀表板開過再關（120 Hz 用按鍵關的 15 場全部是 B，轉換的那個視窗沒有掉任何一格）。冷啟動落在哪一邊
  看開場遊戲載入的樣子：開場有連續 81 ms 逾時等待的 8 場全部是 A，沒有連續逾時的落在 B。合成器等遊戲的上限約 80 ms
  （`presentPhase` 的最大值有 38 個視窗卡在 81.2～83.4 ms，60 與 120 Hz 相同）。
- **要讓它回 A 可以不靠遊戲卡頓（2026-10-10 下午用實驗證實）**：A 與 B 的差別是「虛擬 vsync 到的時候，合成器手上有沒有一張
  遊戲已經交、還沒用掉的幀」——B 多排著一張。讓合成器在一個週期內多跑一輪就能把它消掉：driver 的 PostPresent 立刻返回，
  合成器馬上開始下一輪，這時排著的那一張已經用掉、遊戲的新幀還沒交，合成器只好等它，之後每一格都是等到才送＝A。
  - 實驗用的 dev 開關（`<host>` 的 `sunshine.conf`，預設關）：`vr_dev_kick_every_s = N`（每隔 N 秒一次）、`vr_dev_kick_ms`
    （一次多久，預設 20）。做法是把 driver 的節拍模式暫時改成「PostPresent 立即返回」（driver 既有的 dev 模式，WELCOME 要帶
    dev 旗標 driver 才接受，所以這個開關開著時 server 會帶）；driver 的虛擬 vsync 排程在這段時間不前進，結束後照原本的相位繼續。
    每次印一行 `[VIPLE-VR-KICK] dev: PostPresent free-run for 20 ms`。
  - 結果（每次 20 ms；B 是用合成的 MENU 鍵關儀表板、或把遊戲暫停 1.5 秒做出來的）：

    | 場次 | 在 B 的時候 kick | 在 A 的時候 kick |
    |---|---|---|
    | 模擬頭盔 60 Hz | 2 次都當場回 A（`presentPhase` 0.72～0.96 → 3.3 ms，`echoAge` 18～20 → 5 ms） | 2 次都留在 A；那 10 秒多 3～4 張幀，`missed` 0、`late` 0 |
    | 模擬頭盔 120 Hz | 2 次都當場回 A（0.72～0.76 → 3.4 ms，`echoAge` 11 → 5～6 ms） | 2 次都留在 A；多 3～4 張 |
    | 真頭盔、半速 60 Hz＋補幀 | 回 A：**MTP 中位數 70.5～71.8 → 53.9～57.3 ms** | 3 次都留在 A；頭盔那 10 秒多跳過 3～5 張（多出來的幀），重複顯示沒有增加 |

    五次推回全部成功，七次在 A 的 kick 都沒有改變狀態，代價只有那 20 ms 裡多送的幾張幀。
  - **自動化（§VR-DIRECT-KICK，`<host>` 的 `vr_direct_kick = enabled`；預設 disabled，配戴驗過才改）**：bridge 執行緒看 `presentPhase`，
    最近 1 秒的中位數 < 1.15 ms 而且第 90 百分位 < 2.2 ms 連續成立 3 秒就判定落在 B、kick 一次（長度＝週期×1.25，夾在 20～40 ms），
    2 秒後確認中位數有沒有回到 2 ms 以上；沒有變化就退避（10／30／90／300 秒），所以儀表板開著時不會一直白推。只在 host 週期
    11 ms 以上（約 90 Hz 以下，也就是半速）動作；kick 成功後若遊戲一幀佔掉週期的八成以上就不再動它（重負載時留在 B 不一定比較差，
    見下面「B 的代價看負載」）；一條連線最多 20 次。判斷的邏輯在 `Sunshine/src/vr/vr_direct_kick.h`（純標頭，64 個單元測試）。
    log 是 `[VIPLE-VR-KICK] one period behind the game … free-run for … ms (#n)` 與 `result: …`（`log_tags.md`）。
    **2026-10-11 起判定多一個條件（回聲年齡）**：只看 `presentPhase` 在實際的場次誤判了——SteamVR 剛啟動、遊戲還在載入的
    那一兩分鐘，第 0 層是合成器自己的畫面，`presentPhase` 中位數 0.79～1.22 ms、第 90 百分位 1.3～1.4 ms，和 B 分不開，連推三次
    都沒有變化才進入退避；那段時間 `echoAge` 中位數只有 2.7～5.2 ms，根本沒有多一格。現在同一個 1 秒視窗裡還要有至少 24 筆
    回聲年齡、中位數 ≥ 5.5 ms（A 的典型值）＋半個週期（60 Hz 是 13.8 ms；A 量到 4.4～7.7、B 17.7～20.7 ms）才算 B。回聲年齡由擷取
    執行緒每收下一張對到姿態的影像交給 bridge 一筆（中間的緩衝在 `Sunshine/src/vr/vr_echo_handoff.h`，9 個單元測試，含真的執行緒
    對打），所以沒有串流在擷取、影像對不到姿態時不會推；頭部靜止的無人場次
    `echoAge` 無效（下面「之後每一場的紀錄都要寫狀態」），要讓頭動起來才驗得到。推完的確認規則沒有改（只看 `presentPhase`），
    結果那一行多印 kick 前後的 `echoAge`。**這個條件目前只有單元測試，還沒有在 `<host>` 上跑過**；下表四場是只看 `presentPhase`
    時的結果。
    實機驗收（2026-10-10 下午）：

    | 場次 | 結果 |
    |---|---|
    | 模擬頭盔 60 Hz（MENU 關儀表板、之後把遊戲暫停 1.5 秒） | 兩次都在進 B 之後 3～4 秒 kick、2 秒後確認回 A（0.76 → 3.50、0.75 → 3.24 ms）；整場沒有任何一個 10 秒視窗是 B；儀表板開著的那 30 秒沒有 kick |
    | 模擬頭盔 120 Hz（MENU 關儀表板，之後 50 秒都在 B） | 0 次 kick（照設計不動作） |
    | 真頭盔、半速＋補幀（MENU 關儀表板） | 3 秒後 kick、回 A；之後 MTP 中位數 55.6 ms（同樣的場次不處理是 70.5～71.8 ms），重複顯示 50.01%、`late` 每 10 秒 0.1 |
    | 真頭盔、半速＋補幀 10 分鐘（開場就在 A） | 0 次 kick，MTP 57.3／61.4 ms；對照：同樣 10 分鐘、沒開這個開關而開場落在 B 的那一場整場都是 70.8／79.1 ms |

    還沒做的：沒有人配戴驗過；kick 成功時 `<host>` 的姿態預測要約 15 秒才追回少掉的那一格（可以直接減一個週期）；偵測器的狀態
    跟著 driver 的連線走（兩個「不再推」與次數不隨 session 重來；判定本身自 2026-10-11 起要有串流在擷取才會成立）；
    回聲年齡的條件要在 `<host>` 上驗兩件事：載入那一段不再推、真的落在 B 時照樣在 3～4 秒內推回。
- **統計**：120／90 Hz、儀表板整場停用或配戴的 28 場全部是 A。直通的半速（60 Hz）12 場是 B 4 場（10-08 兩場、10-10 的 S1 與
  S4／S5）、其餘 A（含配戴兩場）。「半速多半是 B」不成立。
- **B 的代價看負載**：約等於 max(0, 週期＋0.8 ms −（遊戲 CPU＋GPU 時間））。A 的編碼要等遊戲這一幀的 GPU 做完，B 的幀在等
  那一格的時候 GPU 已經做完。上表的 3.5／12 ms 只對輕負載成立；120 Hz、解析度倍率 300% 時 B 到第一個封包不比 A 慢
  （A 的中位數 8～11 ms，B 3.8 ms＋等的那一格），而且 fence 等待只有 1.4 ms、`late=0`——重負載時留在 B 反而沒有
  §VR-FENCE-LATE 的問題。只有輕負載加長週期（半速）才貴：10-10 的 S1（B）MTP 66.0 ms，S2／S3／F0／F1（A）55.4～58.3 ms。
- **SteamVR 不知道 B 多排了一格**：配戴場次的 MTP 與 §VR-PREDICT 收斂的值都符合「預測目標＝PostPresent 返回＋2.8 ms＋一個週期
  ＋`vsync_to_photons`」，A、B 相同（60 Hz 的 A 也對得上，誤差 1 ms 內）。B 時遊戲算圖用的姿態就是舊一格的，要靠 §VR-PREDICT
  把預測時間往上補（60 Hz 的 B 估計要約 44 ms，A 約 31～36 ms）——所以半速時 §VR-PREDICT 一定要能動作，見「2026-10-10 配戴後
  查到的另外兩個問題」。
- **同場對照（2026-10-10，頭盔無人場次，同一場先 B 後 A；`scripts/vr/x2_ab.sh`）：B 並沒有比較流暢。**
  - 半速 60 Hz＋補幀（B 130 秒、暫停遊戲 20 ms 翻成 A 之後 210 秒；各去掉轉換後 60 秒）：晚一格才顯示的影像（LDIAG 的
    `lateShift`）B 每 10 秒 23.4、A 21.8，`late` 0.2 對 0.3——沒有差別；MTP p50／p95 是 B 71.0／75.2、A 58.0／62.2 ms。
    **半速時 B 只是白白多 13 ms。**
  - 全速 120 Hz：關掉儀表板後進 B，約 60 秒後遊戲自己有一幀超過 8.3 ms 就回到 A（那個視窗重複顯示 9%）。B 的那 60 秒
    （LATCH 還在收斂）重複顯示 3.28%，之後 A 的 210 秒是 3.31%（`late` 22 對 24、MTP 59.0 對 54.1 ms）。先前跨場次看到的
    「B 的重複顯示只有 A 的一半」在同一場裡沒有重現；而且 120 Hz 的 B 本來就留不住。
  - 所以：120 Hz 不必處理；**60 Hz（半速）進了 B 應該主動翻回 A**。做法見上面「要讓它回 A 可以不靠遊戲卡頓」，列入待辦。
- **之後每一場的紀錄都要寫狀態**：看 `presentPhase` 的中位數（約 3.5＝A、約 0.77＝B），不要看儀表板有沒有停用，也不要看
  `evtToPush`（遊戲關掉反鋸齒之後 A 只有約 0.9 ms）；頭部靜止的無人場次 `echoAge` 無效。無人場次 §VR-PREDICT 不動作
  （合成的頭部姿態量不到姿態落後），狀態不同的場次 MTP 不能比。
- 還沒做的：(1) 同場對照 A 與 B 哪一種重複顯示比較少（頭盔無人場次：儀表板關掉進 B、量一段、暫停遊戲 20 ms 翻回 A、
  再量一段）。(2) driver 已經把 PostPresent 的節流參數、GetFrameTiming 的傳入旗標、SubmitLayer 的預測秒數寫進 timing ring，
  server 還沒有印。(3) 無人頭盔場次的腳本收尾把遊戲與 SteamVR 一起強制結束，拿不到 SteamVR 自己量的遊戲 CPU／GPU 時間
  （`Cumulative stats` 只在遊戲先結束時才印）；改成先結束遊戲、等 1 秒再結束 SteamVR。

**頭盔上可用的紀錄（2026-10-09 盤點）**

| 來源 | 內容 | 注意 |
|---|---|---|
| `~/.local/share/Steam/logs/vrcompositor.txt` | 每個 VR 程式結束時印 `Cumulative stats`：顯示格數、掉格、重投影、合成器與程式每格的 CPU／GPU 毫秒 | 不必開任何設定；只留這一輪與上一輪，場次結束要馬上拉 |
| `perfrecording-steam.app.<捷徑 ID>-<時刻>.csv` | 每秒一列：fps、程式的 CPU 時間（含延後挑影像睡的那一段）、重投影、GPU 時脈、整機功耗 | 要頭盔 `steamvr.vrsettings` 的 `perfcheck.recordStats`；runtime 的 fps 與重投影看不到我們的重複顯示（我們每一格都交圖） |
| `/var/log/deckard-fan-control.log` | 每秒一行：各核心與 GPU 的溫度、風扇、主電源軌功率 | 只留這次與上次開機 |
| journal | Wi-Fi 事件（wpa_supplicant 與 kernel 的 wlan0 行）、電源、我們行程的 CPU 時間與記憶體峰值 | Steam 的 `client_networkmanager.txt` 不記同一個網路內換基地台 |
| `eyetracking.txt` | `HMD on／off`＝有沒有人戴 | 沒有注視點的數值 |
| `/usr/bin/gpu-trace` | kernel 事件與合成器的標記，可以畫出 GPU 上每一筆工作的時間軸 | 見上面第 3 點；擷取的動作約 2 秒，取回的資料裡事件齊全的只有最後 5～6 秒 |

- 頭盔的 GPU 時脈會隨負載升降（穩定串流時多半 578 MHz，最高 903），我們畫一張影像的時間跟著變；跨場比較「等 GPU」要附時脈。
- 頭盔的 Wi-Fi 驅動（ath12k）在換基地台時卡死過兩次（10-07、10-09）。10-09 那次 kernel 先印
  `arm-smmu … Unhandled context fault`，接著每分鐘上百行 `failed to transmit frame`、`wmi command … timeout`，介面看起來還連著
  但完全不通，重開機才恢復。看到這些訊息就不必再查我們的程式。
- 我們的程式在頭盔的 Flatpak 裡升不了執行緒優先權（`setpriority() failed`）；runtime 替每個程式寫的 `xrclient_<名稱>.txt`
  我們這邊沒有（沙箱看不到 Steam 的 log 目錄）。兩項都還沒處理。

**第一次經網際網路（Tailscale）串流（2026-10-09，配戴實玩約 3 分 40 秒）**

條件：頭盔不在 `<host>` 的區網內，經 Tailscale 全程直連，往返時間 7～28 ms（中位數約 10 ms）；每眼 1152、120 Hz、注視點 1.5x、
延後挑影像 5 ms。使用者的體感：勉強可玩。

- 重複顯示平均每 10 秒 73 格（6.1%；去掉一個因網路中斷到 43.5% 的視窗，算進去是 8.1%）；MTP p50 52.2 ms，比區網多 6.5 ms，
  幾乎全是網路往返多出來的。
- 自動位元率 30 秒內由 79 Mbps 掉到下限 4,964 kbps，之後八成的時間不到 8.1 Mbps。原因在規則本身：0.5 秒的視窗配
  1%／3%／8% 的門檻，一次一兩張影像長度的空白就算成 2～4%；回升要連續 5 輪乾淨；下限是上限的 8%。5～16 Mbps 之間丟包比例
  不隨速率變（約 0.6～0.7%），降到下限沒有換到任何東西。高於 15 Mbps 的資料整場只有 20 秒，「不是頻寬不足」還不能下定論。
- 補包大半時間沒有作用：client 只在往返時間不超過 10 ms（120 Hz；90 Hz 是 14、60 Hz 是 19）時才回報缺包，用的是最近一筆、
  沒有平滑，而這條路徑剛好在門檻上下。結果約每 1.6 秒有一張救不回，要靠 intra refresh（約 88 ms）恢復。
- 封包大小（走 VPN 介面時自動用 1024）、多連線（只建了該建的一條）、控制連線都沒有問題。
- 從 GUI 啟動的 VR 串流設不了位元率與自動位元率的開關（只有命令列可以）。
- 同一晚的無人對照顯示，重複顯示的主因其實是影像被頭盔的合成器擋住（見上面「頭盔的「GPU 忙碌期」定案」），延後挑影像改成
  6 ms 之後少四到五成；網路造成的部分（真的晚到、救不回）比原先以為的少。

**2026-10-10 同一條路徑上全速與半速交替跑（頭盔無人場次各 200 秒；兩組、順序相反：全速→半速、半速→全速；往返時間中位數約 10 ms、7～18 ms）**

| | 全速 120 Hz | 半速 60 Hz＋補幀 |
|---|---|---|
| 救不回、要靠 intra refresh 的幀（整場；兩組） | 63、72 張（約每 3 秒一張；每張到畫面恢復約 87～116 ms） | 1、1 張 |
| client 每 10 秒回報缺包／補回 | 59～94／57～83 | 135～165／131～154 |
| 等補包的上限 | 8.3 ms | 14.0 ms |
| 晚到（LDIAG，每 10 秒） | `late` 22.9、`lateShift` 9.5、`gap` 3.5；重複顯示 2.94% | `late` 1.2、`lateShift` 13.3、`gap` 0.1 |
| MTP p50／p95 | 53.9／58.0 ms | 57.0／61.2 ms |
| host 實際送出（整場平均；位元率設定） | 104、104 Mbps（88 Mbps） | 45、47 Mbps（44 Mbps） |

- 兩種模式補包都有在動作，差別在等得夠不夠久：全速只等 8.3 ms，往返 10 ms 上下時補的封包有一部分來不及，那一幀就放棄、
  改走 intra refresh；半速等 14 ms，幾乎全部救得回來。**所以在往返約 10 ms 的路徑上，半速＋補幀因掉包造成的停頓比全速少
  得多**（兩組交替、順序相反，結果一致；樣本都是同一天、同一條路徑，比例不要當成定值）。全速要拿到同樣的效果，得讓等待的上限跟著往返時間走
  （多等幾毫秒、讓一兩張晚一格，換掉一次約 87 ms 的恢復）——已實作成 §VR-REPAIR-RTT（開關預設關），見下面。
- 流量：VR 的位元率設定跟著串流的幀率走，半速時設定值減半，實際送出的量也接近減半（104 → 45 Mbps，少了 57%；
  兩場都是注視點 1.5 倍、每眼 1152×1152）。要省流量，半速＋補幀本身就是最直接的一步。
- 同一輪另有兩場（11:51、11:54）沒有成立：第一場開場 1 秒後連續 15 秒收不到可用的影像而結束，第二場連不上；同一時間
  開發機到另一台機器的 VPN 連線也逾時，是那幾分鐘網路本身的問題，不是串流的問題。

**2026-10-10 下午同一條路徑的三組對照（頭盔無人場次、全速、各 200 秒；自動位元率關＝使用者頭盔現在的設定）**

先講一個更正：**這條路徑的狀況隨時段變，不同時段的場次不能直接比**。正式設定的全速，中午重複顯示 2.9～3.4%、LDIAG 的 `late`
每 10 秒 23～25 次；下午同樣的設定是 5.1～5.2%、50 次（MTP 中位數沒有變）。下面每一組都是同一個小時內交替跑的，而且各帶一場
正式設定當基準。

| 組 | 場次（依時間） | 救不回的幀（整場） | 重複顯示 | `late`／10 s | MTP p50 | host 送出 |
|---|---|---|---|---|---|---|
| 補包的時間預算（13:26～13:38） | 實驗建置：全速直接用半速的預算（client 等 14 ms、server 補 50 ms 內的 block） | **10、17** | 5.8%、5.5% | 58、54 | 55.6、53.6 ms | — |
| 補包開或關（13:47～13:59） | 補包整個關掉（`vr_multilink_repair = disabled`） | 85、75 | 5.2%、5.3% | 49、50 | 53.9 ms | — |
| | 正式設定 | 78 | 5.1% | 50 | 53.9 ms | — |
| 位元率（14:00～14:17） | `--bitrate 50000` | 59、50 | 5.7%、5.5% | 58、57 | 50.9、51.7 ms | 60 Mbps |
| | 正式設定（VR 預設 88.9 Mbps） | 70 | 5.2% | 50 | 54.9 ms | 104 Mbps |
| | `--bitrate 30000` | 33 | 4.8% | 51 | 49.9 ms | 37 Mbps |

- **等補包的上限是全速救不回的主因**：只把兩個常數改成半速的值（實驗用的建置，沒有提交），同一個小時內救不回的幀由
  75～85 張降到 10、17 張，重複顯示與晚到幾乎沒有變。剩下的那十幾張多半是「第二次回報的補包還在路上就到期」
  （回報後 4～6 ms 就到期，往返是 10 ms），另有一次連掉六幀的長空白。正式的做法（上限、重報間隔都跟著量到的往返時間走，
  只在單一連線時啟用，放在開關後面）已實作成 §VR-REPAIR-RTT，見下面。
- **現行規則下，全速時補包幾乎沒有作用**：開著 78 張、整個關掉 85、75 張。
- **位元率只影響曝險，不影響晚到**：位元率越低每幀的封包越少，救不回的幀跟著少（89 → 50 → 30 Mbps：70 → 50～59 → 33 張），
  延遲也少 4～5 ms；但重複顯示與 `late` 不變（約 5%）——晚到來自路徑的延遲抖動，全速只有約 4 ms 的餘裕（半速有 8 ms，
  同一條路徑上 `late` 每 10 秒不到 2 次）。
- **把相位控制器的目標餘裕加大，對晚到幫助很小**（16:38～16:58，同時段交替五場；這一段路徑的狀況又回到中午的水準）：
  v2、目標 50% 重複顯示 2.71%；v3、目標 50% 是 2.71%、2.66%；v3、目標 65%（多約 1.25 ms 的餘裕）是 2.47%、2.08%，
  MTP 中位數多約 1 ms。晚到的幀不是「只差一點點」的那一種，要處理得另外想（多留一整格，或先弄清楚它們晚在哪一段）。
- **`<host>` 到頭盔的 VPN 直連會掉到中繼，有時回不來**：2026-10-10 閒置十幾分鐘之後發生五次（14:59、15:21、16:24、約 19:13、
  22:30 前後）。前三次持續數分鐘到十幾分鐘；第四次持續兩個半小時以上。往返時間由約 10 ms 變成 95～130 ms，那時開的場次只收得到
  四分之一的封包、完全不能用，而且從串流的 log 看不出原因（要在 `<host>` 上問 VPN 軟體這條連線是不是直連）。持續有流量時沒有
  發生過。無人場次的腳本開跑前會檢查。使用者遠端玩時若遇到「突然完全不能玩」，這是第一個要查的。
  - **原因在兩端的 NAT，不在串流程式**（在家用路由器的連線追蹤表上看到的，唯讀）：VPN 軟體在每台機器上都用同一個本機 UDP 埠，
    而家裡有三台節點、頭盔那一端至少兩台共用各自的 NAT。路由器只有在不衝突時才保留來源埠，衝突的那一條換到別的對外埠——
    `<host>` 因此同時有兩個對外埠：對 STUN 伺服器的舊連線用原本的埠，往頭盔的新連線用另一個，而且一直沒有回應；頭盔公告的
    對外埠一小時內也換了四個。各自用 STUN 量到、公告出去的埠，和實際對那個對端用的埠對不上，兩邊的 NAT 又只放行自己送過去的
    那一組，打洞就失敗、只能走中繼。已經建立的直連只要不閒置超過 3 分鐘（UDP 連線追蹤的逾時）就一直維持，所以是「串流中不會掉、
    閒置之後才掉、回不回得來看運氣」。兩端的 NAT 檢測（VPN 軟體另開一個 socket 量）都顯示正常，看不出這個問題。
  - **第四次回不來的那兩個半小時，和頭盔同一個對外位址的另一台節點一直和 `<host>` 直連著。** 使用者把那台的 VPN 斷線之後約
    3 分鐘，第一次探測就回到直連（10～11 ms）；之後閒置 25 分鐘又掉了一次，但那台不在線上時兩端一探測約 10 秒就重新建立。
    「同一個 NAT 後面有另一台節點連著 `<host>`，頭盔就拿不到對得上的埠」目前只有這一次前後對照，不是對照實驗。
    在那之前，從 `<host>` 每 20 秒探測一次持續 25 分鐘、以及完全靜置 12 分鐘再試，都沒有回來。
  - **治本要動網路設定，由使用者決定（沒有動）**：家用路由器把 VPN 的那個 UDP 埠固定轉給 `<host>`（和現有那三條串流用的轉埠
    同一種做法）——頭盔不管從哪個埠送過來都會轉到 `<host>`，回覆走同一筆連線追蹤出去，不必打洞，對方的 NAT 怎麼換埠都沒關係。
    不改設定的話：遠端玩之前，同一個網路的其他節點不要連著 `<host>`。
- 順帶查到：頭盔上的設定在 2026-10-10 早上被改成「自動位元率關、位元率 50 Mbps」，但 VR 模式不看那個位元率
  （沒有帶 `--bitrate` 就用 VR 自己的預設，全速 88.9、半速 44.4 Mbps），實際送出的是 104 Mbps。VR 需要自己的位元率設定
  （下面「還沒做的」最後一項）。

**§VR-REPAIR-RTT：補包的門檻、等待、重報跟著往返時間走（2026-10-10 晚實作；server 開關 `vr_multilink_repair_rtt`，預設關）**

- 做了什麼：client 用最近量到的往返時間（一個視窗裡的低、中、高三個值）決定要不要回報、等多久（上限約 33 ms）、多久重報一次；
  後面整幀沒到的幀一起報；server 對帶旗標的回報放寬可以補的年齡。只在單一連線送影像、而且兩端都同意時才啟用（還要
  `vr_multilink_repair` 與 `vr_multilink_ctrl` 開著）。另外加了不分規則都開著的量測（`[VIPLE-VR-REPAIR] gate 10s` 等）。
  規則與協定位元見 `vr_protocol.md`，設定見 `Sunshine/docs/configuration.md`，log 欄位見 `log_tags.md`。
- 開關關著（或對方沒有同意）時和原本逐步相同：用真的佇列程式做的模擬，2,844 個情境的時間軸完全相同；審查者另寫一套
  （把接收迴圈那一段從原始碼切出來編進去）22,840 對情境也完全相同。
- 驗過的：純邏輯的單元測試 1,986 項、亂數不變式 2,500 個情境（沒有當掉、完整到達的幀沒有被丟、被壓住沒有超過上限）、
  變異測試 56 個全部被抓到、server 的 googletest 75 則；Windows client／server、Android、arm64 都用建置腳本建過。
- 三份對抗式審查（開關關著與協定、規則本身、server 與 client 的接線）找到一個要修的：**放棄一幀之後的暫停，原本每跳過一幀就
  重新起算，成串掉包時第一次放棄之後整段都不再回報**（60 Hz 的例子：現行規則掉 2 張、新規則掉 10 張）。已改成只從第一次放棄
  起算（同一個例子變成 4 張）。其餘是量測的記帳與文件，都已修或寫明。
- **區網上用故障注入模擬往返 11 ms**（`vr_multilink_fault` 的 `d1:5`＝每個方向晚 5 ms；開發機當 client、單一連線、各 70 秒、
  開場 15 秒不算；這幾場用的是審查修正前的建置）：

  | 模擬的掉包 | 更新率 | 規則 | 補包開著 | 每 10 秒救不回 | 每 10 秒補回 |
  |---|---|---|---|---|---|
  | 每 400 ms 連掉 20 個封包 | 120 Hz | 現行 | 0% | 76.6 | 0 |
  | | | 新 | 100% | 0 | 25 |
  | 每 500 ms 斷 12 ms | 120 Hz | 現行 | 0% | 27 | 0 |
  | | | 新 | 100% | 0 | 39.8 |
  | 同上，同一場每 20 秒切換（`--vr-repair-ab 20`） | 120 Hz | 現行那一段 | 0% | 40 | 0 |
  | | | 新規則那一段 | 100% | 0 | 40（補回平均花 12.1 ms、最久 19.8 ms） |
  | 每 499 ms 斷 12 ms | 60 Hz | 現行 | 100% | 0 | 15.4（回報 36.2） |
  | | | 新 | 100% | 0 | 14.0（回報 14.8） |

  120 Hz、往返 11 ms 時現行規則的補包整個不啟動（和真的路徑上「開或關都一樣」相符），新規則把每一次都補回來；代價是被補的
  那幾幀晚 10～20 ms 才放行。60 Hz 時現行規則本來就補得回來，新規則結果相同、回報少六成。第一列兩場的位元率不同
  （沒補回來的那一場被自動位元率壓到 8.5 Mbps），同條件的比較看同場切換那一列。這是固定延遲、規律掉包的模擬，不能代替
  真的路徑。
- **已知的限制**：補包本身也跟著掉的重度成串掉包，60 Hz 時新規則仍比現行規則差一些（模擬：沒收齊的幀 74 對 85 張；
  每一輪是「等到上限 → 放棄 → 暫停裡兩三幀不補」）。半速（60 fps）在往返 10 ms 上下現行規則本來就有效，所以這個開關目前只
  適合全速時開；要不要改成「現行規則的門檻不成立時才換新規則」還沒決定。其餘寫在 `vr_protocol.md` 的已知限制。
- **真的路徑上的驗收（2026-10-11 00:05～00:35，VPN 直連、往返 8～15 ms，頭盔無人、合成的頭與手；最終版）**：
  - 先只看量測（新規則關著、全速 200 秒）：現行規則的門檻只有 27% 的時間成立（往返的中位數在 10～13.7 ms 之間擺，每 10 秒
    切換 10 次以上）。每 10 秒救不回 4.06 張，其中 3.71 張是**根本沒有回報**；報過仍放棄的那幾張，補包在放棄之後 5～10 ms
    內就到齊。回報每 10 秒 47.8 份、真的用上補包的只有 0.94 個 block——其餘是幀內停頓 3 ms 觸發、封包自己接著到的，
    `<host>` 每 10 秒白送約 1,600 個補包。這條路徑上約 11% 的幀掉過封包，幾乎都由 40% 的 FEC 補回，約 0.35% 超過 FEC 的能力；
    另外每 10 秒約 150 次幀內停頓超過 3 ms（一幀的封包到一半停住；晚到的來源）。
  - 全速、同一場每 40 秒切換一次規則（`vr_multilink_repair_rtt = enabled`＋`--vr-repair-ab 40`，各 420 秒）：

    | 場次 | 規則 | 五段各自救不回 | 合計（200 秒） | 每 40 秒的回報 | 重複顯示 | 連續 3 格以上沒換影像／10 s | 5 格以上／10 s |
    |---|---|---|---|---|---|---|---|
    | 第一場 | 現行 | 10、14、16、26、9 | 75 | 164～244 | 4.09% | 0.57 | 0.21 |
    | | 新 | 0、1、9、1、0 | 11 | 16～71 | 3.28% | 0.93 | 0.13 |
    | 第二場 | 現行 | 9、17、5、7、12 | 50 | 164～209 | 2.97% | 0.29 | 0.07 |
    | | 新 | 6、1、0、0、0 | 7 | 12～19 | 2.41% | 0.60 | 0.07 |

    **救不回的幀少 85%、86%**（驗收門檻是不超過現行的四分之一），回報少約九成。新規則補回一次平均花 9～13 ms、最久 35 ms：
    等於把「局部破圖＋約 90 ms 的恢復」換成「那幾幀晚 10～30 ms」，所以連續 3 格沒換影像的次數略多、5 格以上的沒有變多，
    整體的重複顯示還少一點。新規則出事集中在成串掉包的那一段（第一場第三段的 9 張，其中 6 張沒有回報：放棄之後的暫停，
    以及一次缺太多不補）。
    補包用上之後 60 ms 不取相位樣本，約佔新影像的 2～3%。兩場都沒有出現保險暫停。
  - 半速＋補幀、同場切換（420 秒）：現行規則 2 張、新規則 0 張；回報量相同（60 fps 時兩種規則的回報幾乎都是幀內停頓觸發的），
    補幀的防護計數 0。半速時現行規則的門檻九成以上的時間成立，新規則沒有可見的好處、也沒有變差。
- **還沒做**：有人配戴的驗證；低位元率與自動位元率開著的那兩組；60 fps 的回報量（兩種規則都偏多）。驗過之前開關維持預設關；
  全速遠端玩時可以先在 `<host>` 手動打開。

還沒做的：自動位元率要分得出壅塞與短暫空白，回升步長與下限重訂；只有一條連線時用時間上的分散取代第二條連線（§VR-FEC-SPREAD，
純邏輯已經有了、還沒接上）；替遠端做一組預設；把 VR 的位元率放進設定頁。

**頭盔端管線下一輪：不改程式的三組量測（2026-10-11 凌晨；頭盔無人、合成的頭與手、經 VPN）**

- **延後挑影像，runtime 容許到多晚**（全速、每個值一場 110 秒；每場擷取兩次 gpu-trace，所以全系統 perf 取樣開著）：

  | 延後 | runtime 重投影（整場／扣掉開場那一桶） | frame thread 回到 `xrWaitFrame` 到下一次放行：p5／p50／最小 | 備註 |
  |---|---|---|---|
  | 6.0 ms（兩場） | 5／3、4／3 | 1.52／1.80／1.15、1.53／1.74／0.98 ms | |
  | 6.3 | 4／4 | 1.22／1.45／0.71 | |
  | 6.5 | 6／3 | 1.02／1.24／0.44 | 半速＋補幀的 6.5 ms（沒擷取）和 6.0 ms 也沒有差別 |
  | 6.8 | 8／3 | 0.72／0.95／0.07 | runtime 還沒有反應，但餘裕最小的那幾格已經貼到放行點 |
  | 7.0 | 74／71 | 0.51／0.72／0.09，另有 2 格晚回 | 另漏 3 幀；出事集中在開場的第一個 10 秒 |
  | 8.0 | 4,552／4,456（35%） | 迴圈不再等放行 | 每一格都多預測一格（預測顯示時刻 28.5 → 36.9 ms） |

  runtime 的放行點從頭到尾沒有動（相位 0.21～0.23 ms）；它等待的目標＝5.21 ms − 應用程式每格的 CPU 時間（12 份擷取，殘差
  0.035 ms 以內）。**穩定的延後量上限訂 6.5 ms**：6.8 ms 時 runtime 的統計還看不出差別，但餘裕最小的那幾格幾乎都是每 10 秒印
  log 的那一格（`xrEndFrame` 之後多做 0.4～0.9 ms，runtime 量不到這一段），六場抓到的六格換到 6.8 ms 有五格不到 0.3 ms。
  之後的「等在途的幀」不要在那一格等，或把那一段工作移開。每個值只有一場；配戴、不開追蹤時的尾巴沒有量過。
- **半速＋補幀（使用者實際用的模式）的第一批 gpu-trace**（同一場三次擷取，共 1,068 張串流影像、2,135 個顯示格；對照是幾分鐘後
  全速的兩次擷取）：
  - 每張串流影像是兩批 GPU 工作：真的那一批 1.23 ms（色彩轉換 0.22＋還原 1.00），隔 0.2 ms 之後是補幀那一批 2.05 ms
    （六筆，最後寫整張的那一筆 1.19 ms）。這是 903 MHz 的長度：這個模式整場把 GPU 時脈釘在最高檔，全速那一場在 578 MHz，
    兩種模式的毫秒數不能直接比。
  - 補幀開著時「還原」那一筆寫的是兩倍高的影像，不會顯示的下半和上半一樣貴（約 0.5 ms，佔真的那一批四成）——先前由 log 估的
    0.1～0.2 ms 低估了。
  - 照現行的相位目標，準時影像的補幀最後一筆正好壓在合成器要 GPU 的時刻（相位 3.0～4.1 ms）：GPU 要等一筆繪製畫完才換手，
    合成器最多晚 1.36 ms 開始（全速 1.00 ms），晚超過 0.6 ms 的格 8.1%（全速 4.2%）。但合成器做完與寫進顯示控制器的餘裕
    和全速同一個水準，三次擷取 2,690 格沒有任何漏格或重投影的標記。
  - 追蹤開著的那一場 runtime 記了 95 次重投影、23 次漏幀（同樣開追蹤的全速是 6、8 次），其中 26 次與 22 次落在開場與逾時那兩桶，
    對應開場第 5 秒的一次卡頓；其餘沒有逐次的時刻。沒開追蹤的同類場次兩種模式都是 3～8 次。**這 95 次不代表穩態**，
    配戴時有沒有要另外量。
  - 對設計：「補幀那一批避開合成器」如果要做，只需要把最後那一筆延到相位 4.5 ms（不是整批、也不是 5.3 ms）；不要把補幀併成
    較少筆（不可換手的一段會由 1.19 ms 變 2.05 ms）；值得先做的是讓「還原」不畫下半。
- **量測建置（規格的第一步）已整合進本地 main（2026-10-11 早上，25 個提交；還沒在頭盔上跑）**：一律開著、只量不改呈現。
  新增的行與欄位：`[VIPLE-VR-XRLOOP] 10s`（frame loop 醒來的相位、挑影像與交幀離放行點多遠、可相加的延遲鏈 dec＋render＋slack＋lead、
  上一格交幀到這一格呼叫 `xrWaitFrame` 的 `tail`）、`[VIPLE-XR] 10s video` 行尾（mailbox 等待、swapchain 等待、送出與做完的相位、
  補幀那一批的相位、`ab=`）、`[VIPLE-VR-LDIAG] 10s` 行尾（不看分類器基準的晚到計數：等了幾格、提早送出救得到幾張）、
  `[VIPLE-VR-DECPOLL] 10s`、`[VIPLE-VR-PASS] 60s`、`10s frames` 行尾的 `discarded=`；`--vr-latch-ab <秒>` 可以由命令列指定同場切換。
  兩份對抗式審查（不改行為、量得對不對）之後修的重點：相位的 p50 改取環狀的中位數（補幀那一批做完的時刻橫跨 0／T 的接縫，
  照數值取會差 1～4 ms、結論相反）；`ab=` 改成照這個視窗兩邊各跑了幾格標（原本幾乎每次都標在換邊之前的最後一個視窗）。
  Windows 與 arm64 都編得過，內建自測 71 案通過。下一步是頭盔無人的基準四場，先看 `tail max`。
- **`<host>` 的相位控制重新對齊要多久**（全速兩場，第 80 秒起把每張影像多延後 0.5／1.5 ms、第 160 秒收回）：1.5 ms 的階躍，
  誤差 15～19 秒過零，接著是反方向兩成多的過衝，46～56 秒才結束，加上去之後的前 30 秒多出 37 格重複顯示；0.5 ms 的階躍
  量不到暫態（比 10 秒視窗自己的雜訊小）。之後做會移動 `<host>` 相位的同場切換：移約 0.5 ms 每次丟 20 秒、1.5 ms 丟 60 秒，
  MTP 與鎖相相關的量不要跨這種切換比。各只有一場。

**還沒做**

- **§VR-FENCE-LATE 的後續**：過載旗標改成表達「Present 鎖不住虛擬 vsync」已實作並在 `<host>` 與頭盔無人場次驗過
  （§VR-CADENCE，`vr_capture_overload`，預設仍是舊定義；見上面已知的限制）——**剩下配戴驗證，驗過才把 `vr_capture_overload`
  與 `vr_capture_late` 的預設一起改**（推論開關關著的舊規則對比較重的遊戲會直接丟幀或全黑，所以要盡快）；負載重時
  `<host>` 編碼段的雙峰（見實測；先確認機制）；頭盔端顯示「host 的 GPU 過載」「host 的 VR 連線中斷」，黑幀與姿態無效的幀
  不算 live；`vr_probe ipcpeer --peer-gpu-hold-ms` 與 selftest 的 T2.gpu-hold（順便驗卡死時結束串流不會觸發 Hang detector），
  `selftest_consume()` 改用同一份判定；用第二個（比較重的）遊戲驗一次；把過載狀態放進 STATS 送給 client；依完成順序取用
  （等 `skipped` 與 driver 的 `noslot` 整理出來再決定）；讓合成器固定自己合成。
- **降低每幀的像素量**（10-07 晚結論一）：編碼尺寸小於算圖尺寸（已做）、注視點編碼（已做，見上）、10-bit（host 端已做，
  Frame 的解碼路徑不支援，見上）；先用每眼 1152／1440
  在原本位置完整跑幾分鐘，確認延遲與流暢度（要等姿態預測收斂），並請使用者戴眼鏡看清晰度能不能接受。
- 結論二、三與延遲拆解在 10-08 上午做了（見上）。剩：`always`＋§VR-LINK-FRESH 在真頭盔的弱訊號位置驗證、
  頭盔合成管線那約 21 ms 有沒有別的路可以繞。（控制連線改連另一個 host 位址已做，見 §VR-LINK-REHOME。）
- **和 Steam 內建串流比延遲**：它的 log 沒有數字，要請使用者開頭盔裡的進階效能圖表（`showAdvancedGraphs`）讀出來。
- **補幀**第一版已做（§VR-HALF-RATE＋§VR-SYNTH，見上），待使用者配戴確認。搜尋範圍與物體邊緣在第四版做了（見上）；邊緣鋸齒要不要再處理，看配戴的結果。
- **使用者實際配戴驗 §VR-LINK-CTRL／§VR-LINK-REPAIR 與 A2**：10-07 晚配戴驗過（見上：A2 成立、
  掉幀回報與補包有接上、120 Hz 與 overscan 有效、修正後的探測放行沒有再誤放）。要看 10 s 行的 `lagMs`、`hold 10s` 的 `nack／repaired／gone`、server 的 `[VIPLE-VR-REPAIR] 10s`、
  `[VIPLE-VR-LOSS]` 的 `lossToRefresh` 與 `[VIPLE-VR-SESSION] fallback`，再決定各開關的預設值。
- 雙連線、兩條同時斷訊而且其中一條固定落後 8 ms 的組合（S0 `1:489:12,2:489:12,d2:8`）40 s 還會掉 1～2 幀
  （80 次斷訊）：那一幀只有落後那條送到、又缺了幾個，回報離等待期限太近。
- LATCH 帶線上的幀號（`rtpFrame`）：補過包的幀比正常晚到，server 目前認不出來（`frameId` 是發布序號）。
- `video.cpp`：`idr_events` 送出 IDR 時沒有清掉 `vr_idr_retry_pending`，cooldown 到期後可能多送一張 IDR（既有行為，
  第七輪審查順帶看到）。
- 確認第二個位置是不是實際遊玩的位置；不是的話在遊玩位置再量一輪（約 3 分鐘，不用開遊戲）。
- 第二次頭盔實測：核心時戳量測、跨幀等待都只在 S0（Windows、同一張網卡）驗過；等待上限（一個幀週期）與探測門檻
  （1.5 倍、連續 2 次）待實機看 `hold 10s` 的 `maxWaitMs`／`expired` 與 `burstMbps` 再調。
- 每幀分散送出（pacing）重複量測後再決定要不要改 server 的節拍。
- CLIENT_TIMING（1 Hz 統計）仍只走 ENet（不急，掉了也不影響行為）。
- USB 卡死的觸發條件。
