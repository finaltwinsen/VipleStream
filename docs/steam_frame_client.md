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
#   --xr-dump-frame <path>     第 300 幀影像 quad 讀回 PNG（最長邊 ≤ 1280）
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
    (1) 探測——3 個可靠視窗（≥ 60 樣本）平均 ≥ 1.5 ms 時往補償方向改 8～10 ms，等 2 s 再取 3 個視窗平均，落後往預期方向移動
    ≥ 步長一半才算 SteamVR 採用；不採用就退回起始值、本 session 停調，下一個 session 從推算值開始。(2) 追蹤——落後的指數移動
    平均（α 0.25）≥ 1 ms 時補一半、每秒 ≤ 3 ms，每個視窗先夾到 ±20 ms；session 內最多偏離起始值 ±40 ms。預設起始值＝一個
    週期＋30 ms；設定檔有值則固定。
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
    `steamvr.startDashboardFromAppLaunch=false` 無效（待查）。
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
        （120 Hz 約 5 ms）。丟一幀要多等一整個週期，多等幾 ms 一定比較好。
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

  6 ms 以內穩定省約 2～4 ms，再多 runtime 就把整幀往後排。頭盔目前設 5 ms。約 21～22 ms 是這個 runtime 的下限，
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
- **還沒驗的**：有人配戴時看起來如何（合成那一格的邊緣、快速揮動時的殘影、頭部平移造成的視差）；手把以外的動態物體
  （球、對手）；每眼 2160 時 GPU 的負擔。位移的搜尋範圍是全尺寸約 ±32 像素（每 1/60 秒），更快的物體不會被外插。
  合成只做「往前推」（外插），不是在兩張真的影像之間內插，所以不增加延遲。

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

**還沒做**

- **降低每幀的像素量**（10-07 晚結論一）：編碼尺寸小於算圖尺寸（已做）、注視點編碼（已做，見上）、10-bit（host 端已做，
  Frame 的解碼路徑不支援，見上）；先用每眼 1152／1440
  在原本位置完整跑幾分鐘，確認延遲與流暢度（要等姿態預測收斂），並請使用者戴眼鏡看清晰度能不能接受。
- 結論二、三與延遲拆解在 10-08 上午做了（見上）。剩：`always`＋§VR-LINK-FRESH 在真頭盔的弱訊號位置驗證、
  控制連線重連時換到另一個 host 位址、頭盔合成管線那約 21 ms 有沒有別的路可以繞。
- **和 Steam 內建串流比延遲**：它的 log 沒有數字，要請使用者開頭盔裡的進階效能圖表（`showAdvancedGraphs`）讀出來。
- **補幀**第一版已做（§VR-HALF-RATE＋§VR-SYNTH，見上），待使用者配戴確認；之後視結果加大搜尋範圍、處理遮蔽邊緣。
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
