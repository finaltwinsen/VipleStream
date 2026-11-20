# VipleStream — 待辦清單

只列**尚待 / 進行中 / 等硬體 / 等驅動**的條目。
已完成項目 → git log。歷史背景 → git log 與 docs/。

---

## 優先級總覽

| 優先級 | 條目 | 待做事項 |
|---|---|---|
| **Active（M0～M4a 與 2.0.x 已發佈；剩下列尾項）** | **§VR** 2.0 Steam Frame（桌面模式 + PCVR） | 設計見 `docs/vr_architecture.md`、`docs/vr_protocol.md`、`docs/steam_frame_client.md`、`docs/steamvr_driver.md`；完成史在 git log。近期待辦見下方各日期章節與「§VR 補充」。**早期留下、還沒收掉的（多數要核對是否仍成立）**：(1) V4／V5 未解——9 次中 3 次 vrcompositor 自己重畫第 0 層（T4 圖案解不出）；消費端停止／standby 時 vrcompositor `AcquireSync` 逾時、`ReleaseSync 0x887a0001`，根因未明；維護 tick 每 60 s 建 D3D11 device 對串流的影響未量；RTSP dynamicRange 覆寫在現行 client 下觸發不到。(2) V5／V6 延後——註銷 hook／`--steamvr-driver uninstall`、T5／T7、PoC-10、PoC-5a（selftest `--preset` 未實作）；G-DRV 2（T4 不經 manual 的 1 h hold、`reproj` 獨立量測、Present 間隔 p99 要逐幀 log）、3／3'／3''（要 `--probe space` 與 `gdrv:chaperone-offset` preset）、4b／4c、5（空白注入）、6（T7）；`--attach-session`、`--only cleanup`；stub 模式下 guard 不還原；session 結束後 SteamVR 不會自動關（待核對）；上行 tracking 在下行斷線後改走 QUIC 且不回 ENet；GPU copy timestamp 抽樣；S3-09 重定值待確認（vr_architecture §8.2）。(3) XR 桌面——XR 模式平面 backend 仍建 swapchain、開場 shader 預熱；VAAPI 比軟解 R +3 的固定偏移（Frame iris 待實機比對）；虛擬鍵盤 Caps 鎖定、數字鍵盤；Frame 左手面鍵幾何與左 view 開關實機確認。(4) 配戴 A/B——A4 overscan 0 vs 3；錯配若真的發生，再把 release 與 publish 併進同一段 queueMutex；§VR-REARM 的 Frame 實機驗證。(5) Windows CLI 串流行程被強制結束後可能停在結束階段（剩 1 條執行緒、`taskkill` 殺不掉、鎖住 `temp\moonlight`；打包腳本已改成把鎖住的檔案移到 `temp\moonlight.locked-*`），根因未查。(6) 實機 PoC-2b／G-β、PoC-F-pre（要使用者在場）。(7) 使用者端待決（10-03，待核對）：適配器移位、Frame 家用 Wi‑Fi 是否鎖定 BSSID。(8) 既有問題（非 VR，待核對）：`session.cpp` 的 `QHostAddress::isPrivateUse()` 要 Qt 6.6（WSL 的 Qt 6.4 編不過）；`build_all.cmd` 可選的 WSL AppImage 步驟一定失敗；benchmark harness 寫的是舊路徑 registry key。見下方「§VR 補充」 |
| **Active (已建置，逐項驗證大多還沒做)** | **§PERF-R2** 第二輪效率審查（2026-11-13～14） | A 節 49 項（不需實機就能改的）：47 項完成、2 項做了一半（A15 的 AVCodec／PyroWave 那一半、A21 的設定頁 VR 區 Loader），沒有略過或撤回的。Windows client／server、Android、頭盔 aarch64 都建得出來，Windows client `--help` 連跑兩次 rc=0；頭盔 11-14 兩場無人、三場配戴整場正常（注視點分塊走頂點屬性那條路）。**還沒做**：macOS server 的建置、依總表逐項驗、A25／A39 的畫面逐像素比對。B 節 32 項（要實機量測或配戴才能決定）還沒動。細節見下方「§PERF-R2」一節與本機 `temp/perf_review_1113/r2fix_00_summary.md` |
| **Active (已實作並驗證 Windows；XR 桌面待 Frame 實測)** | **§H.4-AUTO／§H.4-SWITCH** client 解析度／更新率「自動（本機螢幕最佳）」＋host VDD 跟著切 | 2026-10-01：server `/serverinfo` 在 dd 開啟且 `output_name` 對得到已啟用裝置時回報該裝置**可切換的最高模式**＋`<DisplayModeSwitchable>1`（`EnumDisplaySettingsW` 列舉、快取 10 秒；否則退回 primary 目前模式）；client 解析度與 FPS 各加「自動」（新安裝預設；既有設定保留原值），串流時取串流視窗所在螢幕的桌面模式，與 host 可切換最高模式取小，預設位元率跟著重算；CLI `--resolution auto`／`--fps auto`。**已驗**（`<host>` VDD 閒置 1080p180）：serverinfo 回 3840x2160@244；1440p120、4K120 host 都切到要求模式、結束回 1080p180；自動在 1080p180 client 串 1920x1080@180、位元率 35 Mbps。S1（linux-builder x86_64 Flatpak＋無頭 Monado，runtime 20 Hz）：XR 桌面自動解析成 2560x1440@20、位元率 13 Mbps；順帶發現要求的 Hz 不在 VDD 模式表時 Windows 設模式整個失敗（1610）→ 加 §H.4-SNAP（顯示器改設 ≥ 要求值的最小可用 Hz），重測 host 切到 2560x1440@60。設定頁「自動」兩個項目已目視確認並補 zh_TW 翻譯（2026-10-02）。**待做**：XR 桌面自動在 Frame 實測（Frame 實際 72 Hz，預期 host 顯示器 snap 到 90 Hz、串流 72 fps）。 |
| **Active (已實作；之後已發多版，系統匣的真實更新還沒有人走過)** | **§SELF-UPDATE** host 自我更新（tray 兩段式 + CLI） | 2026-09-22 實作：HEAD-302 查版、GitHub asset digest 必驗、Windows 腳本兩段式 rename 回滾／Linux apt 路徑；Windows standalone 與 Linux CLI 皆已端到端驗證。**待做**：(1) 下一次 release 後在 .226 與 linux-builder 用 tray 走真實「Install update vX」；(2) release 流程產生 SHA256SUMS 並簽章（目前信任 GitHub asset digest + TLS）；(3) Windows 可對 exe 做 Authenticode 驗證；(4) Linux 附 polkit .policy 讓 pkexec 提示可讀、避免 root 執行使用者目錄腳本。 |
| **Active (儀器已上線，待實測歸因)** | **§INPUT-STALL** 鍵鼠斷續（畫面正常、短暫失控後恢復） | 2026-09-21 兩端全管線儀器化後，合成鍵鼠測試四個量測點皆 0；但使用者真實 session（09-21 19:26 場）client 主迴圈 26 次停頓、每次固定 ≈2016 ms、phase 全為 `SDL_WaitEventTimeout`（卡在 SDL 內部，log 前後無觸發事件），host 四種標記皆 0。v1.5.275 起停頓 ≥300 ms 會抄主執行緒堆疊（dbghelp，SDL3.dll 只解到匯出符號）→ 下次發生對照 `[VIPLE-INPUT-STALL] main thread stack` 定位是 hidapi 列舉／DirectInput／WGI／訊息泵。外部嫌疑：client `AsusIMEService.exe` 崩潰迴圈（LL keyboard hook）、host Steam 被 SC-HID feature 風暴卡 10.7 s／輪。候選修法：client SC 無活動裝置時對 feature request 立即回「無裝置」讓 driver 解除 400 ms 閘控、server N 次未回應後停止轉發；§K.14 開機 IDR 重試改用 per-encoder 計數（reinit 後目前永不再觸發）。 |
| **Active (已修並驗證，剩後續項)** | **§M01-D** app 被殺後立刻重開必 -101（殭屍 session） | 2026-09-23 修正並驗證：host 在同一個 client（同憑證 uuid）發起新的 /launch 或 /resume 時先終止舊 session 並退役它的 QUIC 連線（`§M01-D superseded` / `retired QUIC connection of superseded session`），pending launch 由同 client 的新 launch 直接取代；client 在 -101 前多印一行 `§M01-D no decodable frame` 診斷。Pixel 5 與 Windows client「強制關閉後 2 秒重開」各 3/3 在 3.5–5.1 秒出畫面、無 -101；QUIC 場次 host 的 `WSASendMsg failed: 10022` 也從每場數十次降到 0。**後續**：(1) 更深一層的「B 線」防護（relay 路徑、較舊 session 不得接手）已做成 patch 未合入（scratchpad `m01\D_bline.patch`），需使用者同意變更 host 管理設定；(2) 同一 NAT IP 後有多個 QUIC client 時每個 IP 只有一條 QuicSession，需要 per-connection 綁定；(3) 語意改變：同一張憑證同時開第二條串流（GUI＋CLI、兩台裝置共用憑證）會把第一條擠掉，2.0 VR 若要同 client 多 session 須回頭檢討；(4) -101 對話框「網路連線品質不佳」的文字是否改寫（Android `values*/strings.xml`、Qt `session.cpp`）|
| **Active (已修，待有音訊時補驗)** | **§M01-A** debug 版音訊 FEC assert 中止 | 2026-09-23 Pixel 5 debug 版 AudioRecv SIGABRT（`RtpAudioQueue.c` `LC_ASSERT_VT(blockSize 相同)`，前一行 `Audio block size mismatch (got 348, expected 336)`）。已把 `RtpAudioQueue.c`／`AudioStream.c` 對網路輸入的 assert 改成 runtime 防衛（行為對齊 release：丟棄該 block、不 abort），log `[VIPLE-AUDIO] §M01-A …`。驗證那輪 host 沒有音訊流量（遊戲崩潰、靜態桌面），**修正尚未在實機上被觸發**：下次 host 有遊戲音訊時，用 Pixel 5 debug 版做斷線恢復再確認 0 次 abort |
| **Active (verify)** | **§β.11.b** warp edge MV threshold | Settings UI slider (2-20, default 8) 已上線 v1.4.195；setting 實機 confirm 生效 (2026-05-23, registry vkfrucEdgeMvThreshold=8 + ctor log)；剩主觀畫質比較：keep 8 / 試 4（更多邊緣保護）/ 試 12（更 smooth）|
| **Active (Phase 2.1 完成 / v1.5.249 61f8b0fd)** | **§FRUC-VALIDATE** FRUC 科學驗測方案（科學數據優先、體感降第二線）| drop-and-reconstruct vs GT 取代體感。**Phase 1 已 commit (4833c02d / v1.5.247)：** app 內 `fruc-offline` CLI harness（走真 renderFrame(AVFrame*) + §B-DUMP）+ vkfruc.cpp SW-dump null-timeline-sem crash 修。**Phase 2.1（2026-06-17，scripts/ 本機）強結論：** 結構化內容（make_shapes_pan 硬邊+平滑底剛性平移）+ 公平 NV12 控制（用引擎自己 dump 的 real 幀，修 measurement asymmetry）+ paired bootstrap CI + 驗過的 oracle（warp 正確性 ✓、光流改用閉式解因 Farneback 失效）+ gate 敏感度負對照下，**RIFE@256 在全部 4 標準指標（PSNR/SSIM/IE/LPIPS）+ temporal 一致性上統計顯著勝過 naive-blend，且優勢隨運動難度成長**（slow 36px→fast：PSNR +2.73→+4.08）。**誠實：** block-match-8 在 12-36px 全域運動誠實輸 blend（標準指標），只在 M3 edge-IE（獎勵 source-res 銳利邊）贏；inferDim 是關鍵 cross-engine 共變量（128→256 翻 3 指標）。tooling：fruc_metrics/decimate/compare_dump/oracle_pass/inject_regression/run_validate_round（gitignored，跑 .venv）。**seq-VMAF 已加**（RIFE@256 68 vs blend 42）。**NVOF 改走真實串流驗測成功**（取代 SW renderer 手術——判定那不是小修）：register win-builder + .226 host，throwaway client 串流 TIER=5 → log 證 NVOF execute + 餵 warp（chainBusy ~1.6ms 最快）。**6 引擎覆蓋：** Vulkan 家族 3 引擎完整（block-match/RIFE GT-quality + 全 3 真路徑 perf，NVOF 解鎖）；**D3D11 家族（Generic/NVOF/DirectML）此自動化 session 不可達**（client 硬選 VkFruc，需互動桌面）。**剩：** D3D11 家族（互動桌面）、L1 端到端 GT-quality、跨 build σ。|
| **Active (verify, hw-blocked)** | **§FRUC-XPLAT** 非-NVIDIA RIFE 跨平台啟用 | **已 ship v1.5.249**（1b213e8f / ca05f64b / e4523449 / 7aa47d62）：非-NVOF 設備直起 RIFE@128 + 非-NVIDIA 自動啟用 native RIFE（vendor-gate 綁 hardware 0x10DE，避開 NV 的 device-lost）+ inferDim cap 128 + overlay 顯示真實補幀引擎名。AMD RADV live stream 已證 RIFE 真 engage（frucMode=1 / rifeNative=1 / T3 穩定）惟當時跑 inferDim=256（cap commit 前）。**剩：** inferDim=128 在 AMD 的經驗驗測（offline + 串流 + fps + 補幀數）卡在 **AMD GPU errno -13**（headless 筆電蓋著→logind seat-ACL 缺→amdgpu ACCEL_WORKING 失敗→llvmpipe）。決定性值已 deterministic+committed。**(B) 啟動瞬態不降階已修（§R2-ζ-3b，v1.5.249）：** extreme-floor 加 3s startup grace（只跳「即時 floor」，保留 `>=20 frame` 漸進 demote）→ 修啟動 decode 尖峰（如首 IDR ~481ms）誤觸 T3→T0、~12s 模糊。build OK；live .226 串流（NV_OF=0）確認**無回歸**（T3 全程穩、FRUC active、零 floor）；惟正向冷啟動尖峰未重現（暖機+idle 桌面 decodeMean 8ms<<門檻），grace 擋尖峰屬 code-evident，待冷啟動/AMD 實機正向驗。**(C) Windows AMD/Intel D3D11VA staging bug 仍未做** |
| **Planned (designed, 未動工)** | **§FRUC-GENERIC** 通用 block-match 畫質改良 | plan-mode 計畫已成形：分階段 in-place 改 Vulkan block-match（1A 邊緣不退 crossfade→occlusion-gated 單向 warp／1B in-shader sub-pixel parabolic refine／1C bilinear→Catmull-Rom），每步用 §FRUC-VALIDATE 離線 GT harness 客觀量、贏過 naive blend 即停。根因已讀碼確認（warp 邊緣三層 fade 退回 crossfade＝在邊緣做 naive blend，是輸 blend 主因）。零新 dispatch、binding 不變、純 fp32 跨廠商 |
| **Active (verify + polish)** | **§Q** MP-QUIC 多路徑網路聚合 | 主體與 Fix L–Q.5、ABR（v1.5.133～v1.5.265）都已 ship，完成史見 git log。**待做：** 壓力測試 1440p120。 |
| **Active (shipped v1.5.258, 主線驗畢)** | **§Q-REGAIN** failover 後路徑自動回歸 | 已 ship，五輪 failover 回歸測試驗畢（`scripts\failover_regain_test.ps1`，不入 git）。**剩（不阻塞）：** 乙太瞬斷外因（switch/ARP 層，兩端事件檔無 link 事件）；VPN 路徑持續 loss 機理（原本懷疑 Tailscale MTU 1280 黑洞；2026-11-13 讀碼更正：QUIC socket 沒有要求不分片，client 用 IPv4 Tailscale 位址（100.64/10）時 >1280 B 的封包由本機分片送達、§K.9 的假釋會通過，不是黑洞；會黑洞的只有 client 用 IPv6（fd7a:）位址的路徑，那一種已改成一開始就不 boost（§K.9-TS6，還沒實機驗）。所以這裡的「持續 loss」要另找原因）；ghost-slot 現象（picoquicDeleted slot 的 socket 仍收視訊，§K.10 歸因失真）；server 端 set_path_status 翻轉 client 本地 path_is_backup 而 tracked cache 不知情（Pass 3 既有缺口） |
| **Active (verify)** | **§SLIM** release zip 瘦身 (106→48 MB) | phase 1-4 已 ship。**Lazy fetch 兩路徑皆已接 ModelFetcher（2026-05-29 程式碼確認）：** NCNN (ncnnfruc.cpp → ensureRifeModelDir) + Vulkan Native RIFE (vkfruc.cpp:9324 在 RifeNativeExecutor::initialize 前呼叫 ensureRifeModelDir，回傳 empty→block-match fallback 不 crash)。`ensureRifeModelDir` 已 expose 到 modelfetcher.h 兩端共用。**剩（環境受限）：** 無 VC++ Runtime 的乾淨 Win10 系統實測 missing vcruntime140 提示 |
| **Deferred (hw-bound)** | **§B Phase B** HEVC D3D11VA → Vulkan composite | Code 已 ship v1.4.184-185；阻塞：AMD 780M 測試機 HEVC D3D11VA 本身不可用。需換另一台 D3D11VA HEVC HW decode 可用的 AMD 機器驗 B7 import + B9 FRUC chain |
| **Active (test pending)** | **§B-NVOF autotier** NVOF 成為 NV 最佳 tier | Code 已 ship v1.4.117-138；待使用者 PixArk 20+ 分鐘實測，看 NVOF-PROF drop% 是否接近 0%、chain_mean 是否穩定 < 2ms。若 OK → reapply early-kickoff + NVOF 列 NV best tier；若 drop% 高 → 需 Option E skip 機制 |
| **Active (follow-up)** | **§R2 PASSIVE FRUC** ratio controller | v1.4.169-186 已 ship（T2→T0 跳降、display Hz=0 fallback 都已確認）。剩：進 2x 後 server recv ~180→~140 是否仍有（alignment gate 應已消除，待串流複查）|
| **Active (verify)** | **§M.parity Android** wire/UX | v1.4.160 ship W1-W5+U6+U7。**Pixel 5 v1.5.202 實機驗 (2026-05-29)：** 串流 .226 桌面成功，codec 協商正常（c2.qti.avc/hevc.decoder.low_latency 選用）→ **W1 server-cap 交集邏輯確認 live**。`[VIPLE-PARITY]` 遮蔽 log 為條件式（僅當 server SCM 排除 client codec 時才印）；本次 Pixel5 codec ⊆ .226 caps 故未觸發＝正解（無需遮蔽）。剩 W2-W5/U6-U7 UX 流程主觀驗 |
| **Active (行為決策待定)** | **§K.dd.select** 顯示器選擇 | **2026-09-18 已完成（→ git log）**：.226 重設 `output_name` + `dd_configuration_option = ensure_only_display`，實測 12.9s 乾淨重啟、**沒有重演 v1.5.200 的 35 分鐘卡死**（§K.dd.fallback 守衛有效），拓樸成功切到 VDD、`Display refresh rate [180Hz]`。同步修掉 `refresh_displays()` 「舊顯示器消失後不再重試 output_name」的 fall-through，並補上整條路徑的 `[VIPLE-DD] §K.dd.select` 診斷。**剩下的行為決策**：`validate_encoder()`（`video.cpp`）在指定的 `output_name` 映射不到時，目前只警告、仍拿預設顯示器做編碼器探測。要不要改成直接讓探測失敗？好處是不會拿錯螢幕的 HDR／多 GPU 能力當結果，風險是本來能串流的主機會變成「找不到編碼器」 |
| **Active (P0)** | **§N.5.bug** Android 檔案傳輸 SSL 錯誤 | 待使用者在 Android 重現 + 拿 server log，確認錯誤在哪個階段送出。**2026-11-14 起診斷用的行是取樣**：`[VIPLE-VERIFY] OK` 每台 client、`[VIPLE-XFER] poll ENTER／EXIT` 每個來源位址，各自第一次＋每 10 分鐘一組 info；`[VIPLE-VERIFY] enter` 是不分 client 的取樣。預設等級下看得到「這台 client 的握手驗證通過、handler 有進來、有寫完」；要分辨「verify callback 根本沒被呼叫」或逐次比對，**重現前先把 host 的 `min_log_level` 設成 debug**。失敗路徑的行（DENY、401、200_cmd、EXCEPTION）沒變 |
| **Active (verify)** | **§N.5** Android FileTransferClient runtime | 待實機在串流 session 跑 Send/Receive flow，確認 Pixel 5 Android quirks |
| **Active (long-running)** | **§J.3.e.2.i.8 Phase 2.5** FRUC native source | 殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Active (β.10)** | **§J.3.e.X Path β.10** Linux/AMD/Intel 覆蓋 | §β.12.fix v1.4.193 PASS；剩：視覺品質主觀確認（tiled Conv vs coopmat）+ Intel iGPU 驗測 |
| **Deferred (driver-bound)** | **§K.4** Wayland portal teardown | `Restart=always` 緩解已 ship；需可重現串流環境（有 GPU 的 Linux 機）才能根治。**§K.4-DESKTOP-ID（2026-07-22 已部署驗證 ✓）**：查證 qtbase/qtwayland 全樹——Qt 從不讀 `QT_DESKTOP_FILE_NAME` env，舊 hygiene fix 是 no-op（desktopFileName 一直為空，`_GTK_APPLICATION_ID` 落到 organizationDomain fallback `com.moonlight-stream.viplestream`，實機 xprop 證實）。已改為 QGuiApplication 建構前 `setDesktopFileName("viplestream")` + Linux desktop id 整組改 `viplestream`（.desktop/.appdata 改名、app.pro 圖示以 viplestream.svg 名稱安裝、SDL WMCLASS 對齊）。AMD 機重建部署後 runtime 驗證：`_GTK_APPLICATION_ID=viplestream` ✓、startup portal register 錯誤 0 筆（spam 根治）✓；箱上另一毒源 stale `icon-theme.cache`（192B）已清。剩：使用者目視 dock/app-grid 圖示；server-side PipeWire portal teardown 是不同層問題，仍 deferred |
| **Medium** | **§K.2** Raspberry Pi 5 client (aarch64) | aarch64 建置阻斷已隨 §VR M2a 解掉（樹莓派 5 現在是 arm64 原生建置機）；Pi 5 當串流 client 本身沒有驗過（待核對）；FRUC 全 disable |
| **Low** | **§J.3.e.Y 4Y.5b** native RIFE activation | 256×256 chain +1.7ms 負收益，revert；重做要先解 §β.5.3 D 全套 |
| **Low** | **§H.4-perf** VkFrucRenderer AMD draw-time | m_FrucMode=false ~13ms 偏高；需使用者提供 GPU-PROF log 才能精準改 |
| **Low** | **§A.2 / §A.8** WiX installer / 內部命名 | 沒用 MSI 出貨，優先級極低 |
| **Low** | **§D** HelpLauncher URL | docs/ 已寫；等 doc site 立起來再換 URL |
| ❌ **Won't-fix** | **Linux mouse scale** | 使用者明確放棄，維持現狀 |
| ❌ **Won't-fix** | **NVOF SDK buffer 輸出** | SDK 硬體限制，無法做 buffer 變體 |

