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
2. **gamescope WSI 的 aarch64 layer UNVERIFIED**：SteamOS ARM 是否提供 `libVkLayer_FROG_gamescope_wsi_aarch64.so`、放在哪裡，
   都沒有證據。沒有時 Vulkan loader 只會略過（無害），但 `--filesystem=host-os:ro` 就沒有存在理由，要拿掉。Day 0 會列出
   host 上的 layer 檔案。
3. **`--filesystem=~/.steam:ro` 會暴露 `~/.steam/registry.vdf`**（含 Steam 帳號名）。PoC-F 確認 SteamVR 實際需要的路徑後收窄。
4. **沙箱內的 OpenXR loader 看不到 host 的 `~/.config/openxr`**：由 `XrRuntimeJson` 解析 host 路徑，在行程內設
   `XR_RUNTIME_JSON`（§6.3）。SteamVR runtime 的 `.so` 依賴 Steam Runtime 的函式庫，和 KDE runtime 的 glibc／libstdc++
   是否相容 **UNVERIFIED**（PoC-F-pre、PoC-F）。
5. **libplacebo 選 B（v7.360.1）** 和 FFmpeg 9 的 libav helper 能不能編過 UNVERIFIED；不行就退回 A（補丁一定要帶，否則
   gamescope 下 SIGABRT）。
6. **qemu 建置時間**（G-BUILD，§4）：超標時 U7 提前。
7. **`--device=all` 在 SSH 下的 uaccess**：Developer Mode 經 SSH 跑 `flatpak run` 時，`/dev/video*` 權限是否因為沒有
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
