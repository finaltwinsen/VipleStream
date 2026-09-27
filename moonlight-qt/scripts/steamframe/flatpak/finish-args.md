# finish-args：逐條理由與取捨

對應 `io.github.finaltwinsen.VipleStream.yml` 的 `finish-args`。**兩邊必須同步修改。**
設計依據：`docs/vr_architecture.md` §2.2（P1 Flatpak）、M2a 設計 §3.6 與 v2 修訂 R4。
範本是 Flathub `com.moonlight_stream.Moonlight`（master `234fb653`）的 15 條。和範本的差異都寫在各條的說明裡。

標記：
- **UNVERIFIED**：還沒在實機（Frame）或 S3 上確認；確認的 PoC 寫在括號裡。
- **收窄**：PoC 之後預期要縮小或拿掉。

## 1. 逐條說明

| # | finish-arg | 為什麼要 | 取捨／風險 | 狀態 |
|---|---|---|---|---|
| 1 | `--share=network` | 串流、配對、mDNS 探索、MP-QUIC 多路徑 | 無（串流 client 的本質） | 同 Flathub |
| 2 | `--share=ipc` | X11 MIT-SHM（fallback-x11 時 Qt／SDL 需要） | 和 host 共用 SysV IPC 命名空間；Flatpak 對 X11 app 的標準做法 | 同 Flathub |
| 3 | `--socket=wayland` | Frame 的 gamescope 與 builder 的 GNOME 都是 Wayland | 無 | 同 Flathub |
| 4 | `--socket=fallback-x11` | 沒有 Wayland 時才給 X11 socket（Xwayland-only 環境、舊桌面） | 有 Wayland 時不會暴露 X11 | 同 Flathub |
| 5 | `--socket=pulseaudio` | 音訊輸出（PipeWire 的 pulse 相容層） | 可以錄音（pulse socket 的通病）；app 不錄音 | 同 Flathub |
| 6 | `--device=all` | V4L2 m2m 解碼器 `/dev/video*`（PoC-3，L2 硬解的前提）、DRM render node（Vulkan／VAAPI）、`/dev/hidraw*`（§SC-HID 轉發 Steam Controller）、輸入裝置 | 範圍大，但 Flatpak 沒有 `--device=video`／`hidraw` 之類的細分可用；`--device=dri` 不含 `/dev/video*` | 同 Flathub；Frame 上沙箱內是否看得到 `/dev/video*` **UNVERIFIED**（PoC-0、G-PKG；`v4l2-probe` 直接回答） |
| 7 | `--talk-name=org.freedesktop.ScreenSaver` | 串流時抑制螢幕保護／休眠 | 無 | 同 Flathub |
| 8 | `--filesystem=xdg-run/gamescope-0` | gamescope 的 Wayland socket 名叫 `gamescope-0`，不是 `wayland-*`，flatpak 不會自動沿用；α 在 gamescope（SteamVR dashboard overlay）內跑需要它 | 只暴露 gamescope 的 socket | 同 Flathub；Frame 上 socket 名稱 **UNVERIFIED**（PoC-7） |
| 9 | `--filesystem=host-os:ro` | gamescope WSI layer：JSON 的 `library_path` 指向 `/var/run/host/usr/lib/libVkLayer_FROG_gamescope_wsi_<arch>.so`，要 host 的 `/usr` 才讀得到（Steam Deck 先例，Flathub `7d91dc21`） | 唯讀暴露 host 的 `/usr`、`/lib*` 等系統檔（不含 `/etc`、不含家目錄）；沒有個資，但讓沙箱能讀 host 的函式庫版本資訊 | 同 Flathub；**Frame 上沒有這個 layer 就拿掉**（PoC-7）。aarch64 版 layer 是否存在、檔名是否是 `_aarch64` **UNVERIFIED** |
| 10 | `--filesystem=xdg-config/openxr:ro` | SteamVR 設 active runtime 時寫 `~/.config/openxr/1/active_runtime.json` | **OpenXR loader 自己看不到它**：Flatpak 把 `XDG_CONFIG_HOME` 改成 `~/.var/app/<id>/config`，而這條權限是把 host 的 `~/.config/openxr` 掛在沙箱內**同一路徑**。所以由 `XrRuntimeJson::resolveActive()` 自己查 `$HOME/.config/openxr/1/…`，找到後在 `xrCreateInstance` 前（行程內）設 `XR_RUNTIME_JSON`，來源記進 xr-probe 的 JSON | 新增；Frame 上路徑 **UNVERIFIED**（PoC-F） |
| 11 | `--filesystem=xdg-config/openvr:ro` | SteamVR 的 vrclient 讀 `~/.config/openvr/openvrpaths.vrpath` 找 runtime 位置 | 唯讀；內容是路徑清單 | 新增；是否真的需要 **UNVERIFIED**，PoC-F 後收窄 |
| 12 | `--filesystem=xdg-data/Steam/steamapps/common/SteamVR:ro` | SteamVR 的 OpenXR runtime JSON 與 `.so`（`library_path` 指向這裡） | 唯讀；只暴露 SteamVR 目錄，不是整個 Steam library | 新增；Frame 上的實際路徑（原生 Steam、另一個 library 資料夾、SteamOS 佈局）**UNVERIFIED**（PoC-F／PoC-F-pre） |
| 13 | `--filesystem=~/.steam:ro` | `~/.steam/root`、`~/.steam/steam` 是指向 Steam 安裝位置的 symlink，SteamVR 與 vrclient 會經由它找路徑 | **會暴露 `~/.steam/registry.vdf`（含 Steam 帳號名）**；唯讀但仍是個資。**PoC-F 之後收窄**成實際需要的子路徑或拿掉 | 新增；**收窄** |
| 14 | `--env=LIBVA_DRIVER_NAME=` ＋ `--unset-env=LIBVA_DRIVER_NAME` | host 設的 VAAPI driver 名稱對 runtime 內的 Mesa／driver 不一定適用；清掉讓 libva 自己偵測。先設空再 unset 是為了相容舊版 flatpak（沒有 `--unset-env`） | 使用者想在 host 強制指定 VAAPI driver 時，要改用 `flatpak run --env=LIBVA_DRIVER_NAME=…` | 同 Flathub |
| 15 | `--env=LIBVA_DRIVERS_PATH=` ＋ `--unset-env=LIBVA_DRIVERS_PATH` | host 的 driver 路徑在沙箱內不存在或 ABI 不合 | 同上 | 同 Flathub |