---

## 各章節待辦補充

### 2026-11-20 傍晚：使用者在家配戴，VR 比 Steam 內建串流糊（server／頭盔 2.0.253）

- **§VR-QP-ROOM**：線路有餘裕時，畫質門檻從「QP 20 停止回升、QP 15 回收」改成 10／6。配戴前後：16～47 Mbps（QP 10～18）→
  100～150 Mbps（QP 3～9），使用者「好多了」。2.0.254 起「有餘裕」不看遠近（原本用 RTT 基線 ≤ 10 ms，使用者指出這是在分區網與遠端）：
  只看 30 秒內沒有因為網路降過碼、而且還沒塞過車或碼率低於塞車點的八成。**沒驗**：2.0.254 的判斷沒有實際場次跑過（只有單元測試）；
  中等頻寬的線路會不會在八成附近來回、計量線路（手機熱點）多用多少流量。
- **§VR-SHARPEN**：頭盔端分塊路徑對亮度做對比自適應銳化（Steam 的客戶端有銳化、我們原本沒有）。配戴「差不多了」。
  強度寫死在 shader（`mix(8, 5, 0.6)`）；**待做**：要不要做成設定、半速補幀那條路（`kTilesSynthBody`）沒有加、頭盔多耗多少電沒量
  （11-10 量過高碼率約 11 W、Steam 約 9.4 W）。
- **host 供不出畫格被當成網路不穩**：這次是我留在 host 的四個測試播放器佔了 CPU／GPU，合成器漏格 → 頭盔重複顯示 → 自動半速
  （`[VIPLE-VR-RATE] network unstable: short repeats`）。兩條連線的空檔同時出現＝來源端沒出畫格。**待做**：自動半速要分得出
  「host 沒出畫格」和「網路送不到」（host 已有 `VR-DRV timing 10s … missed=`）。
- **2D XR 桌面從頭盔介面啟動時碼率沒重算**：先照 1440p60 算出 40 Mbps，解析度被 host 壓到 1080p、幀率之後才改成 144，碼率停在 40 Mbps
  （命令列啟動一開始就知道 144 Hz，同場景是 62 Mbps）。`session.cpp` 的 `[VIPLE-RES] auto fps (XR desktop)` 那一段。**待修**。
- host SteamVR 裡「The Thrill of the Fight」的每款解析度是 78%（每眼 1900），依使用者指示改成 100%（備份 `steamvr.vrsettings.bak-1120-thrill78`）。
- 在別台機器開的程式要在那一台確認真的關掉（`scripts/benchmark/pace_run.sh` 用 SSH 的 Stop-Process 沒關掉 host 的 ffplay，`-autoexit -t` 對 lavfi 也沒生效）。**待修腳本**。

### 2026-11-20 午：清 TODO 這一輪（server 2.0.248／client 2.0.249）

做了什麼（細節看 git log 與 `temp/specs/2026-11-20/` 四份設計）：§IDR-FAST 審查後的兩個修正、§P2 第 1／2 期（壅塞分級、分級降碼、
壅塞時的 IDR 間隔、塞車點、client 等 IDR 的計時重試、凍結看門狗跟著 RTT 放寬）、§HOTSPOT-ADAPT 第一步（兩端）、§HOTSPOT-MISSING、
§INPUT-STALL 的證據保存、一批小修正。已無人驗：三種注入丟包型態（頭盔 → host）、600 ms 注入延遲的降碼與回升、連不上熱點的 host
（開發機）兩端的時間軸。

**還沒驗／還沒做**
- §P2：只用「單純加延遲」驗過（不是真的頻寬不足，降碼救不回延遲，會一路降到下限）。真的限速（頭盔端要 `ifb`）還沒試；
  L1 太容易進不進得去要看實際 Wi-Fi 場次的 `[VIPLE-ABR] cong level` 行數。第 3 期（即時延遲下界、2D 用 FRAME_FEEDBACK、QUIC-only 的 RTT）沒排。
- §IDR-FAST：「10 秒內第 4 張起間隔 1500 ms 並到期補送」那一支，注入的間隔剛好 ≥1.5 秒，沒真的走到延後。
- client 計時重試（`§IDR-RETRY`）只在延遲場次觸發過一次；`§FRZ-WD-RTT` 沒觸發過（要 RTT > 750 ms）。
- §HOTSPOT-ADAPT 第 3 步（兩端的狀態回報，2.0.252）驗過兩個場景：掃不到熱點的 host 第 23 秒關熱點、host 收到 CLOSED 就停止重試；
  連得上的 host 8 秒加入、熱點保留。**沒驗**：靜態原因（host 沒有無線介面）與下一場的 QUERY、回報遺失時退回 45 秒、連開兩場時上一輪的判定
  有沒有漏給新場次、舊 client／舊 server 混用。`docs/vr_protocol.md` 的 wire 表還沒補這兩則訊息（0x0C、0x10、能力位元 0x800）。
  設計文件 S1／S2／S4／S6／S7 沒做。
- host 常駐圖示的更新摘要、QUIC `SO_RCVBUF`、Linux 更新的 polkit 政策檔（2.0.252）：都只確認建得過（.deb 裡有政策檔與 helper）；
  摘要要等下一次 release 才看得到、polkit 對話框文字要在 Linux host 上實際走一次更新。Windows 通知的中文標題長度要實機看。
- STUN 重量：沒做（探測 socket 綁在控制埠上，重量會和串流搶埠；乾淨的做法是由控制埠的 ENet socket 自己送 STUN，要動 `stream.cpp`）。
- Linux x64：2.0.252 的 AppImage 與 .deb 在 CachyOS 容器建得過（順手拿掉 metainfo 裡已 404 的上游截圖網址）；沒有裝起來跑過。
- §HOTSPOT-MISSING（host 重啟網卡後查不到介面就整場不再連）：改了但沒重現過（要網卡閒置約一小時才會走到提早重啟）。
- §INPUT-STALL：`input-stall.log` 與兩次堆疊、等待鏈還沒看到實際輸出；2 秒停頓的來源仍未找到（三個嫌疑見設計文件）。
- 2D 丟包補傳（§FLAT-REPAIR，2.0.251）：注入 8% 隨機丟包 5／5 補回；真實負載 5 分鐘（頭盔 → 開發機、113 fps）5 次丟包全補回、
  沒有要 IDR、0 丟幀。**沒驗**：RTT 超過門檻的連線（應該自動不用）、補傳額度用完、host 分批送一幀時尾端誤報、Linux host（沒有這條路）、
  Android client、CachyOS client。單網卡的區網 2D 場次現在都改走多連線的 hub 送影像，使用者日常串流要留意有沒有異狀。
- 頭盔虛擬鍵盤的 Caps 鎖定與數字鍵盤（2.0.251）：只確認編得過、裝得上；版面與 Caps 的視覺提示待配戴看，按鍵碼可用
  `--xr-dump-keyboard <png>`＋`--xr-test-keyboard "a{caps}ab{num}5+{close}"` 無人驗（還沒跑）。host 端 numpad 鍵碼的掃描碼對應沒讀過。