`--env=` 只用來**清掉** host 的變數（不變式 7：不以環境變數當使用者設定）。任何功能開關都走 Settings UI 或 CLI 旗標。

## 2. 刻意不帶

| 項目 | 理由 |
|---|---|
| `--env=IGNORE_RFI_LATENCY_BUG=1`（Flathub 有） | C30：我們的 fork 在 `vaapi.cpp` 改讀 opt-in 的 `HAS_RFI_LATENCY_BUG`，這個變數沒有作用；帶了只會誤導 |
| `--env=QT_QUICK_CONTROLS_STYLE=Material`（Flathub 有） | `main.cpp` 已呼叫 `QQuickStyle::setStyle("Material")`，多餘；而且環境變數會蓋掉程式內的預設 |
| `--filesystem=home` | 範圍太大。probe 的樣本改複製到 `~/.var/app/<app-id>/data/samples/`（`frame-poc-collect.sh --samples` 會做），或只在測試命令列加 `flatpak run --filesystem=<dir>:ro` |
| `--filesystem=host-etc` | host 的 `/etc/xdg/openxr/1/active_runtime.json`（系統層級的 active runtime）因此看不到。SteamVR 寫的是使用者層級（第 10 條），先不開；PoC-F 若發現 Frame 用系統層級再加 |
| `--filesystem=xdg-run/monado_comp_ipc` | 只有 S1（Monado）測試要，測試時用 `flatpak run --filesystem=xdg-run/monado_comp_ipc` 臨時加，不進 manifest |
| `--filesystem=/run/udev:ro` | SDL／hidapi 在 Flatpak 內的 hidraw 列舉走 sysfs 應該就夠；§SC-HID 在 Frame 上若列舉不到裝置再評估（**UNVERIFIED**） |
| `--socket=session-bus`、`--talk-name=org.freedesktop.Flatpak` | 不需要整個 session bus，也不需要在沙箱外啟動程式（self-update 是 server 端的功能） |
| `--allow=devel` | 只有 perf／ptrace 偵錯才需要；需要時用 `flatpak run --devel` |

## 3. libdrm 決定（兩種 arch 都開）

- `vr_architecture.md` §2.1 要 x86_64 dev 開 libdrm（S1 在 AMD builder 上走 VAAPI → DRM_PRIME → `pl_map_avframe_ex`），§2.2 又寫 `disable-libdrm` 只套 x86_64（沿用 Flathub `97ae2a29df`：NVIDIA 上 DRM master hook 造成 Vulkan deadlock）。兩者矛盾。
- **決定：兩種 arch 都開 libdrm**（qmake 不帶 `CONFIG+=disable-libdrm`）。aarch64（Frame，Adreno／Turnip）本來就要 DRM_PRIME；x86_64 Flatpak **只給開發用**，不發佈，`build-steamframe.sh` 拒絕 `--arch x86_64 --flavor release`。
- 日後若要把 x86_64 Flatpak 給 NVIDIA 使用者，照 Flathub 在 x86_64 加 `CONFIG+=disable-libdrm`（manifest 的 viplestream 模組用 `build-options.arch.x86_64.env` 傳給 `build-viplestream.sh`，或另開 flavor）。

## 4. 已知風險

1. **Qt 6.11 是新變數**：Windows（`C:\Qt\6.10.x`）與 builder（系統 Qt 6.10.2）都在 6.10。KDE runtime 6.11 是 Qt 6.11.2。出現 Qt 6.11 專屬的問題時，退路是 `runtime-version: '6.10'`（同為 freedesktop 25.08 基底、仍在維護）；manifest 與 `build-steamframe.sh` 的 `KDE_BRANCH` 一起改。
2. **`~/.steam:ro` 暴露 `registry.vdf`**（第 13 條）。PoC-F 確認 SteamVR 實際需要哪些路徑後收窄。
3. **gamescope WSI aarch64 layer UNVERIFIED**：SteamOS ARM 是否提供 `libVkLayer_FROG_gamescope_wsi_aarch64.so`、放在 `/usr/lib` 還是別處，都沒有證據。沒有時 Vulkan loader 只會略過（無害），但第 9 條 `host-os:ro` 就沒有存在理由，要拿掉。`frame-poc-collect.sh` 會列出 host 上的 layer 檔案。
4. **OpenXR runtime 在 KDE runtime 沙箱內能不能載入**（R2）：SteamVR 的 runtime `.so` 依賴 Steam Runtime 的函式庫，和 KDE 6.11 runtime 的 glibc／libstdc++ 是否相容 **UNVERIFIED**。先用 PoC-F-pre（S3，`moonlight-qt/scripts/steamframe/s3/`）在 builder 上驗證機制，Frame 上由 `xr-probe` 回答（失敗時 JSON 內有 `dlopen` 的 `dlerror()`）。
5. **libplacebo 選 B（v7.360.1）**：沒有 `VK_KHR_internally_synchronized_queues`，所以不需要 Flathub 的 gamescope 補丁；但和 FFmpeg 9.0 的 libav helper 能不能編過 **UNVERIFIED**（S1 第一次建置時確認）。編不過就退回 A（Flathub 的 `4d82c689`＋補丁，補丁一定要帶，否則 gamescope 下 SIGABRT，見 moonlight-qt#1925／#1930）。
6. **hardening 旗標**：`buildsystem: simple` 不會自動注入 SDK 的 CFLAGS／LDFLAGS；`build-viplestream.sh` 明確傳 `QMAKE_CFLAGS+=`／`QMAKE_CXXFLAGS+=`／`QMAKE_LFLAGS+=`。picoquic／picotls 的 CMake 會自己讀環境變數 `CFLAGS`。
7. **appstream-compose: false**：省掉 appdata 的嚴格驗證（`<id>` 非 reverse-DNS 等）。上架 Flathub 時要打開並修 appdata。
8. **`--device=all` 在 Frame 上的 uaccess**：SSH（Developer Mode）執行 `flatpak run` 時，`/dev/video*` 的權限是否因沒有 active seat 而不同 **UNVERIFIED**；`v4l2-probe` 開檔失敗時會記 errno、mode／gid 與 `getgroups()`。

## 5. 測試時才加的權限（不進 manifest）

```bash
# probe 讀樣本（或改用 frame-poc-collect.sh --samples 複製進 ~/.var/app/<app-id>/data/samples/）
flatpak run --filesystem=<樣本目錄>:ro io.github.finaltwinsen.VipleStream decode-bench <樣本> --json <路徑>

# S1（Monado）
flatpak run --filesystem=xdg-run/monado_comp_ipc io.github.finaltwinsen.VipleStream xr-probe

# PoC-F-pre：SteamVR 不在預設佈局時（例如 Flatpak 版 Steam）
flatpak run --filesystem=<SteamVR 目錄>:ro io.github.finaltwinsen.VipleStream xr-probe --xr-runtime-json <runtime JSON>
```