- `--steamvr-driver uninstall`（2.0.251）：實作了、從沒執行過（會動 host 的 SteamVR 註冊，要使用者同意才試）。
- web 設定頁的「串流控制選單」開關、找不到頭盔時 `xrGetSystem` 的重試（只在頭盔上等）：沒實測。
- 開發機 server 的 log 要用旗標檔取：`temp\deploy_local.logs` 放著再 `schtasks /Run /TN VipleStream-DeployServerLocal`，複製到 `temp\devsrv\`。

### 2026-11-20 早：§IDR-FAST 丟包後 2 秒凍結（server 2.0.245，已提交 v2.0.245）

- **修正（`video.cpp` §IDR-FAST）**：平常（10 秒內送出的 IDR 不到 3 張）間隔縮到 250 ms、落在間隔內的請求到期補送（距上一張不到 35 ms
  的成對請求仍丟掉）；10 秒內已送 3 張就回到原本的 1500 ms 與丟棄。VR session 不走這裡。
- **待確認**：使用者實際串流的體感；丟包連發時（退回 1500 ms 那一支）還沒實測。
- 開發機 server 部署：`schtasks /Run /TN VipleStream-DeployServerLocal`（使用者已登錄的最高權限排程工作，`scripts\setup_local_deploy_task.ps1`），
  結果看 `temp\deploy_local_elevated.log`。

### 2026-11-19 晚：使用者四台實測、§HOTSPOT-VERIFY（client 2.0.244／server 2.0.243，已提交 v2.0.245）
- **§HOTSPOT-VERIFY**（對象是掃不到頭盔 6 GHz 熱點的 host，例如開發機）：有多連線時只認「這一場有一條已確認的連線走熱點」（`LiVrLinkOnDedicated()`），
  45 秒內沒有就關；並記住這台 host 沒連上（資料目錄的 `hotspot-nojoin`），30 分鐘內再連它不開熱點也不等。
  **還沒做**：連這種 host 的第一場仍要等 45 秒；server 端連不上時會一直每 2 秒重試。
- **頭盔測試捷徑已刪**：無人場次改從正式捷徑啟動（頭盔正式啟動器加了本機掛勾，重跑安裝腳本會消失）；`scripts/vr/t2d.sh` 可對任意 server 跑 2D 無人場次。

### 2026-11-19 凌晨～上午：第三輪審查（agy Hydrogen／Sonnet／Argon／Neon）、今晚驗證、§STREAM-MENU（已提交 v2.0.242；client 2.0.239／server 2.0.240）

**11-19 中午補：Neon 剩下 12 個單位審完（49 個全數完成），第三輪修正（client 2.0.241）**

- 沒驗到：`vrnetmode.h` 的目錄權限與狀態檔讀取（要「只用適配器」模式的場次）、放生路徑（要讓解碼器卡住）、H.264 SPS 改寫路徑。

**11-19 下午：第二、三輪留下的 88 項未處理全數查證，需設計的一併處理（client 2.0.242）**

- **待實機確認**：提權安裝的更新後重開是不是一般權限（下次真的更新時看）；mDNS 探索與 App 清單排序；解碼器卡住時的放生（要讓硬體解碼器卡住才走得到）。
- **待量測再決定**：等 `REFRESH_START` 期間每個新掉幀都立刻重送 LOSS（每幀兩份、重送計數不累加，500 ms 才退回 IDR）。改成只擴大範圍、照 2×RTT+20 ms 重送會把退回 IDR 提前到約 3 個重送間隔，要用丟包場次比較。
處置逐項記在本機 `temp/review_1119/TRIAGE.md`，各批的判定表在 `temp/review_1119/fix/<批>.md`，報告在 `reports/`。
- **已修（要配戴或實機確認的行為改變）**：
  - 自動半速（`xrrateauto.h`）：視窗切換時進行中的斷流沒算、切換後 4 秒穩定期的重複被算進判斷、試升失敗的退避沒加倍。**要在不穩的網路上看升降。**
  - PCVR 送出前重建 projection（relatch／補幀防護）時眼睛位置、注視點 quad 沒跟著重算（`xrcontext.cpp`）。穩態 `mismatch=0` 走不到。11-19 晚配戴：頭部平移時注視點與周邊沒有錯開（該場整場 `mismatch=0`，重建路徑仍沒走到）。
  - 控制串流（`ControlStream.c`）：VR API 閘門改到 `startControlStream` 成功才開；停止時在鎖內銷毀連線；恢復狀態機不再重送已被涵蓋的 LOSS；重連目標位址的切換條件。無人場次開場與穩態正常；**拔線、換網卡的情境沒重測**（11-18 那三台的測試要再跑一次）。
  - client 主機管理（`computermanager.cpp`／`nvcomputer.cpp`）：已配對主機只採用釘選憑證的 HTTPS；憑證不符只換下一個位址、不降級配對；serverinfo 的位址只收單播 IP 字面值；喚醒只對使用者自己填的位址做 DNS。**手動加入「uuid 相同但憑證換了」的主機會失敗，要先重新配對或刪掉再加。**
  - Linux／macOS 單一實例鎖改放 `$XDG_RUNTIME_DIR`，開不到鎖檔會直接結束（原本靜默跳過）；log 檔改用 `O_NOFOLLOW` 開（`main.cpp`）。**Linux x64 client 還沒實際啟動過這一版。**
  - 頭盔安裝腳本：`--no-steam` 會留到自動更新、SSID 以位元組計長、`--hidden` 要是獨立欄位、netmode 狀態檔跳脫控制字元、交還家用 Wi-Fi 後 20 秒內每圈查網卡。**新腳本還沒裝到頭盔上跑。**
  - Android：配對流程（連點、等 PIN 時的輪詢、主機被移除）、解碼器輸入長度檢查、更新下載的轉址。Pixel 5 上 2.0.237 可連線；之後的版本沒重測。
- **新發現、沒處理**：
  - 半速＋補幀的無人場次每 10 秒有 0～10 次 `mismatch`（補幀防護有處理到，但仍計入）；11-15 的 2.0.202 紀錄是 0。用今晚修改前的建置重跑也有，**不是今晚造成的**，是 2.0.203～2.0.234 之間的變化或無人場次的條件，待查。
  - `§CTRL-REGAIN` 只在強制綁網卡的重連後才會啟動（`ctrlLocalValid = forceBind`）。審查建議改成一律啟動，但路由沒指回原網卡時會每 15 秒重連一次，沒採用；要改得先讓移回去的那次重連強制綁原網卡。重連後也沒重設 `ctrlPrimaryGoodSinceMs`。
  - NVENC 改幀率的路徑（被動補幀調整才會走）、Steam Controller 停用重試（要實體手把）、§VR-EDGE-HOLD、跨磁碟區移動與外接儲存，今晚驗不到。
- **需設計（成立、沒動）**，細節在各批 `fix/*.md` 的「需設計的項目」：`vrApiReady` 與刪 mutex 之間的窗口（要改 `Platform.c` 的計數或加 in-flight 計數）；追蹤走 ENet 時每筆配置＋持鎖；控制通道沒有重放視窗；`sendC2S` 整段持鎖；xrvideo 繪製失敗仍 release；relay proxy 的 XML 沒有憑證驗證、`relaylookup.cpp` 不驗憑證又把 rikey 寫進 log；刪除主機時不等其他任務；Windows 更新程式的提權腳本放在 %TEMP%；`relayPsk` 明文存在設定裡；定位失敗時沿用上一筆頭位。
- **§STREAM-MENU（新功能，server）**：2D 串流期間，遠端點開始選單的電源圖示、或在桌面按 Alt+F4，不出現 Windows 的電源選單，改顯示 host 畫的「串流控制」選單（中斷串流、結束 App 並中斷、鎖定、睡眠、關機、重新啟動、取消）。設定 `stream_menu`（預設開）。另有每 300 ms 的保險，看到「關閉 Windows」對話框就取消它。
  **還沒測**：鎖定、睡眠、關機、重新啟動、保險那條（本機叫出對話框）、選單的鍵盤操作、英文介面。Ctrl+Alt+Del 畫面不處理（安全桌面）。web UI 設定頁還沒有這個選項。
- **頭盔電源鍵（另一件事，還沒做）**：頭盔上 `steamos-powerbuttond` 讀 `/dev/input/event0`，一般使用者可以 `EVIOCGRAB` 獨占（實測成功）、Flatpak 有 `devices=all`，所以串流期間把頭盔電源鍵長按換成串流選單是做得到的；短按要不要保留睡眠、怎麼把按鍵還給 Steam 還沒定。

### 2026-11-18 深夜：影片模式檔案管理、選取狀態、熱點加入、server 設定頁（client 2.0.234／server 2.0.233，已提交 v2.0.234）
- **§MEDIA-FILEOPS 檔案管理**（host 端 `/viple/media/op`、`opstatus`、`opcancel`，要 server 設定 `media_write`（預設關）才可寫）。
  **沒驗**：外接儲存裝置、跨磁碟區移動、Linux server。
- **§VIDEO-LASTPLAYED**：對使用者一律稱「播放項目的選取狀態」（11-18 被糾正過，不要用別的說法）。

### 2026-11-18 晚：§REVIEW-1111 的 19 項「需設計」實作（client 2.0.229／server 2.0.230，已提交 v2.0.234）
逐項結果在 `temp/grok_review_1111/FIXLOG.md` 最後一節（已修 16、不修 3）。還沒驗到、要補的：
- **實體 Steam Controller（CachyOS 接手把、裝新版 client）**：短按不被 keepalive 蓋掉、feature 握手成對、
  場次結束後 host log 有沒有 `disabled on retry N`（§SC-HID-GATE 停用被拒絕的背景重試；失敗時會印 CONFIGRET）。
- **影片模式播 host 分享的檔案**（憑證只給已配對電腦）：從媒體庫播一支 host 分享的檔案；瀏覽器把來源指到別的主機時
  log 要有 `not presenting the client certificate`。
- **頭盔無人 VR 場次＋第二條連線熱插拔**（`pendMutex`）。
- **桌面改幀率**（NVENC `reconfigure_framerate`＋擷取節拍）：要 client 補幀判定吃緊才會送，無人狀態沒觸發到；
  看 host log `[VIPLE-FPS] NVENC frame rate updated` 與 `Capture frame pacing updated`。

### 2026-11-18 下午：遠端 2D 糊清循環＋PyroWave 提示（host／client 2.0.226～227，已提交 v2.0.234）
§Q-STALE-DGLEN（經 VPN 的 2D 串流每 14 秒糊清一輪）已修並經使用者確認。剩下：
- PyroWave 碼率過低的提示（`[VIPLE-PYROWAVE] bitrate … is below …`）用的建議值 `getPyroWaveRecommendedBitrate()`
  （1080p60≈60 Mbps、4K60≈235 Mbps）**沒有用畫質量測驗過**。
- 經 WARP 時剩下的降碼率（16 分鐘 31 次 cut、stale 共 1681）都伴隨真的丟包（`lostB` 382 KB），大約每 30～40 秒在 17 Mbps
  附近發生一次——像是 WARP 路徑的上限，還沒細查。
- **§QUIT-HANG（client 2.0.227）**：退出串流後按「結束遊戲」卡在「結束 Desktop...」。串流結束時已經送過 `/cancel`，
  清單還顯示執行中的那幾秒按下去，`PendingQuitTask` 看到 `currentGameId==0` 就什麼都不做，而完成通知只在狀態有變時才發。
  改成工作自己通知完成（`[VIPLE-QUIT] nothing is running …`）。**修正後沒有實際重現驗過**（要在 UI 上搶那幾秒）。
- **還沒處理**：靜止桌面時 server 只送 36～46 fps（要求 60，`[VIPLE-ENC-DUP]` 重複幀補得不夠），開發機與 host 都有；
  client 把自己送出的 ACK 封包算成「send loss 30～60%」（`§Q-LOSS-RXCHECK` 有擋住換路，但浮層上的丟包率是假的）。

### 2026-11-18 VR 遊戲可用性（host 2.0.224／client 2.0.225，已提交 v2.0.234）
做法是開發機當模擬頭盔
（`--vr-emulate`），頭與手把由新的 dev 參數 `--vr-puppet <檔案>` 控制（格式在 `moonlight-qt/app/streaming/vr/vrsynthetic.h`），
從 client 視窗截圖判斷——**沒有人戴著玩過**，下面「通過」只代表啟動、畫面、追蹤與選單輸入都對。

**這一輪修掉的（§SC-HID-GATE、§VR-PADMODE 誤判、§VR-FOCUS 擴充，都在 host）裡還沒驗的與限制**
- **§SC-HID-GATE**（service 啟動就把虛擬 Steam Controller 停用，這一場的 client 送來實體手把的 feature 回應或 input report 才啟用，
  場次結束再停用）。**待驗（重要）**：用真的 Steam Controller（醒著、睡著各一次）串流一場，確認 Steam 仍然認得手把、按鍵與震動正常——
  開發機的接收器上沒有連著的手把，這條路徑完全沒有實測。
- **§VR-PADMODE**（商店沒有分類 52 時再看 `<Steam>\config\openxr\steam.app.<id>_<手把類型>_N_binding.json` 有沒有）：
  限制——遊戲第一次執行、檔案還沒產生的那一場仍會判成手把模式。

**19 款的結果**
| 遊戲 | 結果 |
|---|---|
| Assetto Corsa Competizione、GRIP | 通過：VR 主選單顯示、自動切 Xbox 手把模式（手把按鍵這次沒有逐一驗） |
| Moss、Dance Dash、Toy Trains | 部分：啟動、追蹤、按鍵都對，但開場的手勢（翻書／踩按鈕／拉燈繩）用模擬手把沒做出來，**要配戴確認** |
| Tilt Brush | 不能用，和串流無關：遊戲啟動時呼叫 `wmic.exe`，見下 |
| CarX Drift Racing Online | 要在遊戲裡手動開 VR 模式，見下 |
| Assetto Corsa | 未解：見下 |

**還沒解的**
- **host 的 SteamVR 主控台**：11-18 中午依使用者決定改回啟用（`steamvr.vrsettings` 拿掉 `dashboard.enableDashboard=false` 與
  `arcadeMode=true`，備份 `steamvr.vrsettings.bak-1118-dash-reenable`）。無人場次開場時它是開著的（疊在遊戲前面）；
  **配戴時開場會不會自己關、對延遲有沒有影響還沒有人看過**。
- **CarX**：`-steamxr`（`steam://launch/635260/VR`）只是讓它連上 SteamVR，VR 畫面要在遊戲裡「設定 → 繪圖 → VR 模式」打開才開始
  （打開後場景程式立刻出現、選單變成 VR 裡的一塊虛擬螢幕、自動切 Xbox 手把模式）。**這個設定不會保留**：正常結束再開還是平面，
  每次都要在 host 桌面（或 SteamVR 主控台的桌面檢視）手動開一次；要不要由 server 代為處理還沒決定。
  另外 CarX 收到關閉要求 10 秒內關不完，`[VIPLE-STEAM-QUIT]` 會強制結束它。
- **Assetto Corsa**（`steam://launch/244210/VR`）：Steam 把它的 2D 啟動器當成非 VR 程式，啟動前在 SteamVR 主控台跳一次性的
  「VR 劇院螢幕」說明等人按確定（11-18 用 Steam 的除錯介面代按了，之後不會再出現）；啟動器起來了，但頭盔裡的劇院螢幕整片綠色
  （Steam 的桌面串流沒有畫面），沒辦法操作啟動器。要查：綠色是不是和我們的擷取或編碼衝突；主控台改回啟用之後有沒有重試過待核對。
  AC 要先過 2D 啟動器、進賽道（acs.exe）才是 VR 場景程式，手把模式在那之後才會開。
- **Tilt Brush／WMIC**：用 SYSTEM 身分的排程工作跑 DISM 裝好了（`WMIC~~~~` Installed；SSH 下 `Add-WindowsCapability` 回 Access is denied），
  但 `System32\wbem\WMIC.exe` 還沒出現（有待處理的檔案搬移），Tilt Brush 仍然失敗；host 下次重新啟動後再驗。
- 這一輪的 dev 工具：`temp/vrgames/vg.sh`（start／set／click／shot／stop）、`sweep.py`、`clicksweep.py`；每款的截圖與紀錄在
  `temp/vrgames/out/`、`temp/vrgames/results.md`（本機，不入 git）。

### VR 手把相容性（§VR-PADMODE、§VR-SKELETON，2.0.68）
- **§VR-PADMODE**：不使用 VR 手把的遊戲（商店沒有 Tracked Controller Support）自動插虛擬 Xbox 360 手把；ACC、GRIP 用模擬頭盔
  端到端驗過（XInput 對照、手動長按切換、結束移除）。**待驗**：配戴實玩 ACC／GRIP 的操控手感；頭盔 dev 版的切換提示圖層（還沒人看過）。
- **§VR-SKELETON**：driver 補手部骨架（由扳機、握把、拇指觸碰推彎曲程度）；ACC 的 `invalid device path for skeleton output` 已消失。
  **待驗**：配戴時遊戲裡的手指動作（Alyx、Moss 這類會顯示手的遊戲）。
- **§VR-WHEEL（2.0.72，v2.0.75 已提交）**：手把模式時兩支手把像握方向盤一樣轉＝左搖桿 X；設定頁開關＋轉向角度（180／270／360／540／900°），
  串流中兩支搖桿同時按住 1.5 秒切換（結果寫回設定）。開關與角度放在 0x5506 左手輸入的保留位元組，舊版兩端忽略。host 單元測試 14 項過；
  **待驗**：配戴 ACC／GRIP 試手感、快捷鍵提示、轉超過 90 度與轉身時的穩定度。

### §AUTO-UPDATE 各平台自動更新（2.0.73，v2.0.75 已提交；預設自動安裝）
- **待驗**：2.0.122 是第一個帶自動更新的正式版，要等它之後的下一次發佈才驗得到實際的下載＋驗證＋安裝（四個平台各一次）；Android 第一次的「安裝未知應用程式」授權流程；
  頭盔的 `auto-update.sh` 還沒在頭盔上跑過。

### §LAT 延遲與體驗改善路線（2026-11-05 定案，依序做、做完一項更新一項）
現況（10-07 頭盔，未逐段量）：延遲 41～45 ms ≈ 主機 SteamVR 管線約 2 格 14～16＋擷取編碼 3＋網路 2＋解碼 3＋延後挑影像 3～4＋頭盔合成到顯示約 1.3 格 9～11。
最大兩塊是兩端 SteamVR 的管線；換語言（C、組合語言）不會改善（等待時間不是 CPU 指令，編解碼在硬體）。
3b. **ABR 塞車反應太慢（11-05 從 LAT10 發現）**：模擬頭盔 75 Mbps 時送 84.8／收 58.6 Mbps（收到的少三成＝在排隊），單向延遲 p95 83 ms、max 172 ms，ABR 要等 owd 趨勢累積到 overuse（約 10 s）才降。改法：收／送速率比連續低於門檻就直接降（不等趨勢），並檢查開場直接用上限 75 Mbps 起跳是否合理。**狀態：已加規則、待頭盔驗（host 2.0.81）**——`vr_abr_fast.h` 的 on_rates：送 ≥ 收 × 1.15 連續 2 次（約 1 s）就降（rate-shortfall+delay，降幅照收／送比），單元測試 27 項過（新增兩項）。模擬頭盔 90 s 這條規則沒觸發：塞車時是 owd-trend 先降；而且收送速率只在處理收包報告時更新，**整段卡住時報告也送不過來，速率停在舊值**——下一步補「超過 N ms 收不到收包報告＝塞住」直接降。開發機這條 Wi-Fi 本身會整段卡 1.7 s，不能拿來判斷效果。
4. **A2 主機畫面節拍對齊頭盔顯示＋A4 依實測動態調整延後挑影像**。**狀態：調查完（11-05），全部要頭盔驗**——`--vr-emulate` 不跑 OpenXR 迴圈、不送 LATCH，模擬頭盔量不到。候選（依序）：① 延後挑影像依顯示週期比例縮放（144 Hz 一格 6.94 ms，固定延後佔比太大）；② 依實測 GPU 完成時間（tail max、heldMiss）在 3～6 ms 間動態調。死路（不要重做）：host 目標餘裕 40%（撞頭盔合成器 GPU 忙碌段，重複幀 3.39%）、依晚到比例自動調餘裕（底噪讓積分器一路推到上限）、延後挑影像 > 6.5 ms（`kLatchSpanMaxUs`，7 ms 時 runtime 重投影 71 格）。調查報告：`temp/lat_a2/survey_out.md`。
5. **A5 送深度圖層讓頭盔做位置修正＋D1 依實測延遲與頭部動作調整預測量**。**狀態：未開始**
6. **A1 分片編碼與傳輸、A3 研究讓 SteamVR 少排一格**。**狀態：未開始**
其他候選（排在後面、依量測決定）：B3 AV1（先確認 Frame 的 AV1 解碼延遲）、B4 影像封包 Wi-Fi 優先權、B5 批次收送（USO／RIO、sendmmsg）、
E1 頭盔效能等級（注意電池保護斷電）、E2 熱路徑即時優先權、E3 熱路徑去鎖去配置。

### §LAT-2D 2D 串流延遲與體驗（Windows／Linux／Android 用戶端，2026-11-05 使用者：不是只有頭盔）
VR 的 [VIPLE-VR-LAT10] 與 §ABR-SHORTFALL 只在 VR session 生效；2D 走另一套 ABR（AIMD）與另一條呈現路徑。依序：
2. **2D ABR 塞車反應**：限速工具（`scripts/net/host_throttle.sh`、`scripts/net/ab_2d_throttle.sh`）與三項修正（延遲趨勢與絕對門檻任一成立、`abr_cut_idr` 預設 disabled、延遲回到基線時加快回升）已做（host 2.0.85）；Android 對照 11-12 跑過（`temp/lat_2d/ab_and_1112/`）。**待看**：host 處理時間有幾個 10 s 從 3.9 跳 7.9 ms（與這些修改無關）；Linux client 與 Tailscale 遠端路徑也要跑同一組。
3. **呈現路徑排隊**：Windows（D3D11 flip model、waitable swapchain、V-Sync 關時的節拍）、Linux（Vulkan／EGL present mode）、Android（MediaCodec 低延遲旗標 KEY_LOW_LATENCY 與廠商選項、Surface 直出、Choreographer 節拍）。**狀態：部分查明（11-05）**——Windows 已是 flip model＋最大排隊 1 格＋waitable＋允許撕裂，用戶端解碼＋排隊＋算圖合計約 2.4 ms；Android 預設「最低延遲」直接 release、排隊 1.1 ms。剩下：Exynos 的低延遲鍵只套 H.264（要擴到 HEVC／AV1，沒有 Exynos 手機可驗）、Choreographer 執行緒優先權（只影響「平衡」模式）。設定頁應註明補幀會多約 13 ms 延遲。
4. **解碼端**：各平台硬體解碼的輸出延遲（Android 解碼器常多緩衝 1～2 張）、零複製。**狀態：未開始**
5. **輸入延遲**：相對滑鼠完成（2.0.89；client `--input-test`、host `[VIPLE-INPUT10]`、累計位移封包 `SS_MOUSE_MOVE_REL_CUM_MAGIC 0x5500000A`，host 宣告 `<VipleStreamInput>1` 才用）。剩下：按鍵／鍵盤／手把按鈕仍是可靠傳送（漏一個按下或放開會錯，要另外設計）；Android client 用同一份 common-c，重建就會用新封包（還沒在手機上驗）。

### 2026-11-15～16 回到家：區網位址與熱點自動啟閉（2.0.205～2.0.219，已提交）

- **頭盔熱點＋host 無線網卡的區網雙路徑（v2.0.209，11-17 配戴五場確認；§FRAME-HOTSPOT-AUTO／EARLY、§VR-HOTSPOT-YIELD、§VR-HOTSPOT-PROFILE）**：
  host 的熱點設定檔名稱要＝SSID（手動）；`VipleStream-Frame` 這個名稱會被 host 的 Steam 當成錯的設定檔而切斷，甚至整個停止連線。
  配戴的其中一場家用 Wi-Fi 那條斷了約 70 秒，原因沒查到（頭盔、host、路由器都沒有紀錄）。查法：host 的 Steam 紀錄
  `logs/remote_connections.txt`、無線事件紀錄 `Microsoft-Windows-WLAN-AutoConfig/Operational`（約 12 分鐘就被洗掉）。
  **沒實測**：Steam 沒在連時 server 自己連上的那條（只用 netsh 手動驗過 Steam 會接受）；換另一台 host 的路徑；熱點本來就開著但
  沒有裝置連上的區網場次仍然不動。不要用關 host 的 `WifiAPEnabled` 當解法（Steam 反而會切斷我們的連線）。
- **§FLAT-ML 補洞（2.0.219）**：要求多連線的判斷原本沒有排除「經 VPN」。頭盔有熱點憑證，經由剛好用私有網段的 VPN
  （自架 WireGuard 的 10.x）連回來時會誤要求多連線，那一場只有一條連線又不走 QUIC。現在路徑是 VPN 就不要求
  （log：`flat (2D) session: the host is reached through a VPN - multi-link not requested`）。**沒有實機驗**（手邊沒有這種 VPN）。
- **§FLAT-ML 2D 區網多路徑（host／client 2.0.218，11-18 無人驗過三台）**：平面場次在區網、雙方新版、這台有第二條路時不走 QUIC，
  改用和 VR 相同的多連線 hub。
  **限制／沒驗**：只有 Windows host 宣告（Linux host 的 hub 沒跑過）；新 client 對舊 host 只用「host 關掉 vr_multilink」代替驗過；
  控制連線換路的約 1.2 秒內輸入會斷；CachyOS 那台螢幕關著時解碼佇列會滿（每 7 秒要一次 IDR，
  單一路徑也一樣，和多連線無關）；沒有配戴實際玩。

### 2026-11-13～15 外地經 VPN 這一輪（2.0.176～2.0.204）待驗與待做

**待配戴確認**
- **球的軌跡、拋球、手把與畫面同步（§VR-CTRL-PREDICT 2.0.201、§VR-EDGE-HOLD `vr_edge_hold_ms` 預設 12）**。
  無人量測用 host 端 `vr_probe --mode swing|toss`（真頭盔＋合成揮拍與拋球）。
  11-15 配戴（host 2.0.202）：「球拍判斷更跟手」。**還沒回報的**：拋球、揮拍反彈的手感；拿球的手按著扳機時會比畫面晚一段，
  放開後約 0.3 秒回到同步。
  **還沒驗的**：4 只有單元測試與一場線路極差的無人場次；`--vr-synthetic-uplink-gap <週期>:<空窗>` 可以固定重現
  「放開訊號晚到」（`temp/linetest/probe_run.sh toss …`）。真的控制器（速度有雜訊）下加速度修正的表現只能靠配戴判斷——
  覺得拍子抖就設 `vr_ctrl_extrap_ms = unlimited`（SteamVR 自己外插）或數值（固定上限）。放開訊號晚超過約 75 ms 的救不回來。
- **§VR-REPAIR-WAN 外地補包（2.0.202；`vr_multilink_repair_far`，連線功能位元 0x20，只在原本規則不補的路徑上生效）**。
  11-15 配戴（補包開著的 34 個半速視窗）：丟幀→刷新波 1.1 次／10 秒、MTP p95 110 ms，「注視點範圍內無可挑剔」。
  **還沒做的配套**（審查報告
  `temp/perf_review_1113/repair_wan_*.md`）：補包放行那一批的排隊時間被 host 當成解碼時間（全速時可能擋碼率回升）、
  補包吃掉 LOSS 之後 FEC 少了往上調的訊號、連續幾幀都缺時逐幀各等一輪、整場都開著補包（不是 30 秒一段）時的長測。
- 攤開送（§VR-PACE-AUTO）多出來的延遲值不值得：同場對照 72 fps 多 7～9 ms、144 fps 多 3.6 ms，換到丟包事件少四到六成。
- `coast`：上行有空檔時頭與拍子不往回跳（只在上行超過 25 ms 沒有新樣本時才有差別）。
- 注視點分塊的邊緣（A25、A39 改過那一趟 shader，數學上每通道最多差 1／255）。

**待做**
- **§FRAME-HOTSPOT-PAUSE 待配戴確認**（已實作）：經 VPN 的場次，頭盔的串流轉接器熱點開著而上面沒有任何裝置時，串流開始自動暫停、
  結束還原（和 Steam 設定頁的開關同一個控制通道；使用者自己關掉的不碰）。熱點開著時每約 30 秒搶走無線電 3～16 秒；11-14 手動關掉的
  對照：週期消失、整場全速、碼率目標 2～4 → 9 Mbps。要確認：在外面開著熱點直接串流的體感、回家後 Steam 內建串流照常連得上熱點、
  串流行程被強制結束之後熱點有被補回來。
- **§M.2-SELF 接管提示寫的是自己的名字（已修，2.0.192）**：待使用者在筆電上確認——用熱鍵離開串流（session 留在 host）再進來，
  不應再跳「<自己> 正在使用這台電腦，接管會使其斷線」（`/serverinfo` 在憑證對得上擁有者時回 `SessionOwnedByCaller=1`）。
- **弱訊號鏈路停擺時的送出量**：全速 144 fps 時碼率目標降到 2 Mbps 下限，線上仍約 5.5 Mbps（每張至少兩個補滿的資料封包＋FEC）。
  鏈路只剩幾 Mbps 時這個底本身就塞得住。11-14 晚上同一場的對照（目標都在下限附近）：144 fps 線上 6～20 Mbps、整幀收不到 2.1～6.1%；
  降到半速之後線上 3.5～3.7 Mbps、0.3～3.9%（平均 1.6%）——約每 1～3 秒一次，就是使用者說的「定一下、伴隨破圖」。
  可以試的方向：小張的影像用比較小的封包（線上的量跟著目標走）、外地也開補包（§VR-REPAIR-WAN，連同配套）、半速時刷新波照時間而不是照張數。
- **VR 碼率控制重新設計**：11-13 加的四組規則（斷訊擋降、降碼無效就撤回、路徑升級加速回升、收送落差）鎖在 `vr_abr_experimental`
  （預設關，關著時和 2.0.175 相同），三次配戴都變差。丟包和碼率無關的線路上碼率仍被一路壓到下限，回升每秒只有幾十 kbps。
  已知的具體缺陷（同一秒第二刀把碼率往上拉、擋降只擋降不擋升等）列在本機 `temp/perf_review_1113/abr_recheck_*.md`，
  單獨修任何一項都會讓現況的碼率更低，要整體重做。
- **開場碼率**：經 VPN 時也從 75 Mbps 起步，弱線路頭 10 秒丟包 13～41%、20 秒內被砍到 4 Mbps 上下。要有依線路的起點。
- 攤開送自動切換的那一刻，幀到齊的時刻移大半個幀週期，頭盔的顯示相位要十幾秒才追上：切換時給 latch 一個前饋。
- 頭盔的錯誤回報入口：client 在頭盔上當掉時只有 systemd 的 core，沒有任何提示（11-14 的 §XR-FEATCHAIN 是靠無人場次才發現）。

### §PERF-R2 第二輪效率審查（2026-11-13～14；A 節 49 項已改並建置，逐項驗證大多還沒做）

審查彙整在本機 `temp/perf_review_1113/r2_00_summary.md`（A 節＝不需實機就能改的 49 項、B 節＝要實機量測或配戴才能決定的 32 項）；
修正總表在同目錄的 `r2fix_00_summary.md`（每一項的狀態、動到的檔、預設行為有沒有變、驗證方式、殘留風險），各組的完整報告是 `r2fix_*.md`。

**建置狀態（2026-11-14）**
- macOS 的 server 沒有建置機，改到的共用檔先靠 Linux 那一次把關。
- `scripts/check/xr_guard_scan.py` 回報 5 筆沒有守衛的 XR 型別（`moonlight-qt/app/main.cpp` 的 `XrContext::setHandBackOnShutdown` 一筆、
  `streaming/player/playersession.cpp` 四筆）。HEAD 就有、不是這一輪改出來的；5 筆都在 `HAVE_VR_PLAYER` 之內（`app.pro` 只在有 OpenXR 的
  Linux 建置才定義），Windows client 實際建置通過，是掃描工具不認得這個巨集。

**待驗（細節與預期的 log 在總表每一列）**
- **上頭盔之前先在開發機做**：A25（注視點分塊邊緣漸層的色彩換算）與 A39（分塊取樣座標改由頂點屬性內插）改的是 PCVR 預設每格都跑的那一趟。模擬頭盔、注視點「小」，`--xr-dump-frame` 改前改後比整張：亮度相同、每通道差 ≤1／255、上排逐位元相同；client log 要有 `tiles: sampling positions come from vertex attributes`。萬一畫面上下顛倒或兩排對調，把 `xrvideo.h` 的 `m_TilesVaFailed` 初值改成 true 就整個關掉 A39。
- **開發機＋host，開場與閒置**：A4（nvprefs 改背景，開場約 −0.4 秒）、A8（開場第一個 IDR 請求不再被冷卻壓掉）、A18（VR 編碼器探測快取，要同形狀連跑 3 場才看得到 hit）、A30（PRECHECK 沿用預先探測）、A9／A41（閒置時編排器與 bridge 的喚醒次數）、A7（開場降碼真的進編碼器，看 10 秒行的 `enc=`）、A19（RTSP 每場 5 個請求）、A5＋A44（`[VIPLE-STARTUP]` 各行、`decoder props src=probe`）。
- **開發機＋host，log 量**：A1（SC-HID）、A2（網卡列舉）、A12（/serverinfo 與輪詢的每請求行）、A13（送出失敗洪水）、A14（log 輪替：`.1`／`.0`、icacls、服務模式的 svc log）、A15（ABR 每步一行）、A28（client log：`rikey=REDACTED`、上限只出一行）、A32（逐筆丟失行限流）。
- **開發機＋host，行為**：A3（2D FEC 回降，clumsy 打丟包）、A27（結束自訂 app 的等待）、A29（疑難排解頁只抓 4 MB、Copy Logs；Safari 也要點一次）、A42（UPnP 退避）、A26（主機輪詢每 3 秒只剩 47984）、A10（封面與右鍵選單的七個開啟點，平面視窗與 `--vr-shell noxr` 各一次）、A21＋A31（`--vr-shell noxr` 啟動與設定頁、ViGEm 虛擬手把導覽）。
- **頭盔無人場次**：A6（過場頁不跑轉場之後 XR 交接正常）、A33（`[VIPLE-VR-VIEWS]`、`[VIPLE-VR-RATE] request`、`[VIPLE-VR-BUFFER] 10s` 三種行都還在、沒有 `[VIPLE-VR-LOGASYNC] dropped`、`latch2end` 沒變差）、A31（子行程的 `Loaded N` 行數）、A45（`instance attempts` 裡 1.1 被拒那次的毫秒數；超過約 20 ms 才值得記住可用版本）、A46（兩條連線、沒有故障時 `dup10s` 的 `uniq`／`solo` 接近 0）、A21（VR 模式與桌面模式各開一次設定頁）、A11（影片模式的 EAC 片）、A36／A38（非預設路徑：`vr_fovea_tiles=disabled`、半速＋補幀）。
- **遠端（Tailscale）**：A22（影像封包 1184：同碼率下包數少約一成、沒有新增掉包；先確認 host 端的 Tailscale MTU 是預設值）、A34（開場就在中繼時兩端第一輪就 restun＋ping）、A35（DERP 中繼且單連線時姿態上行降到 1 倍，`relay thinning on／off`）、A40（client 用 fd7a: 位址連線時不 boost MTU；「fd7a: 上大包會黑洞」這個前提還沒有實機證實）、A47（FEC 升高期間的 `VR episode` 行）、A48（單連線的 `[VIPLE-VR-FECGAP]`）。
- **Linux 桌面（CachyOS、Ubuntu）**：A16（zh_TW 介面中文正常、log 是 `CJK font found on the system`）、A24（AppImage＋`QT_QPA_PLATFORM=offscreen`＋軟解，啟動警告不再多等 3.5 秒）、A14 的鎖檔（同時跑兩個 server 行程時第二個不輪替）。
- **Android 手機**：A17（冷啟動沒有 `VIPLE-VK-PROBE`、開 Vulkan 補幀時仍有）、A20（靜置 60 秒每台主機只有一次 `[VIPLE-PCLIST] shortcut confirmed`、沒有 `saved`；`dumpsys shortcut` 三種情況）、A37（GLES 與 Vulkan 補幀三個品質等級都編得過）、A22 的 Android 那一半（要手機＋Tailscale）。

**做了一半、刻意延後或要人決定的**
- A15 的另一半：`Sunshine/src/video.cpp` 的 AVCodec／PyroWave 路徑沒有回報實際碼率，這兩種編碼器的 VR 場次 10 秒行顯示 `enc=-`（host 的 VR 都是 NVENC，實際場次不受影響）。
- A21 的另一半：設定頁 VR 區包進 `Loader { active: VrLauncher.available }` 沒做（會動到頭盔上那一頁的焦點邏輯，要能實際操作才做），建議和「2D 專用區塊包進 Loader」同一輪。
- A10 延後的三件：佔位圖判斷搬到 C++ 之後再設 `sourceSize`、封面快取改存原始位元組、XrShell 自己的 incubation controller。
- 能建置、能跑一場之後再考慮的選做：A8「前 8 幀剛回應過請求就不送 §K.14 重送」、A18「VR 擷取用啟動時存的 adapter LUID」（服務重開後第 1、2 場也能命中快取）、A9 改用 `NtQuerySystemInformation`、A13 的 macOS、A14 的時間戳與 formatter 快取、A45「行程內記住可用的 OpenXR API 版本」。
- A22＋A40：MP-QUIC 場次經 Tailscale 仍用 1024 B 影像封包；A40 只處理了 IPv6 路徑，放寬到 1168 之前要先確認 IPv4 Tailscale 路徑的實際行為。
- A32：`RtpvLossLogDropped()` 的原型現在在兩個 `.c` 各寫一份，要搬進 `Limelight-internal.h`；`tests/vr_repair_sim/sim.c` 可能缺 `VrFlags` 與 mutex 的替身而連結不過（v2.0.59 起的既有狀況，沒有建過、不確定）。
- 本機腳本要跟著改：抓 host log 的腳本（`scripts/vr/watch_host_sessions.sh`）連 `sunshine.log.1`、`.0` 一起抓；`scripts/diag/viple_display_probe.ps1` 的 `-LogTail` 加大；`scripts/benchmark/results/round-m0-game-20260923/tools/analyze_round.py` 靠行數算丟失與送出失敗的地方要改讀彙總行。
- 要人決定：VR 介面啟動時的「未對應手把」對話框要不要留（現在保留）；UPnP「找到過閘道之後就不退避」這個多加的保守條件要不要留；疑難排解頁的省略提示要不要寫成完整句子（要加翻譯 key）；舊 client 才會走的 `IDX_LOSS_STATS` 要不要併進 2D FEC 的共用規則。

**B 節：要實機量測或配戴才能決定的（編號與做法見 `temp/perf_review_1113/r2_00_summary.md` 的 B 節；前置的量測都已在 A 節上線）**
- B26　自動半速把掉幀當成晚到：RTT ≥22 ms 的單連線場次都降到半速、升不回來。要在同一條遠端線路 A/B。
- B27　遠端單連線補包的 RTT 門檻（把約 160 ms 破圖換成約 40～50 ms 凍結）。前置 B26。
- B25　refresh wave 長度改依時間算（半速時每波少約 55 ms 破圖）。前置 B26。
- B24　中繼（DERP）開場的碼率上限，少掉開場最糟的 8～12 秒。和 B21 連動。
- B10　VR FEC 升到 40% 後依實效提早回降。前置量測 A47（`VR episode`、`elevatedUse`／`nearMiss`）。
- B28　parity 延後送（§VR-FEC-SPREAD）。前置量測 A48（`[VIPLE-VR-FECGAP]`）；只有缺口延伸進 parity 區佔多數才做。
- B1　注視點 QP 差分圖在位元流裡看不到效果、ABR 看到的 QP 卻照扣（以為 14、實際約 27）。兩種修法都會大幅改變碼率與畫質，要使用者決定並配戴。影響 B3、B4。
- B4　quality-trim 被單一樣本雜訊重設（平台期停在 QP≈14 收不下來）。前置 B1。
- B3　VR 編碼加 QP 下限 12（只省開場與載入）。前置 B1。
- B5　週期性 intra refresh 安全網 2 秒對 5 秒 A/B（不改程式，先做 B4）。
- B2　XR bring-up 每次約 2.1 秒來自 Steam app 身分（拿掉三個環境變數後 0.19 秒）；要配戴確認 SteamVR 的 per-app 設定、儀表板、輸入綁定沒變。前置量測 A45。
- B13　XR bring-up 與解碼器探測、音訊測試並行（/launch 早約 0.3～0.45 秒）。前置量測 A44。
- B15　無頭 XR 在 drSetup 就建解碼器（影像是關鍵路徑時每場 −0.27～0.4 秒）。前置量測 A44；效益和 B14 不可相加。
- B14　LiStartConnection 之後的串行雜項（Wi-Fi 掃描擋板、麥克風、播放裝置；約 0.1～0.2 秒）。前置量測 A44。
- B16　探測到真解碼器之間的負面快取（頭盔省 10～40 ms）。
- B19　2D 靜止畫面不再以完整幀率重編（靜止時 NVENC 約 49 次／秒降到約 10 次）。要驗 client 補幀與畫質收斂；粗略 A/B 可先把 host 的 `minimum_fps_target` 設 20（也會影響 VR）。文件已改成實際行為。
- B20　2D 第一張重複幀的期限。前置量測 A49（`[VIPLE-ENC-DUP]` 的 `lateBehindDup` 每秒超過 1 次才考慮）。
- B6　2D QUIC 影像固定 4+2 FEC 改成 0／2 兩級（乾淨區網線上量可省約 1/3）。
- B7　VR 音訊 512 kbps 降到 256 kbps（兩條連線合計少約 0.77 Mbps；音質要配戴聽過才定預設）。
- B8　每幀最後一個 data shard 不補零（144 fps 時每條連線約 0.79 Mbps；只在 VR＋多連線）。
- B9　兩條連線的 parity 各減半（省約 3.2 Mbps，但單條空檔時保護也減半）。前置量測 A46（`dup10s`）。
- B12　driver 提供 hidden area mesh（預設分塊下頻寬只省 0.5～1.2%，主要是 host GPU 少著色 15～20% 像素）。
- B17　影片模式全景投影沒有新影像就沿用上一張（30 fps 片源 GPU 約省 3～11%）。
- B21　Tailscale 狀態輪詢在直連穩定時放寬到 15 秒（不要 30 秒，會漏掉短的中繼段）。
- B23　DDA 畫游標時每張新畫面整張複製兩次，改成只保存與還原游標底下那一塊（1440p120 約 1% GPU）。
- B31　沒有 client 的 Steam Controller 時，Steam 對虛擬裝置每秒數十到兩百多次的查詢（A1 只壓了 log，省不到這段；要實體 SC／Puck 驗）。
- B32　server 行程重啟後自己開的 SteamVR 留著不關、也擋住 Steam Link（由程式碼推得，要先在 host 重現再立項）。
- B18　（低優先）解碼輸出改 UBWC 零複製匯入：約 20～80 mW，照原審查的做法做不出正確畫面。
- B29　（低優先）relay 的 WS 隧道整批送：只影響經 relay 的 2D 場次，線上約省 2.3～2.6%。

### 2026-11-11 這一批（2.0.126～134）待驗與待做
- **§VR-ABR-TRIM-PROP**（品質過剩時調降幅度隨 QP 落差加大、畫質不足時補回調降前的碼率）：頭盔無人實測開場 84 → 23 Mbps 約 30 秒、QP 穩在 15～18、每條鏈路 28.6 Mbps。**待驗**：載入畫面把碼率收過頭之後的回補只有單元測試驗過；配戴看畫質。
- **§CAP-PHASE**（`capture_phase_lock`）與 `steam_switch` 已加進 Web UI（Advanced／Network 頁，英文與繁中；其他語系顯示英文）：只建置驗過，沒有開 Web UI 目視。
- **`vr_gpu_priority` 預設改 realtime**（使用者 11-11 定案）：桌球一場 host 端 3.95 → 3.15 ms、MTP 44.5 → 43.1 ms。**待驗**：GPU 吃滿的遊戲、配戴。
- **半速＋補幀支援分塊**（2.0.129）、**§DEC-STUCK-RESET／FFmpeg v4l2m2m LAST 緩衝區補丁**（2.0.126～129）：無人驗過，待配戴。

### §REVIEW-1111 grok 審查（2026-11-11；索引在本機 `temp/grok_review_1111/INDEX.md`）
逐項紀錄（已修／不修附原因）在本機 `temp/grok_review_1111/FIXLOG.md`。下面只留還沒驗到、還沒做、要人決定的。

**修了但沒有實際觸發過的**
- 2.0.135（bugs-G003#3／#4、G005#1、G007#1／#2）：只跑過 2D「連線→斷線→2 秒內重連→斷線→結束」三輪與頭盔一場，各項沒有逐一做觸發測試。
- 2.0.136 配對工作階段（sec-G003#3：加鎖、最多 8 筆、180 秒過期、同 uniqueid 以新的為準）：沒有實際配對驗過（要有人輸入 PIN）。
- 2.0.137：D3D11 `m_RenderLock`（bugs-G024#1）的視窗改大小、PCVR 直連 `/launch` 失敗不再改走 relay（bugs-G023#1）的 relay 路徑，都沒有實際觸發。
- 2.0.138：更新等 UAC 逾時（bugs-G008#2）、系統匣選單（bugs-G010#1）要有人在 host 桌面操作才驗得到。
- 2.0.139：NCNN 補幀晚到（perf-G024#1）在開發機沒跑到（NCNN 被自動判定不可用）；
  Android 的備份排除金鑰、憑證與主機資料庫（bugs-G015#1／sec-G015#2，換手機要重新配對）與解碼器重建失敗路徑（bugs-G017#1）只確認 APK 建得出來，沒有實機驗。
- 2.0.140：異常輸入（短 rikey、壞掉的狀態檔、未閉合的 `$(`）沒有逐一餵過；bugs-G009#3（ENet 斷線、多連線還活著時呼叫 `vr_drain_s2c_ctrl_down`）要頭盔。
- 2.0.141：影片模式的載入遮罩（perf-G019#1，轉圈最多 20 秒）與 helper 自己交還家用 Wi-Fi 時通知 host（bugs-G026#2）沒有實際觸發（要播影片、要等 helper 交還）。
- 2.0.142～149：ANNOUNCE 範圍檢查等各項的異常輸入沒有逐一餵過，個別項目沒有逐一觸發。
- 2.0.150 第五批（perf-G019#2「啟動中」遮罩不轉圈、bugs-G026#3「只用轉接器」的存檔過期、bugs-G018#4 Linux 更新腳本等 90 秒、bugs-G007#8、perf-G002#3）：沒有個別觸發過。
- **§SERVERINFO-MIN／§STEAM-SWITCH-ACL（server 2.0.160；`steam_switch = paired｜admin｜disabled`，預設 paired）沒驗**：全新裝置實際走一次 PIN 配對（只對過兩個 client 的必要欄位都在）、Admin 裝置放行、409 那條、client 介面上 403 訊息的呈現。
- **§UPDATE-DIGEST（2.0.161～162）沒驗**：client 端下載＋核對的 C++ 路徑（要有比目前新的 release 才觸發得了，**下一次 release 後第一件事是在舊版上按一次更新**）、Linux AppImage 的更新、Android 的更新下載。
- 頭盔 2D 串流狀態條的字「Waiting for video from the host」（§Q-SOCK-SNDBUF 那一輪加的）沒有目視。
- Android 更新摘要（`UpdateNotes.java`）的對話框要等下一次 release 才看得到。

**還沒做**
- **高**（依報告編號；11-18 的重評有沒有處置到，待核對）：perf-G025#2（v4l2m2m 參數集一變就拆解碼器）；sec-G003#2（relay 進來的 `/launch` 沒有配對身分——只做了緩解：沒有配對身分的 `/launch` 不能搶走正在串流的 session，沒人在串流時仍可啟動；根治要讓 relay 帶上配對身分，是協定改動）；Android：bugs-G016#3（`/cancel` 時序與 relay）、perf-G016#1、perf-G017#1～#2、sec-G015#1（ShortcutTrampoline 匯出）。
- 做了一半的：bugs-G018#1 的 `session.cpp` 3928 附近的讀寫還沒改；bugs-G013#2 的「`start()` 撞上 STOPPING 回 503」；sec-G012#1 的「偽造 PING 影響統計」；bugs-G026#4 的應有幀數仍寫死 144；bugs-G007#8 沒有加 Flush；perf-G024#1 正常路徑的同步讀回（共用貼圖那條路在 NVIDIA 上會 device lost）；perf-G011#1 的「同一則只算一次」（會讓無線停頓那一批判不出來，`OwdWifiStallIsNotCongestion` 擋下）。
- 看過、還沒做的（中；11-18 的重評有沒有處置到，待核對）：sec-G014#2（VR 預約不是原子操作）、perf-G002#4（模擬 Home 鍵睡 100 ms；功能預設關）。
- host 常駐圖示的更新摘要還沒做。
- VR 鍵盤記住上次拖到的位置；2D 手把模式下開鍵盤（左手選單鍵是 Back，現在不能開）。

**待配戴**
- 核心繪製路徑的體感（2.0.163 用模擬輸入處置過：`xrWaitSwapchainImage` 逾時改成再等、注視點／分塊參數加一致性序號等；無人場次可用 `hs_run.sh` 的 `HS_TARGET`，預設 pcvr，可給 xr-desktop）；影片模式進片與離開時的按鍵（G029-02#2、#5）。
- **§XR-PADMODE（2D 串流的 Xbox 手把模式與方向盤，client 2.0.171）**：兩支選單鍵同時按住 2 秒＝切換手把模式、兩支搖桿同時按下 1.5 秒＝切換方向盤；對應同 host 的 `vr_padmode_map.h`，方向盤算法在 `xrwheel.h`（與 host 那份同一套，改一邊要改另一邊）；只支援 Frame 控制器（Touch／Index 只有一顆選單鍵，切不進去）。頭盔上 SteamVR 接受整組綁定（29 個）。**沒驗到**：實際按鍵切換、按鍵對應、方向盤手感、host 上遊戲有沒有吃到手把。
- **§VR-KBD（VR 串流的虛擬鍵盤，同一手選單鍵＋握把按住 1 秒開關）**：11-13 配戴可用（在 ACC 輸入姓名成功）；§VR-KBD-DRAG（鍵盤下緣白條，指著按住扳機拖動）有沒有配戴回報，待核對。
- **§XR-HUD-ORDER**（2D 串流的手把模式提示與低電量圖示改在螢幕 quad 之後送出）：只確認編得過，沒有配戴看過。

**其他**
- **ACC**：兩場遊戲只出約 57 fps（合成器統計 2051 張對 2948 張掉幀），144 Hz 下是規律的頓挫，原因沒查。ACC 的手把要在遊戲「選項 → 控制 → DRIVING CONTROL」切到「手柄」（`Assetto Corsa Competizione/Config/controls.json` 的 `carControllerMode`＝1；host 上已改，備份 `controls.json.bak-1113`），VR 場次沒有重驗（ACC 的 VR 選單不出現在 host 鏡像畫面，無人操作不了）；ACC 手把模式有轉向輔助、速度靈敏度、轉向伽馬 3.0 等濾波，用我們的方向盤模式時手感會不線性。
- **NCNN（RIFE）後端輸出全黑**：還在，但有防護——啟動時的自我檢測偵測到輸出全零就退回一般補幀（`probe REJECTED — RIFE output all-zero`）。根因（ncnn 與驅動）沒查。
- **觀察到一次、沒重現**：2.0.135 部署後第一輪回歸，host 服務在 17:31:37 當掉一次（0xc0000005，位置是送封包用的 `thread_local std::vector<WSABUF>` 解構，也就是某條送封包的執行緒結束時；一秒後系統記了一筆 NDIS 的 LiveKernelEvent 15e）。同樣的順序再跑六輪沒有再發生；前三天也沒有這種當機。原因未明，再遇到要留 dump。
- **要使用者決定的**：sec-G001#1 還沒設 Web UI 密碼時區網內任何人可搶先設定（上游行為）；Android 配對狀態信任明文 HTTP 的回報（上游行為）。

### §VR-FOVEA-TILES 注視點＝每眼兩塊影像（2.0.121；設定頁「關／小／中／大」2.0.122）
- **待驗**：設定頁選 medium／large 的配戴比較（只做過無人量測）；
  同位置同遊戲和 Steam 內建串流的配戴對照（使用者）。
- **「半速＋補幀」也用分塊（client 2.0.129）**：頭盔無人驗過（`temp/launchertest/ml/tsynth_1111/`，模擬手部擺動＋模擬視線），
  **沒有配戴看過**（補幀的殘影、注視點那塊邊緣只有戴上才看得出來）。dev 的 `--vr-fovea-single-layer`、`--vr-unwarp-rgb` 仍然不宣告分塊。
  存畫面（`--xr-dump-frame`）每次會讓繪製停約 0.7 秒，量流暢度時不要帶。
- **§VR-FOVEA-QP 周邊那塊量化加粗（`vr_fovea_periph_qp`；11-11 使用者指定預設 26＝Steam 同級，已部署 host）**：**配戴還沒看過**
  （Steam 注視點那塊佔視野 53%，我們「小」只有 38%，周邊露出來比較多，太糊就調低）。量測在 `temp/launchertest/ml/qp_1111/`、`buf_1111/`；
  同場的結論：顯示緩衝維持 1 格、「沒人收音就不送」不做；「大」剩下比 Steam 多的約 1.1 W 不是位元造成的。
- **§DEC-STUCK-RESET＋FFmpeg v4l2m2m LAST 緩衝區補丁（client 2.0.126～128；
  `moonlight-qt/scripts/steamframe/flatpak/ffmpeg-v4l2m2m-last-buffer.patch`）**：補丁實際生效的樣本只有一次（連續 14 場短場次
  `scripts/vr/startup_soak.sh` 的第 14 場，log `capture buffer 0 outside draining: last=1 bytesused=0 -> decoder START sent`）；
  自動重建留著當保險，可用 `--vr-inject-dec-stuck 30` 模擬。
- **之後可選**：三層金字塔（同像素量下交界更淡）、NVENC 外部移動向量提示（注視點那塊跳動時幾乎不花碼率）、周邊銳化。

### §VR-NETBENCH 設定頁「測試網路」（2.0.107；頭盔兩輪實測的結論：保留家用 Wi-Fi）
- **待查**：頭盔硬體解碼器（v4l2m2m／iris）偶爾整階 0 張解出（in=2 out=0）。收尾卡住已繞過（§DEC-JOIN-TIMEOUT，2.0.109：
  等 3 秒就放生解碼執行緒），原因未知，再遇到要抓 dmesg。
- **待驗**：§VR-ABR-SATURATED-NO-RESTORE（2.0.110）——碼率爬到上限附近時有沒有再出現「拉回→再斷」；§VR-RATE-CAUSE（2.0.106）——
  實機還沒遇到頭盔端降速；§VR-BUFFER-SHED（2.0.115，斷線後多壓的一格 3 秒內讓掉）的配戴體感。
- 每眼 2304 是頭盔端上限（網路 0 丟失仍跳格），階梯上限定在 2016。
- 重錄母片的注意事項：host 傾印 4608×2304 會暫存約 57 GB；NVENC 轉 4608×2304 的 `-maxrate` 要 ≤ 600M；
  頭盔解析度改成 2304 後平常的遊戲解不開，錄完要改回。

### 頭盔家用 Wi-Fi 的空檔（§WIFI-SCANBLOCK 2.0.114 之後剩下的）
- **仍有另一種空檔（待查）**：11-09 的 8 場中 4 場有 156／250～290 ms、一次 5.5 s——wlan0 沒斷線、beacon loss 0，host 端看到
  那條連線前 20 秒就開始掉追蹤封包，頭盔系統 log 沒有任何事件、跟掃描無關（可能在路由器端）。畫面都靠轉接器那條接住。
- host 的轉接器網卡偶爾連著但收不到：§VR-HOTSPOT-RXSTALL（2.0.108）連著但 20 秒只送不收就重啟網卡。11-09 有一次是收、送計數
  都不動，手動重啟網卡才恢復——看門狗有沒有涵蓋這種情況待確認。

### §VR-NETSEL 網路狀態選擇＋§VR-MIC 頭盔麥克風（v2.0.75；選網路 2.0.107 起由設定頁「測試網路」決定）
- **§VR-NETSEL**：`viplestream-netmode.service` 代為暫停／接回家用 Wi-Fi（心跳 5 秒沒更新、轉接器沒連線、6 小時上限都會交還）。
  遊戲中的自動比較已拿掉（暫停再接回會讓頭盔 Steam 登出再登入、觸發 steamwebhelper 崩潰），「自動」只套用設定頁「測試網路」
  存下的結果。11-08 頭盔實測過：服務可以暫停 wlan0、區網優先（§LAN-PREF）、轉接器直連不套單連線上限、暫停時不降碼（§VR-NETSEL-HOLD）。
  **待驗**：§VR-CTRL-DOWN-S2C（控制通道斷線期間震動改走連線）與 §VR-CTRL-ROUTE 的 PONG 判斷（2.0.100）還沒在實際切換時驗到；
  斷訊後拉回沒拉（§VR-ABR-OUTAGE）那次的原因——10 秒行已加 `outage=` 欄位，再遇到時看。

- **§VR-MIC（11-05 模擬頭盔端到端通過：client 每 10 s 送 500 則、host 收 501 則、0 掉包、緩衝 50～70 ms、開場 1 次 underrun；host 裝置 float32／48 kHz／2 ch）**：頭盔 SDL 錄音＋Opus（48 kHz 單聲道、20 ms、24 kbps、in-band FEC）→ C2S MIC 0x0D（VR 控制通道 unsequenced）→ host 解碼寫進
  「Steam Streaming Microphone」播放端（做法同 Apollo PR #1428）；`vr_mic`（預設開）、能力位元 `VIPLE_VR_SERVER_CAP_MIC 0x100`、找不到裝置回 NOTICE 5；
  設定頁「把頭盔麥克風送到電腦」（預設開）。兩個模組 gemini 寫、我核對並修過（COM 物件全在播放執行緒、SDL 子系統釋放）。
  頭盔實機 11-11 驗過背景開啟與送達（每 10 秒 500 則、drops=0）。**待驗**：host 上的裝置名稱與格式、實際收音品質與延遲、掉包時有沒有嗡嗡聲（WiVRn #1012 的教訓）。

### §VR-RADIO 頭盔單一無線電輪流用的掉包（2026-11-05 討論；11-08～09 量過）
- 頭盔只有一個無線電，家用 Wi-Fi 與轉接器熱點兩條連線輪流用；離開熱點頻道的那段是整段連續掉包（20～30 ms＝144 Hz 的 3～4 張），
  FEC 救不回，現在靠「每條各送一份」接住（同一個無線電多搬一倍）。
- **未做**：多連線改成「主路完整＋備路 FEC、不穩才雙送」（省空中流量）；讓頭盔經熱點由主機分享上網（要改頭盔路由／DNS）。

### 2.0.59 待驗（§ABR-ADAPT、§TS-PATH、§VR-BUFFER-AUTO、§REVIEW-1104）
- **頭盔 Tailscale operator**：頭盔端 rebind 要 `sudo tailscale set --operator=steamos` 一次（使用者執行），沒做的話頭盔只能 ping、中繼恢復較慢。
- **§VR-RATE-AUTO（2.0.96，自動半速，不重開 session）**：11-08 配戴一場（144 Hz）升降時機正確、半速節奏鎖有效（3 格以上重複每 10 s 0～1）。
  2.0.97 起長凍結（每 5 s 視窗 ≥3 次）＝中斷，不算降速理由；11-08 下午在家兩場開場短重複 63／71 被擋下、整場全速（`temp/launchertest/ml/worn_1108_lan/`）。
  **還沒做**：降解析度那一半（第 4 期另一半）。
- **§ABR-LOSSRATE-VERIFY／§ABR-RESTORE-STABLE（2.0.94～95）**：11-08 配戴看到兩者都有動作（撤銷 3 次、不拉回沒撐住的碼率）；再看幾場遠端的碼率曲線。
- **補幀畫面目視**：vkfruc_interp 4-tap shader 第一次真的生效（5 月 24e98950 的 .frag 從沒重編）。NCNN（RIFE）後端輸出全黑見「§REVIEW-1111」一節。
- **沒有環境驗的**：relay 打洞改用 STUN 實際埠（Qt＋Android，要沒有 Tailscale 的外網）；Android 開 MP-QUIC 時行動網路保活與 Wi-Fi→行動網路接手（要有 SIM 的手機、host 可從外網連）；Linux VAAPI 實機。
- **2.0.63 發佈後可驗**：host 常駐圖示走一次真實的「Check for updates」（§SELF-UPDATE）、電腦版 client 更新對話框的摘要（§UPDATE-NOTES）、Windows 更新等 90 s 不覆蓋的提示視窗。

### Linux 安裝包與更新摘要（2.0.33）

- **§PKG-SYSTEMD 待做**：使用者撤銷授權後重登入，目視對話框顯示「VipleStream-Server」；`cachy_install.sh` 已補 setcap（11-12），CachyOS 上還沒驗。
- **字型**：串流 overlay 先用 AppImage 內建 Noto CJK、補 Arch 路徑——待串流中目視。
- **§UPDATE-NOTES**：電腦版 client 更新對話框的摘要實際流程要等下一版發佈才看得到；host 常駐圖示的更新檢查還沒有摘要。

### §PYROWAVE 小波編碼（2.0.23 起；只能手動選，不進 Auto）
- **Steam Frame（2026-10-28，2.0.23，2D 桌面模式 1080p60 80 Mbps、家用 Wi-Fi）待查**：頭盔上主機處理延遲只在暖機回報一次、之後都是 n/a（Linux 有持續回報，所以是頭盔端的問題）。
- **Linux qt 已驗（2026-10-28，2.0.23 AppImage，AMD Vega／RADV，1080p60 50 Mbps 經 Tailscale）**：零複製成立、丟幀 0、
  解碼 p50 0.53 ms、主機處理延遲 1.1～1.7 ms、繪製 60 fps。有一段約 6 秒解碼升到 14～24 ms 後自行恢復（原因未明，網路與 host 正常）。
- 未驗證（需實機）：Android 手機（bridge 已建置）。

### §Q 雲端審查（2026-10-23，PR #4～#7）延後項目
- **QUIC FEC 不滿 4 片的群組**（審查 #6，nit）：server 的 §5b FEC 每 4 片資料才產生 parity、群組跨幀累積，一幀結尾 1～3 片要等下一幀才有保護（靜態畫面時可能很久）。修法要在幀尾補發不滿群組的 parity，client 需能辨識不滿 4 片的群組＝**兩端線上格式要一起改**，要先規劃相容性（舊 client 對新 server）。
- **failover 實測**：2.0.18 server 的 §Q-REINJECT-CNXQ／§Q-VIDEOPATH-ID／§Q-SINGLE-SURVIVOR／§Q-GRACE-CAP 還沒用 `scripts/failover_regain_test.ps1` 跑過（要按 UAC）；審查 #5 的 VR 相關修正要戴頭盔驗。

### §VR 補充（2.0 Steam Frame）

- **§VR-HOTSPOT-WATCHDOG（2.0.32，已做）**：VR 串流中，host 連頭盔熱點的網卡斷線 30 s 就重啟驅動，每場最多
  `vr_hotspot_restart_max` 次（預設 2）、間隔 ≥2 min，用完就在 log 提示實體重插。10-31 用 WLAN 封鎖過濾器模擬卡死，
  無人場次驗過完整流程。待做：真的卡住時（8002 不斷重複、收送計數歸零）看重啟能不能救回；網路上還沒有 Windows 端同類回報，
  再發生就存 WLAN-AutoConfig 事件 log（約 40 分鐘會被洗掉），考慮回報 Valve。

- **§VR-MULTILINK 多連線（2026-10-06 起）**：實測與設計見 `docs/steam_frame_client.md` §8.7、線上格式見
  `docs/vr_protocol.md` §4.10。待做：
  - 還沒驗的：§VR-LINK-FRESH（`always` 模式）在真頭盔弱訊號位置；§VR-LINK-REHOME 沒有路由的那條分支；VR 設定頁的版面待使用者看；
    §VR-HOST-LOCKED（host 停在登入／鎖定畫面時回明確的錯誤）鎖定狀態下的實測。
    已知：SteamVR 在 Frame 上不給眼動取樣時間（noTime 100%），sampleAge 永遠 0，非我們的 bug；Frame 的 iris 解碼只給 8-bit 輸出、
    Main10 會讓 client 崩潰（頭盔上已擋掉）。
  - 遊玩位置的連線量測（`scripts/vr/vlpt_run.sh`，需要使用者把頭盔放在遊玩位置約 10 分鐘）。
  - 被退回探測的連線，條件恢復後的重新放行（第二次實測沒有回來：探測量到 20～198 Mbps、門檻約 225～285）。
  - **10-08 晚配戴三輪的結論（`steam_frame_client.md`「10-08 晚配戴實測」）**：不夠流暢的主因是 repeat／skip。待做（要配戴）：
    和 Steam 內建串流同位置同遊戲對照。
  - **10-09／10：頭盔的 GPU 忙碌期定案＝SteamVR 合成器每格一筆高優先權的 GPU 工作（`steam_frame_client.md`「頭盔的「GPU 忙碌期」
    定案」）**。接下來的 client 改動做過一輪設計（2026-10-10；一次一項、都先放在開關後面、預設關），剩下的：
    - 等在途的幀（§VR-LATCH-FLIGHT）：挑影像時有影像已經送進 GPU 還沒做完，就多等最多 0.5 ms。已實作、預設關（保存鍵 `vrlatchflightms`、
      `--vr-latch-flight <ms>`），沒有配戴過。判斷條件與驗收場次在 `steam_frame_client.md`「頭盔端管線下一輪」。
      下一步：驗收指標改成同場的 `rescBy` 前兩格與 `events`；補跑全速／半速 A/B、整場開／關、
      驗閘門、XR 桌面與結束路徑；無人過了才配戴（每眼 1152）。
    - **頭盔影片模式（§VR-PLAYER）與全 VR 介面（§VR-SHELL）**：§VR-SHELL-SCROLL、§VR-SHELL-LAUNCHING 的配戴確認沒有紀錄（待核對）。
    - **§VPN-ADDR（10-21）**：server 在 `/serverinfo` 廣告 VPN 介面位址（`VipleStreamVpnAddress`），client 只收 100.64.0.0/10、
      存成 `vpnaddress`、當最後的候選位址。原寫「待在外面實測」；11-13～15 外地場次用過 Tailscale 位址，是否算驗過待核對。
    - 解碼輸出改成 100 µs 短輪詢（§VR-DEC-POLL，少 0.1～0.15 ms）；補幀那一批少一次色彩轉換（§VR-SYNTH-LEAN，少約 0.32 ms）。
      其中「上一張的小圖每次都被宣告成無效」是未定義行為（補幀的客觀檢查都過了，表示目前的驅動上內容還在），
      要不要不設開關直接修，**待使用者決定**。
    - 條件式：送出就發布（§VR-PUB-EARLY）要先做「故意把 GPU 工作拖長」的實驗，那會把頭盔的合成器推到期限邊緣，
      要使用者在場；補幀那一批避開合成器（§VR-SYNTH-DEFER）要先在配戴場次重現重投影。色彩轉換併進注視點還原只對全速有用，選做。
    - 「延後挑影像」目前是固定毫秒數，換到 90／144 Hz 時不會跟著週期變；之後要改成依顯示週期或 runtime 的回饋。
  - 頭盔的 Flatpak 裡執行緒升不了優先權、runtime 寫不出我們的 `xrclient` log（沙箱看不到 Steam 的 log 目錄）還沒處理。
  - 經網際網路（Tailscale）串流（`steam_frame_client.md`「第一次經網際網路（Tailscale）串流」與 2026-10-10 的兩段對照）：
    - **全速時救不回的幀（每 3 秒左右一次、每次約 90 ms 的局部破圖）**：§VR-REPAIR-RTT（門檻、等待、重報都跟著量到的往返時間走，
      只在單一連線時啟用、server 開關 `vr_multilink_repair_rtt` 預設關）已實作。剩下：
      - 60 fps 時兩種規則的回報都偏多（每 10 秒約 75 份、用得上的不到 2 份）：「多久沒有新封包就回報」在 60 fps 是固定 3 ms，
        這條路徑的幀內停頓常超過它。改成跟著量到的到達間隔走（`--vr-repair-idle learned` 已經有、還沒驗）可以省 `<host>` 白送的補包。
      - 60 Hz 重度成串掉包時新規則比現行規則差一些（已知的限制）：評估改成「現行規則的門檻不成立時才換新規則」，
        讓半速維持現行規則。
      - 每幀封包數的估法（前一張是大幀時會把後面一兩張的空白判成補不完）、預先報過的整幀在輪到之前不重報、保險對一次事件
        記好幾筆失敗——都只寫進已知限制，還沒改。
    - **`<host>` 到頭盔的 VPN 直連閒置後會掉到中繼（往返約 100 ms，串流不能用），有時兩個小時以上回不來**：2026-10-10 五次。
      原因查到了，在兩端的 NAT（同一個 NAT 後面有兩三台 VPN 節點共用同一個 UDP 埠，對外埠對不上；同一個網路的另一台節點
      連著 `<host>` 時頭盔就一直建立不起來，見 `steam_frame_client.md` 的實測）。**待使用者決定**：家用路由器加一條把 VPN 的
      UDP 埠固定轉給 `<host>` 的規則（治本）。程式這邊：開始 VR 串流前先量往返時間，超過 60 ms 就在頭盔上明講「連線太慢、
      通常是 VPN 走中繼」，繼續量、變快了自己開始，也可以選「仍要開始」或取消（§VR-PREFLIGHT，2026-10-11）——
      **已實作，還沒在頭盔上驗**（`steam_frame_client.md`「開 VR 串流前先量到主機的往返時間」；本機測試在
      `moonlight-qt/tests/vr_preflight/`）。待做：在頭盔上看對話框與雷射／手把的操作（dev 旗標 `--vr-test-preflight 150*8,30`
      不必等 VPN 真的掉到中繼）、VPN 在中繼時實際量到的值；自動開始要不要有時間上限、25–60 ms 要不要提示，待使用者決定。
      **更正：家用路由器前面還有一層 NAT。** 2026-10-11 使用者同意後在路由器加了那條轉埠，加完才查到路由器的對外位址是私有位址；
      從頭盔實測，外面主動進來的封包到不了路由器（原本那三條串流的轉埠也一樣不通）。要在上游那台裝置處理（轉埠、DMZ 或橋接），
      **待使用者處理**；路由器上的規則留著，上游打通後就會生效。
    - **VR 的位元率放進設定頁**：頭盔上「自動位元率關、位元率 50 Mbps」對 VR 沒有作用（VR 用自己的預設，全速 88.9 Mbps、
      實際送出 104 Mbps）。50 Mbps 實測：流量少四成、延遲少 4 ms、救不回的幀少約兩成半，重複顯示不變。要不要做、預設值待使用者決定。
    - 自動位元率被區網用的規則釘在下限（使用者說以後再處理）。
    - **設定頁用控制器操作（§UI-SCROLL，2026-10-11，使用者回報在頭盔上用雷射指標捲不下去）**：已改——右搖桿與 LB／RB 捲頁、
      Frame 上捲軸常駐可以用雷射拖、VR 模式下設定頁一打開就在 VR 那一區、下拉選單用手把可以上下選；Windows 上用虛擬手把驗過。
      **待使用者在頭盔上確認。** 還是不行的話先看介面行程 log 的 `[VIPLE-UI-NAV]`（手把有沒有被看到、輸入有沒有進來）。
  - **10-09 已實作、待頭盔驗證（都在開關後面、預設關；`steam_frame_client.md`「依這三輪做的改動」有下次配戴的順序）**：
    client 量測 `[VIPLE-VR-LDIAG]`（一律開）、host `vr_latch_mode = v3`＋`vr_latch_acquire`（Monado 上驗過：一步到位、約 3 s 進入追蹤、
    driver 沒有多漏格）、client `vrtrackrelock`／`vrlatchrelatch`／`vrlatchwaitms`／`vrlatchabsec`、dev `--vr-test-jitter`。
    頭盔已裝新版（10-09 起的無人場次用它收 LDIAG）；各開關的配戴驗證還沒做。
  - **§VR-FENCE-LATE（2026-10-09 事故：遊戲的 GPU 負載一重 → 擷取端每一張等 fence 都逾時 → 每 1.5 s 拆線重連、頭盔全黑；
    `steam_frame_client.md`「host 擷取端的 fence 晚到」）**：server 端已實作（`vr_capture_late`），`<host>` 的設定是 enabled。待做：
    - **過載旗標改成表達「Present 鎖不住虛擬 vsync」：已實作（§VR-CADENCE；`vr_capture_overload = late | cadence`，預設 `late`＝
      舊定義，偵測器照算照印）**。細節在 `steam_frame_client.md`「已知的限制」。待做：
      - **配戴驗證**（300% 上下、AA 3 或換一個比較重的遊戲）；驗過才把 `vr_capture_overload` 與 `vr_capture_late` 的預設一起改。
      - 340／350% 的頭盔場次（`lockR`）、中途負載回落（B1）與重負載下重新載入（B2）兩場還沒跑。
      - 臨界負載：由鎖得住升上來時有 1～7 秒照常取樣的空窗；不到約 1.7 秒的短過載每次要 12 秒才清；locked 與
        unsure(fraction) 之間沒有黏性。
      - v3 的節拍閘門吃 divisor；§VR-PREDICT 改看 host 延遲的層級變化；進 hold 時把積分退回樣本變壞之前的值；
        把狀態放進 STATS 給頭盔顯示。
      - `vr_capture_late_max_ms` 的預設 100 一併檢討（遊戲載入之後量到的 `presentToFence` 最大約 10.7 ms；SteamVR 剛啟動的頭幾張
        到 16～26 ms，其中一張等滿了 25 ms 的預算下限）。
    - **負載重時 `<host>` 編碼段的雙峰（§VR-GPU-PRIO）**：原因是 VR 擷取期間 server 行程的 GPU 優先權只有 HIGH；
      `vr_gpu_priority` 預設已於 11-11 改 realtime（使用者定案；數字在 `steam_frame_client.md`）。還缺：配戴驗證、第二個比較重的遊戲、
      更長的耐久（只有 340%、整場 realtime 連續 10 分鐘一場）。
    - §VR-PREDICT 在過載時的兩個小洞：旗標設起來之前的那一個 CLIENT_TIMING 視窗仍會被採計（追蹤階段最多改 3 ms，也會寫進
      下一場的起點）；探測走到一半遇到過載時，前後的視窗會隔著過載相比。要補的話由 `stream.cpp` 把旗標由關變開的那一刻通知
      `vr_predict`（探測中就退回重量、追蹤中就把最近那一次修正退回）。
    - 開關關著的舊路徑有兩個本來就有的洞沒有動（開關開著時已經補上）：ring 開不起來一直重試、以及每一張 descriptor 都過期時，
      擷取迴圈不會呼叫任何回呼，session 結束時擷取執行緒停不下來。改預設、拿掉舊路徑時一起收掉。
    - 頭盔端顯示「host 的 GPU 過載」與「host 的 VR 連線中斷」；黑幀、姿態無效的幀不算 live（現在拆線期間頭盔只看到黑畫面，
      沒有任何提示）。把過載狀態放進 STATS TLV 送給 client。
    - `vr_probe ipcpeer --peer-gpu-hold-ms`（目前只有參數、沒有實作）與 selftest 的 T2.gpu-hold；`selftest_consume()`
      （T2 的消費者，仍是固定 2 ms＋1 秒規則）改用 `vr_capture_health.h` 的同一份判定，T2.stop-signal 的預期時間跟著改。
      有了假 driver 之後要實測的一項：停止 Signal、等到 `teardown deferred … allowedInMs` 大於 10000 的那一段結束串流，
      `sunshine.log` 不應出現 `Hang detected`（目前只用模型推演過）。
    - 還沒用第二個遊戲驗（推論：畫質調到剛好跑滿更新率的遊戲，在開關關著時一進去就會丟幀或全黑；也還沒有量過任何遊戲
      實際的 GPU 時間）。
    - 累計統計裡 `late`／`over_period`／`fence_wait_ms`／`present_to_fence_ms` 四個欄位只寫不讀（10 s 行用的是視窗那一份）：
      拿掉，或讓 selftest 的結果行用它。
    - 依完成順序取用（改 `read_latest()`）：`presentToFence` 已量到（見實測）；等 `skipped` 與 driver 的 `noslot` 整理出來
      再決定要不要做。
    - 讓合成器固定自己合成（與遊戲的幀脫鉤；儀表板開著時就是這個狀態，代價是遊戲幀率再低一些、多一次重新取樣）：只評估。
    - driver 的 `Present()` 等合成器同步貼圖也是寫死的 5 ms（第二個固定門檻，這次沒有踩到）。
  - STUN 的公開位址只在啟動時量一次：要週期性更新得先換一種不佔用控制埠的探測方式（現在用 SO_REUSEADDR 綁在控制埠上）。
  - `Sunshine/tests/integration/test_config_consistency.cpp` 的 AllConfigOptionsExistInAllFiles 在 vr_* 的設定鍵上本來就會失敗
    （這些鍵沒有進 web UI 的 config.html／en.json）；要嘛把 vr_* 列進該測試的 internalOptions，要嘛補進 web UI。
  - 開場 30～40 s 的重複，七到九成是 host 在 SteamVR 啟動與遊戲載入時沒出幀（每場 200～250 張），需要 host 給「遊戲開始出幀
    且穩定」的訊號才能遮住，還沒做。
  - 配戴實玩時 host 的 evtToPush 由 0.97 變 2.38 ms（擷取要多等 GPU），連同遊戲算圖變久讓 MTP 多約 4.5 ms。
    原因在 10-09 查到了：配戴時儀表板關著、遊戲貼圖直接當第 0 層（直通），driver 的合成排在遊戲這一幀的 GPU 工作之後，
    等 fence 就是在等遊戲算完（見上面的 §VR-FENCE-LATE）；無人場次儀表板多半開著所以量不到。能不能省（例如讓合成器自己合成）還沒評估。
  - **半速串流＋補幀（2026-10-10 第一次配戴之後；`steam_frame_client.md`「半速串流與補幀」「2026-10-10 配戴後查到的另外兩個問題」）**：
    使用者回報畫面整個不對、一直閃。已修並在頭盔上用存圖驗過：(1) 注視點＋補幀時先發布的那一張畫錯；(2) 合成那一格偶爾顯示成
    下一張沒畫過的下半（加了防護）；(3) 注視點還原右眼的垂直中心用了左眼的值（實測右眼中央偏 0.57°，全速也有）。
    (4) 半速時 `<host>` 的 §VR-PREDICT 不調整、起始值算錯（整場姿態超前約 23 ms；`/launch` 新增選填的 `vrDispHz`）：已修，
    2026-10-10 中午用新加的量測在頭盔無人場次整條驗過（`--vr-synthetic-hmd` 的姿態落後改對合成軌跡量；半速落後收斂到
    −0.2～+0.7 ms、全速 −0.6～−0.9 ms）。補幀另外加了客觀檢查（在動的東西被往對的方向推、推了九成多；`synth-check/flow_check.py`）。
    **待使用者重新配戴**（要特別看：快速揮動的球拍邊緣、桌球這種小東西在半速時會不會一頓一頓）。還沒做的：
    - 無人場次的量測工具：合成基準仍綁在頭盔真的 locate 成功之後（locate 失敗的那幾格會拿凍結的基準去比）；合成的手與 MENU
      仍用取樣當下的時間（相對於頭的相位落後一個預測長度）；dev 合成場次會改到 `<host>` 學到的預測時間——現在靠腳本在收尾重啟
      server，根治是 client 在 `/launch` 標示 dev 合成場次、`<host>` 照樣調整但不存。
    - 60 Hz（半速）時 `<host>` 落在多一格的時序會一直留著（2026-10-10 中午之後六場無人半速場次有兩場，MTP 約 71 對 53～58 ms）。
      **已做成自動（§VR-DIRECT-KICK，`<host>` 的 `vr_direct_kick = enabled`，預設關）**：讓 driver 的 PostPresent 立刻返回約 20 ms
      把它推回去。2026-10-10 在模擬頭盔（60 Hz 兩次都在 3～4 秒內推回、120 Hz 不動作）與真頭盔半速（3 秒後推回，MTP 55.6 ms；
      不處理是 70.5～71.8 ms；10 分鐘、開場就不多一格的那一場 0 次誤推）驗過。待做：
      - **配戴驗證**，驗過才改預設。
      - **載入畫面的誤判（2026-10-11 在實際的場次看到：遊戲還在載入、第 0 層是合成器自己的畫面時連推三次都沒有變化）已改**：
        判定多一個條件——擷取端交過來的回聲年齡至少 24 筆、中位數 ≥ 5.5 ms＋半個週期（60 Hz 13.8 ms；真的多一格是
        17.7～20.7 ms）；不推時每 60 秒印一行 `[VIPLE-VR-KICK] not kicking: …`。已部署到 `<host>` 並用模擬頭盔驗過（2026-10-11）。
        沒有重現到的只剩「client 已經斷線、SteamVR 還在出
        合成器的畫面」那一種（模擬頭盔做不出那個 `presentPhase`），以及真頭盔與配戴。原本寫的驗法（細節在
        `steam_frame_client.md` 自動化那一段的「還沒做的」）：遊戲還沒出幀時把 client 斷線 30 秒以上，不可以有 kick、要有
        `not kicking … n/a over 0 frames`；連回來做出真的多一格，3～4 秒內推回而且編號是 #1（無人場次要讓頭動起來——頭部
        靜止時回聲年齡無效，真的多一格也可能不推）。
      - 回聲年齡這個條件已知的三個弱點，都先靠 log 收數據再決定要不要改：(1) 只比中位數的下限——追蹤樣本一陣一陣到、
        相鄰兩批隔 22 ms 以上時形同不存在（kick 與 `not kicking` 行印了第 10 百分位來分辨）；(2) 門檻是照「多一格＝多整個
        週期」定的，量到的只多 13 ms：60 Hz 離多一格的下緣 3.9 ms，72／90 Hz 外推只剩約 2.4／1.0 ms（要置中就把式子裡的
        5.5 ms 換成約 4.3 ms）；(3) 對到姿態的影像不到四成（60 Hz）時真的多一格也推不出去。
      - 推回成功時 `<host>` 的姿態預測要約 15 秒才追回少掉的那一格：成功當下直接把預測時間減一個週期。
      - 偵測器的狀態跟著 driver 的連線走（橫跨待機、重新 arm、換遊戲），而且兩個「不再推」（20 次上限、重負載）都不可逆；
        成功的判定用絕對值 2 ms、判定落在多一格的門檻 1.15 ms 兩邊各只有約 0.05 ms 的餘裕；72／90 Hz 會動作但沒有量測
        （回聲年齡的門檻在這兩個更新率是照公式算的）。
    - 不帶 `vrDispHz` 的舊 client 開半速時，`<host>` 把那一場學到的姿態預測時間存成「60 Hz 頭盔」的那一筆，之後同一個 server
      行程開全速會由它換算（偏小約 11 ms、約 20 秒追回）。只發生在新 server 配舊 client；根治：收到 CLIENT_TIMING 的顯示週期
      之後把這一場的鍵改對。
    - 補幀的防護觸發後記帳沒有跟著改：下一格會再顯示一次真的那一張、合成那一格晚一格才出現，LDIAG 也多記一次 lateShift
      （穩態每 10 秒 0～1 次）。
    - 半速時 `<host>` 的 v2 LATCH 會吃到「晚一格才顯示」的影像的折返樣本、往反方向修（slips 每分鐘 9～16 次，全速 3～5 次）：
      client 認出這種影像不回報，或半速改用 v3，擇一評估。
    - 補幀那幾個 pass 的 GPU 工作正好跨過頭盔合成器的那一段：配戴的兩場 runtime 重投影 0.11～0.23%（全速 0.01%）。
      合成那一張不急，可以等合成器那一筆做完再開始。
    - 半速的驗收不能看 `[VIPLE-VR-MTP10]` 的 repeat／skip（晚一格才顯示的影像看不出來），要看 LDIAG 的
      `lateShift + 2×late + 2×gap + early`；補幀開著時 `mismatch` 大多是同一張影像的第二次發布、`heldMiss` 與 `age` 的意義也不同。
    - 小洞：顯示 90 Hz 時半速不生效但補幀照開（白做 GPU 工作）；判斷前後兩張連不連續的基準是函式層級的 static、開場第一筆
      間隔異常時整場不合成也沒有警告；第二張 render 失敗時仍然 release。
    - 注視點**關著**時，補幀估位移的 pass 把「正前方」當成影像正中央（這台頭盔實際在左眼 (0.582, 0.398)、右眼 (0.415, 0.408)），
      轉頭時頭部轉動的位移預測最多差到 2 格，靜止的景物可能被估出不存在的位移。目前使用者都開著注視點、沒有踩到。
      做法：不論注視點開關都把真的中心傳下去，只用斜率表示開關；改完要另外在頭盔上存圖驗。
    - 審查提的其他小項：client 第一次用到注視點時印一行還原用的中心（左右眼各一組），之後不必靠存圖就能核對；補幀估位移
      兩眼共用左眼的視角總和（差 0.2～0.5%，不到一格）；還原時每眼靠鼻側最外面一欄的雙線性取樣會混到另一隻眼（補幀的下半有夾、
      上半與單獨還原的 pass 沒有）；`vrlaunchparams.h` 檔頭說 relay 共用 `toQuery()`，實際 relay 不要求 VR。
    - 經 Tailscale 時半速的自動位元率下限跟著減半（約 2.5 Mbps，每張影像的位元數和全速在下限時差不多）；位移估計在這種
      畫質下會不會把壓縮雜訊當成位移，沒有驗過。
    - 合成那一半在快速移動的物體邊緣有碎塊與鋸齒（位移場是 1/8 解析度）；配戴看起來如何、要不要再處理，等使用者看過。
  - **`<host>` 直通時的兩種時序（等遊戲這一格／多一格；`steam_frame_client.md`「host 直通時的兩種時序」）**：10-08 記的
    「一場半速串流整場 MTP 高 13 ms、原因未查」就是落在多一格的那一種。結構已查清，進出的規則還沒有直接觀察到。待做：
    不改程式的實驗（儀表板開著啟動、用合成的 MENU 鍵關掉確定進入多一格，再依序暫停遊戲行程 35 ms／150 ms／1.5 秒）；
    server 把 timing ring 裡已經有的節流參數、傳入旗標、預測秒數印出來；之後每一場的紀錄都記狀態（`presentPhase` 中位數
    約 3.5 或約 0.77 ms）；無人頭盔場次的腳本收尾改成先結束遊戲再結束 SteamVR（才拿得到 SteamVR 自己量的遊戲 CPU／GPU 時間）。
    要不要把那一格拿回來（60 Hz 輕負載約 12 ms；120 Hz 重負載時留著反而沒有 fence 晚到的問題）等同場對照再定。
  - 頭盔端 dmabuf 匯入失敗會讓 client 崩潰（目前只有 10-bit 會觸發，已擋掉）：要有除錯版才能定位崩潰點。
  - LATCH 帶線上的幀號（補過包的幀不該進相位量測）；CLIENT_TIMING 仍只走 ENet（不急）。
  - 單一路徑的影像送出也改成非阻塞＋過期丟棄（Windows 對送不動的 Wi-Fi 網卡一次會卡 10 秒；多連線沒啟用時仍是舊行為）。
  - USB 網路（頭盔 NCM gadget ↔ Windows UsbNcm）一有 UDP 流量就卡死，觸發條件未定位；`pnputil /restart-device` 不可靠
    （第二次留下 Port Reset Failed，要實體重插）。
- **2.0 release notes 必寫**：Android 自 2.0 起改為 release 建置、package `com.piinsta`（原本出貨的是 `com.piinsta.debug`／「VipleStream (Debug)」）。兩者是不同 app，舊版不會原地升級：使用者要安裝新版、重新配對，再自行移除舊的 Debug 版。release keystore 在建置機本機（見 `docs/building.md` §2.4），遺失就無法再發可覆蓋升級的版本。
- **M1a 後續（不擋 M2a）**：
  - `encode_run_sync` 沒有 VR 標記（Windows 全走 async，暫不影響）。
  - recovery=intra 的協商依據是「probe 選到 Windows NVENC」，不是 `NV_ENC_CAPS_SUPPORT_INTRA_REFRESH`；沒有 IR 的 NVENC 會退回 IDR（已處理 cooldown 內的補送），M1b 改看實際能力。
  - STATS 的 `staleCount` 等 M1b 時鐘對映。
  - **觀察（非 M1a，stream-quality 範疇）**：一般 session 的 QUIC 視訊在開場幾秒有丟包爆發，ABR 從 30–35 Mbps 連砍到約 5 Mbps（reset+IDR）再回升；22:38 與 23:40 兩次對照模式相同，屬既有行為，值得另開一輪查 BBR 起步。
- **M1b 過程發現的既有問題（非 VR）**：
  - host 的 service ImagePath 沒加引號（`C:\Program Files\...\tools\viplestream-svc.exe`，NSIS 安裝），屬 unquoted service path；一般使用者無法在 `C:\` 建 `Program.exe`，風險低，但安裝腳本應加引號。
  - service 實際執行 `tools\viplestream-svc.exe`，zip 與 self-update 只更新安裝根目錄那份 → service wrapper 從未被更新（host 上是 2026-04-25 版）。本機部署腳本已改成依 ImagePath 更新；self-update／zip 版面待 M1b S2-10 一起處理。
  - client CLI 對「已存在但輪詢中」的 host 發 `stream`：輪詢執行緒被停掉時印 `Aborting timed out request`（其實是 QThread::exit 結束巢狀 QEventLoop，非逾時）；配對遺失時 CLI 停在需要配對、沒有明確錯誤。
- **M2a 後續**：
  - Linux 的 `-O3 -flto` 從未生效（Ubuntu／KDE SDK 的 qmake 開 `force_debug_info`，release 用 `QMAKE_CXXFLAGS_RELEASE_WITH_DEBUGINFO`，實際 `-O2 -g`；x64 AppImage 也是）。要改就得動已出貨 AppImage 的程式碼產生，另案評估；`CONFIG+=disable-lto` 目前在 Linux 沒作用。
  - **錄動態內容的 decode-bench 樣本**給 PoC-3b：靜態桌面的 P 幀幾乎是空的，掉幀／破損後的恢復不具代表性，要改錄持續變動的內容（影片、遊戲、移動的視窗）。
- **Frame 實機（2026-09-28 到手；Day 0／Day 1 結果與 G-α 凍結根因見 `steam_frame_client.md` §7.4／§7.5）後續**：
  - **NVENC level／tier 固定**（`nvenc_base.cpp` `pin_level_for_reconfigure()`，ABR 降碼不再改 VPS/SPS；Windows 已部署，下列驗證項有沒有逐一做過待核對）：驗 `[VIPLE-ABR] NvEnc: pinned …` 出現、ABR 降碼後 `--dump-bitstream` 的參數集不變、Frame 不再出現 `parameter sets changed`；另確認 NvEncReconfigureEncoder 接受明確的 level／tier（被拒時退回自動選擇重試一次，log `reconfigure rejected with pinned level/tier`）。AMF／QSV 與 AV1 的 level 固定未做。**Linux host（FFmpeg `hevc_nvenc`／`h264_nvenc`）沒有覆蓋**：審查實測（RTX 3060）tier 同樣隨碼率翻轉，而且 FFmpeg nvenc 在 CBR 下 SPS 的 VUI 帶 HRD（`bit_rate_value_minus1`、`cpb_size_value_minus1`），**每一次** ABR 調整都改 SPS → Frame 的 client 每步都重建 decoder；要在 avcodec 路徑給明確 level／tier 並拿掉 HRD（或 client 改成只比對會影響解碼器的欄位，但哪些欄位會觸發 iris source change 要先驗）。
  - **§SF-PARAMSETS 的已知競態**：client 丟掉參數集改變的 IDR 後向 server 要 IDR，若落在 §S.19 IDR cooldown（1500 ms）內會被壓掉，要等 §FRZ-WATCHDOG／連續丟幀上限（1.3–2 s）才恢復。server 固定參數集後就不會觸發；要徹底解，可讓 ABR 降碼的 forced IDR 也更新 `last_idr_emit` 並豁免降碼後第一個 client 請求，或把被拒的 IDR 暫存給新 decoder。
  - **FFmpeg v4l2m2m 的 source change**：解析度不變時只送 `DEC_CMD_START`，iris 需要 capture 重新 STREAMON；capture 的 dequeue 用 `timeout -1`，driver 停住時 decoder 執行緒永遠出不來（連 session 都結束不了）。client 已用 §SF-PARAMSETS 繞開；長期考慮在 manifest 對 cgutman FFmpeg 補 patch（完整的 capture 重建＋有限 timeout）。
  - **β／rc 的 OpenXR 包裝形態**（2.0.2 起先以 `install-steamframe.sh` 的不隔 PID 啟動器上線，見 `steam_frame_client.md` §5.1；
    下面是長期方向）：Flatpak 內 SteamVR IPC 因 PID namespace 出錯，曾讓 Frame 的 SteamVR 重啟（`finish-args.md` §6）。`xr-probe` 要在 `FORM_FACTOR_UNAVAILABLE` 時依規格重試、退出前給 runtime 收尾。manifest 的 SteamVR 路徑（`xdg-data/Steam/…/SteamVR`、`~/.steam`）在 Frame 上都不對（實際在 `/opt/steamvr`），M3a 一起收窄。
  - **Linux 的 `[VIPLE-INPUT-STALL]` 誤報**：非 Windows 一律當作有輸入，PlVk 在 Pacer 執行緒繪製時主迴圈閒置就被記成停頓；要改成用 SDL 事件時間判斷或只在 Windows 啟用。
  - **多路徑（使用者 2026-09-29 指示：G-α 後再精進）**：量測式選路（§MP-MEASURE、§MP-SINGLE-AVAIL、client §MP-FAST-STALL 等）已上線，picoquic 多 available 時 per-path queue 餓死的根因已修（fork 4abcb195 §MP-PERPATH-WAKE／DG）。**待做**：在此基礎上恢復探測式品質換路（§MP-PROBE）並評估是否拿掉 §MP-SINGLE-AVAIL；standby 路徑的活性訊號（保活／探測）讓 §MP-FAST-STALL 每次都成立；接手後偶發 §Q-FAILOVER-PROMOTE 來回；使用者指示判斷一律看連線狀態、不綁特定硬體。另觀察：復原後 §Q-FAILOVER-PROMOTE 與 client REGAIN 駐留互拉造成影像在兩路徑間來回（既有行為）。待做：上述三項實測、接手偵測、Android（Pixel 5）回歸。
  - **msm 核心錯誤回報 Valve**：`drm_gem_prime_fd_to_handle` → `msm_gem_import` 失敗的清理路徑 `drm_gem_put_pages` NULL deref（SteamOS 6.18，trace 在 `galpha-20260929-r2b\B2b-kernel-oops.txt`）；client 已用 §SF-DMABUF-CACHE 避開。**2026-10-03 PCVR 第三輪再踩一次**（XR 影像路徑當時沒用快取、每幀匯入；這次整台頭盔卡死、只能強制重開），快取已改成 PlVk／XrVideo 共用的 `PlvkCommon::DrmTexCache`。核心這邊：上游 `e6863b085606`「drm/msm/gem: Fix dma_buf import error paths」（v7.3-rc1 起，沒有 stable 標記）已修，回報資料與 6.18 回移 patch 在本機 `scripts/benchmark/results/frame-msm-oops-report/`，由使用者回報 Valve。
  - **CLI 參數會被存成設定**：`Session` 開始時 `save()`，`stream --quic` 等覆寫會留在設定檔（上游行為）；是否改成 CLI 覆寫不寫回，待決。

### §B-NVOF autotier 判斷標準

PixArk 20+ 分鐘測試後看 log：
- `[VIPLE-VKFRUC-NVOF-PROF]` 每 60s 一筆 → `drop%` 接近 0 且 `chain_mean` < 2ms → **OK**
- `[VIPLE-VKFRUC-TIER]` tier 穩定在 T3-T5 → **OK**
- 若 `drop%` 高或 tier 頻繁 demote → 需 Option E (NVOF skip 機制)

### §B Phase B 下一步（需換測試機）

需要一台「D3D11VA HEVC HW decode 可用、Vulkan video decode HEVC 不支援」的 AMD 機器：
1. 串流 HEVC → log 出現 `initializeCompositeD3D11 OK` + `B7 first frame imported`
2. 連續 60 秒不出錯 → 進 B9 (FRUC chain + present)

### §Q MP-QUIC 剩餘待做（主體已 ship v1.5.202，完成史見 git log + memory）

| # | 項目 | 優先級 | 狀態 |
|---|---|---|---|
| Q.r5 | Android 多路徑實機驗測 | **P2 (hw-bound)** | ⚠️ 阻塞。Pixel 5 無 SIM (gsm.sim.state=ABSENT) → cellular 不可用；gnirehtet v2.5.1 在 Android 15 (API 35) VPN 建立失敗。**單路徑已驗 OK (v1.5.202 Pixel 5 實機 adb)**。需 ①插 SIM 走 WiFi+cellular 或 ②換 Android 12-13 裝置 |
| Q.r11 | 遠端高碼率 ABR | **P3** | 遠端 WARP/Tailscale 單路徑碼率須 ≤ 路徑可承載 (~8-15 Mbps)。若要更高碼率穩定 → 評估自適應 bitrate（client 偵測持續丟包 → server 動態降碼，兩端工程量中大）|
| Q.r12 | §Q-PERF 殘餘候選 | **P3** | v1.5.233 盤點留存：server 送端 fromAddr 來源綁定（WSASendMsg+PKTINFO，多介面送端成立）、audio failover 冗餘（scheduler 死碼活化或移除）、FEC parity 自適應（4+0/4+1/4+2 依 loss）、lossStatsThread 退出後不重生、session map 純 IP key 衝突、UDP socket SO_RCVBUF/non-blocking |
| Q.r13 | 驗測基礎設施 | **P2** | failover_test_auto.ps1 加 log 收集/斷言/IP 參數化；host 端丟包注入（CoworkMCP 於 2026-09-23 停用，改經 SSH 在 host 操作） |
| §SC-ID | §SC-HID Steam Controller 轉發 Round 2 收尾 | **P3 (hw-bound)** | Round 1 四根因修復已隨 v1.5.270／v1.5.271 發佈（Windows Puck、Linux 藍牙／USB、Android USB-C 皆驗過；完成史見 git log）。**Round 2 剩**：feature 回應改 RELIABLE（三份 common-c 同步）、控制器直連時 0x87 SET_SETTINGS 的 ack 由 client 合成（現在 host Steam 每次多等 400 ms 閘控逾時）、session 起始 ring backlog 延後 flush（host 在 client passthrough 就緒前不 flush，現在會有約 10 行 `forwardFeatureRequest: no device open`）、Steam Virtual Gamepad 抑制只在實機看到多一支 X360 才做；`write stats` 的 `drvVer` 位元組順序（source 已修，是否已隨建置生效待核對）；串流 B（本機 Steam 退出）未跑（選配）。Android 藍牙 SC 不可行（HOGP，平台限制），可再探 gen-2 是否另有 Valve 自訂 GATT 服務（SDL `HIDDeviceBLESteamController` 走 `100F6C32-…`）。diag 腳本在 `Sunshine/src/platform/windows/sc_hid_driver/diag/`（`sc_hid_probe.ps1`、`sc_hid_host_check.ps1`）；driver 簽章與 build 防呆（`check_schid_driver_fresh.ps1`）已完成 |
| Q.r16 | review batch 2 已知殘留（驗測觀察項） | **P3** | **驗測 PASS (2026-06-16)**：L1 30s smoke ✅、L2 R.3 firewall ✅、L3 kill+reconnect×3 ✅。殘留觀察項（不阻 commit）：①S5 stats 快取 idle 凍結（§K.10 log 顯示舊值，診斷限定）②invokeRecvHandler heap alloc per datagram（可改 shared_ptr+atomic swap）③LiGetEstimatedRttInfo 無鎖讀 peer（pre-existing TOCTOU）④A1 tick 在 video 全死但 control 活時仍 ramp（既有假設）|

備註：standalone cmake configure picoquic（除錯 picoquic 本身）需 pkg-config + OpenSSL dev headers；一般走 build script 不需。

### §SLIM Vulkan Native RIFE fix

lazy fetch 兩條路徑都已接 ModelFetcher（見總覽 §SLIM）。還沒做過的驗測（待核對）：清 `%LOCALAPPDATA%\\VipleStream\\fruc_models\\` → registry vkfrucEnableNativeRife=true → stream → 預期看 [VIPLE-MODELFETCH] download flownet.bin 5-10s + SHA-256 OK + RIFE init success

### §M.2 Session Ownership（已完成 → git log；僅存 deferred 驗測）

四項修復已 ship（b83c9807 + d56d2e8a / v1.5.246），L1/L2(R.3)/L3 驗測 2026-06-16 全過。**唯一 deferred（低）：** 雙使用者並發 VM 驗測 — 雙 user account + per-user service（不同 port）+ Xdummy per user + 兩 client 同連確認 display/audio/input 不串擾。

### §N.5 Android 檔案傳輸驗測

- Send 小檔 → 看 `[VIPLE-XFER]` server log 確認 chunk 正確收到
- Receive listing UI 路徑是否需要調整

---

### 已排除（驗證後不再重查）

- **串流底部撕裂（最下一排像素重複好幾排、間歇）** — 2026-06-18 雙來源讀碼確認 **HEAD（6f2411f9）無此機制**。FRUC 整條（NV12→RGB buffer／warp `frameHeight`／interp 輸出 buffer／libplacebo override 貼圖／present crop／原生 `vkfruc_interp.frag`）高度全鎖同一個 `frame->height`（`vkfruc.cpp:6972-73 / 11394 / 8556`，`plvk.cpp:5421 / 6300-02 / 6331`）；warp `clamp(y,0,frameHeight-1)`（`plvk.cpp:2522-39`）+ interp.frag 正規化 UV 取樣 + `clamp(y,0,srcH-1)`→ 顯示高度永不超過 buffer 高度，無從複製。y-clamp 自 **v1.3.113** 就在；FRUC renderer 無任何 commit 在修此 artifact（「23X 後變少」非 code fix，推測係 §FRUC-XPLAT 後 block-match 不再是主力引擎）。唯一理論殘餘（某解碼器把 `frame->height` 報成 coded 1088）會是**每幀穩定**、非間歇，與症狀不符故排除。實修代價：無（不需改）。

## 鐵律（任何改動都不能破壞）

1. **Fallback 機制保留** — renderer cascade 失敗不能讓使用者 crash
2. **D3D11 renderer 是穩定主線** — Vulkan 仍標實驗性，J.5 切換前不改預設
3. **每個 Phase 都要有 baseline 對比** — 改動前量好基準
4. **build script 不能無聲帶舊 binary** — staging step 必須做 errorlevel-check
5. **版號唯一來源** — `version.json` + `build-tools/version.ps1`，不手改子專案
