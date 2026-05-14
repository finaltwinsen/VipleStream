# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

完成項紀錄在 git log；這份只列尚待 / 進行中 / won't-fix。

---

## 優先級總覽

只列**待辦 / 進行中 / 等硬體 / 等驅動**的條目。已完成、negative result、won't-fix 移到下方對應章節 OR git log。

**戰略：** §B / §B-NVOF / §B2 自家 ME→warp→blend pipeline ceiling = ~20-30% warp ratio。NvOFFRUC.dll 47.7% 是架構性差距無法 tweak 追平。**ML-based interpolation (§J.3.e.X Native RIFE Path β)** 是 ceiling 60-80% 的唯一路 — **v1.4.0 已 ship**。current status: §B/§A' = Linux/AMD/Intel fallback；Path β = NV Vulkan flagship quality (beta opt-in)。

| 優先級 | 條目 | 一句話 |
|---|---|---|
| **Active (next)** | **§J.3.e.X Path β.11** FRUC interp quality 微調 | β.11 hypothesis 1 baseline (EDGE_AWARE_MV_THRESHOLD 2.0→8.0 動態邊緣 8×8 馬賽克 fix) ship in v1.4.11 (fa46a88)，待使用者實測決定：keep / 試 16 (仍馬賽克) / 退 4 (edge 變糊) / 改 push constant runtime tunable。其他 hypothesis (luma-gap backstop / big-motion bias / sub-pixel precision) 若需要再開 |
| ~~**Active (P0, post-v1.4.33)**~~ ✅ **FIXED v1.4.38 + v1.4.40 Android leg** | **§M.1.f** stale ownership lock — 快捷鍵退出後 server-side 釋放 | 2026-05-14 FIXED — root cause 是 client-side quit key combo 路徑不送 /cancel (`session.cpp:1395-1397` `quitAppAfter` pref 預設 false 把 `/cancel` send 跟 client app exit 兩件事綁同條件). Fix: (a) **moonlight-qt v1.4.38** `session.cpp` 拆 `shouldQuit` 成 `shouldNotifyServerCancel` (graceful exit always send /cancel) + `shouldQuitClientApp` (`quitAppAfter` pref). (b) **moonlight-android v1.4.40** `Game.java stopConnection()` 加同 pattern：`conn.stop()` 之前先用 `xferHost / xferHttpsPort / xferServerCert` 建臨時 NvHTTP 呼叫 `quitApp()`。(c) **server defensive `clear_owner_uuid()` public method 保留** (供未來 timeout 機制用)，但 **rtsp.cpp `remove()`/`clear()` session-count==0 hook 在 v1.4.39 reverted**（race condition with normal stream setup — host log 顯示 hook 在 launch 跟 RTSP session insert 之間 fire 把 just-set owner 抹掉）. **§M.1.f.2 follow-up** active: `last_activity` timestamp + idle reconcile 機制（非 hook-on-teardown）補 client crash / WiFi drop / power off 異常路徑 |
| **Active (post-v1.4.12)** | **§M.2** Phase 2 雙 user 並發 streaming 驗測 | Ubuntu VM 上 per-user systemd + Xdummy + PipeWire 端到端跑通；NVENC 並發等實體機到位。等 §M.1 (v1.4.12) 取得使用者實測 sign-off 後開工 |
| **Active (long-running)** | **§J.3.e.2.i.8** Phase 2.5 — FRUC native source 整合 | per-slot buffer 改善大半，殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Active** | **§J.3.e** SW Vulkan path 持續優化 | 1080p120 × 3 codec 全 PASS；4K AV1 SSE2 後 62→76fps；4K H.264/HEVC decoder-bound（CPU 上限） |
| ~~**Active (β.9 part 2 post-v1.4.6)**~~ ❌ **NEGATIVE RESULT v1.4.42** | **§J.3.e.X Path β.9** TRIPLE 60→180 graph-executor split | β.9 Phase 1 (互斥鎖解除 + 2× RIFE inference chain) 已 ship in v1.4.6 (99f86b5)。β.9 Phase 2 (graph executor t-shared front-end + t-dep back-end split) 在 v1.4.42 加入 t-dependency analysis (parseParam 後 BFS mark Layer.dependsOnT + Model.timestepDepLayerStart + frontEndCacheBlobs) 並實測 — **split@48/389** (front-end 12%, back-end 84%)。算盤: chain×2 = 200% baseline → split = 12%×1 + 84%×2 = **180%** (只省 20%)，加上 live-set 78 blobs ≈ 78+ MB GPU cache 開銷 + 400 LOC Vulkan/buffer refactor 風險。**ROI 太低，full split implementation 不做。** Analysis 數據保留在 `parseParam` log line `[VIPLE-RIFE-VK] β.9 Phase 2 t-dep analysis: split@N/M` 供未來模型版本變動時重新評估。要拿到原 plan 的 130% 目標需要 RIFE-v4.25-lite 內部結構翻新（讓 in2 propagation 延後到大部分 flow extraction 之後），那不是執行器可解決的。|
| **Active (β.10 post-ship)** | **§J.3.e.X Path β.10** Linux / AMD / Intel 平台覆蓋 | Path β / NVOF 在 Ubuntu noble compile + link 已通過 (v1.4.0 §K.X 對齊)；待 native runtime smoke + AMD/Intel coopmat fallback 驗測 |
| **Maintenance** | **§B / §B-NVOF / §B2** 自家 ME→warp→blend pipeline | A' 修完 (luma + range + consensus-max) 從 0% 推到 7-23% warp ratio；UI 整合 + Phase 7E 都 ship；**Path B 全套不做**（ceiling ~30% 跟 ML 60-80% 沒法比）；現狀作 Linux/AMD/Intel fallback 已堪用 |
| **HW-pending** | **§I.D** Android Vulkan FRUC async compute | D.2.0–D.2.5 已 ship + Pixel 5 verify；剩自然 45fps→90Hz ideal 1:2 比例 — Pixel 5 panel 鎖 60/90Hz、GameManagerService 鎖 60fps，需 LTPO panel hw 才能驗 |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 3d.6 — AV1 native VK decode grey | 9 個 parser bug 修完仍 grey；NV driver 596.36 + AV1 vkCmdDecodeVideoKHR 黑/灰；§J.3.f ffmpeg 包裝路徑已 cover AV1，這條 deprecated |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 1.7 ONLY-mode NVDEC device-lost | 5 個變體都繞不過，NV 596.36 結構性 bug，預設 PARALLEL 穩 |
| **Deferred** | **§K.4** Wayland XDG portal teardown root-cause | `Restart=always` 緩解已 ship；需可重現 streaming 環境（GPU Linux 機）才能修進 wayland.cpp/portalgrab.cpp 的 EPIPE 路徑 |
| **Medium** | **§F** DirectML 搬 D3D12 / command bundles | 27.6% warp ratio 已驗，但 latency 慢「不堪用」；搬 D3D12 + command bundles 後或可降到 ≤14ms 進入 60fps DUAL budget；Path β 已是 Vulkan 端 ML flagship，DirectML 進一步推進優先級降低 |
| **Medium** | **§J.1** 路線 A (ID3D12 bridge) | NV 596.84 對 D3D11_TEXTURE_BIT 死路；ID3D12Device intermediary 未驗；Path β 走通後優先級降低 |
| **Medium** | **§K.2** Raspberry Pi 5 client (aarch64) | Pi 5 + V3DV mesa Vulkan + V4L2 HW decode + DRM/KMS render；上游 source-level 支援，沒 CI prebuilt；FRUC backend 全 disable（Vulkan 補幀 Pi 5 GPU 不夠力），純 streaming 應 OK |
| **Low** | **§G.1** RIFE v1 11-channel | A1000 launch overhead bound (§G.3 negative result)；RTX 30/40+ 才有意義；Path β shipped 後優先級降低 |
| **Low** | **§J.3.e.Y 4Y.5b** native RIFE activation fp16 | 2026-05-09 嘗試 + revert：256x256 (1080p 配 D-lite UP) 場景 chain +1.7ms 是負收益，跟 4Y.5a postmortem「weight L1-cache-bound 不是 bandwidth bound」結論吻合。重做要先解 D-lite asymmetric center-crop（§β.5.3 D 全套）讓小 dim 真的可選。完整 commit 在 wip/4Y.5b-activation-fp16 branch (82afee5) — 因走錯方向已砍 |
| ~~**Low**~~ ✅ **C.1 + C.2 + C.3 + C.5 ALL SHIPPED** | **§J.3.e.Y 4Y.7** dispatch fusion 全套 | C.1 (Conv→Mul ch-bcast) ship v1.4.1 (`0e6240a`)；C.2 (BinaryOp→Activation) ship v1.4.42；C.3 (Split elision via blob aliasing) ship v1.4.42；**C.5 (Conv→Add→Activation triple fusion) ship v1.4.49 (`fb77551`)** — 40 ResBlock tails 從 Conv→BinOp(Mul)→BinOp(Add)→ReLU 4 dispatch 壓成 1 Conv dispatch。實測 RTX 3060 @ 256×256 stable benchmark (warmup=15 iter=50): C.5 alone -10.7% median (18.10→16.17 ms)，record phase -49% (HALVED, 40 fewer dispatches)，gpu phase -8.9%。**累積 C.1+C.2+C.3+C.5 from session-start baseline**: mean 20.89→16.16 ms (**-22.6%**)，gpu 19.83→15.48 ms (**-21.9%**)，ncnn ratio 3.33×→2.58×。vs-ncnn correctness 全 PASS 不變 (mean=4.72e-05 max=1.35e-02)。剩餘 ROI 候選: Winograd Conv2D (~3-5× on 3×3 Conv, multi-day)、coopmat fp16 precision fix (-70× regression → fixable?)、pack-by-N channel packing。|
| **Low** | **§A.2 / §A.8** WiX installer / 內部 class rename | 沒用 MSI 出貨 / 純內部 |
| **Low** | **§D** HelpLauncher URL → 結構化 docs | docs/setup_guide.md + docs/troubleshooting.md 已寫；HelpLauncher 切過去等 doc site stand 起來 |
| **Active (mouse, v1.4.42+)** / ~~overlay font~~ ✅ **FIXED v1.4.41** | **§N.5.linux** Linux client (Ubuntu VM) ↔ Windows host stream UX bugs | (1) **滑鼠 input forward 不 work** (still active) — keyboard 傳 host OK，mouse event 完全不傳。**Code analysis hypotheses (sorted by likelihood):** **H1 (most likely):** `SDL_SetRelativeMouseMode(SDL_TRUE)` 在 Hyper-V VM console 失敗，silently fall back 到 `m_FakeCaptureActive=true` (input.cpp:383)，但 fake-capture 只藏 cursor 不啟用 xrel/yrel 追蹤 → `LiSendMouseMoveEvent(0,0)` 全送 0 deltas → host 收「mouse 沒動」。**H2:** `SDL_CaptureMouse()` 在 xcb 無 `XCB_GRAB_POINTER` 權限被 deny。**H3:** Hyper-V synthetic input driver 只送 absolute coords，`event->xrel/yrel` 永遠 0。**H4:** SDL window 在 Hyper-V console 沒拿 `SDL_WINDOW_INPUT_FOCUS`，event loop early-return drop。**Fix roadmap:** (a) input.cpp:383 加 diagnostic log 印 `SDL_SetRelativeMouseMode` return + `SDL_GetError()` + fake-capture state；(b) 短期 workaround：Linux 平台預設啟用 absolute mouse mode；(c) 長期：偵測 Hyper-V 自動切 absolute。需 GUI Linux VM 驗測。 (2) ~~**Overlay 字元亂碼**~~ **✅ FIXED v1.4.41** — main.cpp 加 Linux-only `QFontDatabase::addApplicationFont(NotoSansCJK-Regular.ttc)` + build-appimage.sh 從 `/usr/share/fonts/opentype/noto/` bundle CJK font 進 AppDir，runtime 從 `$APPDIR/usr/share/fonts/opentype/` 載入。AppImage builder 需 `apt install fonts-noto-cjk` 才有 source；缺檔時 silent fallback 到 host fontconfig。 |
| **Active (P0, post-v1.4.35)** | **§N.5.bug** Android file transfer client SSL `TLSV1_ALERT_INTERNAL_ERROR` post-handshake | (b) server stale state **✅ FIXED in v1.4.34 commit `550100d`** (sweep_stale_locked, 實測 06:11:22 age=59s pending→failed verified). (a) **仍 active** — v1.4.35 `[VIPLE-VERIFY]` diagnostic 證實 **Scenario B**：每個 client SSL request server-side `verify_callback` 都 `enter` + `OK uuid=XXXXXXXX-…`，**零 DENY**，TLS handshake + cert verify 全成功。但 client BoringSSL `tls_record.cc:572` 從 server 收到 internal_error alert. **意味 alert 在 TLS handshake 之後 application 層送**（SimpleWeb / boost-asio strand 處理 request 階段）。下一步 diagnostic options：(i) 在 xfer_poll/blob/result handler 加 entry/exit/try-catch BOOST_LOG 看 handler 是否 invoke + 是否 throw；(ii) host-side curl reproduction (需要 export Pixel 5 paired cert/key 到 host)；(iii) SimpleWeb internal read/write error verbose patch. 推薦先 (i) — 最 surgical |
| **Active (post-v1.4.33)** | **§N.5** moonlight-android FileTransferClient runtime verify | 建置 + JNI hooked，但實機未測；接收方向 listing 走 MediaStore.Downloads 而非任意 path（v1 限制） |
| **Low (post-v1.4.33)** | **§N.6** moonlight-qt Cancel hotkey (Ctrl+Alt+Shift+X) | Web UI 重新整理 + 等 stream 結束已可中止 transfer；in-session hotkey 還沒接到 session keystroke handler |
| **Low (partial-fixed v1.4.40)** | **§N.7** Sunshine Linux fs_picker (zenity) | v1.4.40 ship `src/platform/linux/fs_picker.cpp` **stub** (回 `std::nullopt`) 讓 Linux server build 能 link（`system_tray.cpp` 對 `fs_picker::pick_open_file` unconditional reference），tray「Send to client」目前 noop。real zenity subprocess 實作仍 TODO — 需 native Linux GUI session 驗測 DISPLAY / DBUS_SESSION_BUS_ADDRESS env 透過 sunshinesvc `CreateProcessAsUserW` 正確繼承|
| **Low (post-v1.4.20+)** | **§H.4-perf** VkFrucRenderer AMD draw-time perf fix | OSD「(含 V-sync; 60Hz 下限 ~16.7ms)」標籤已 ship；m_FrucMode=false 真實 ~13ms (60Hz) 仍偏高，要先抓 `[VIPLE-VKFRUC-GPU-PROF]` 量化才能 dive in。AMD-only，NV 未複現 |

### v1.4.0 ship 帶走的條目（之前在 Active 表內）

- ✅ **§J.3.e.X Path β** Native RIFE Vulkan FRUC backend — β.1/β.2 chain swap + β.4 down/up wrapper + β.5 RIFE flow + native warp + β.6 stability fix（`drainOverlayStash` `vkDeviceWaitIdle` race fix，7m49s 連測無 crash）+ β.8 prefs UI + docs
- ✅ **§J.3.e.X Final.3b** Native RIFE→NcnnFRUC drop-in — replaced by Path β（NV dual-VkDevice block 走不通，改接 VkFrucRenderer 共用 VkDevice 通了）
- ✅ **§J.3.e.Y 4Y.6** Tensor Core Conv2D path — coopmat shader 4 commit 完整實作 + `VIPLE_RIFE_VK_COOPMAT=1` opt-in default-OFF（perf +7% / precision -70× tradeoff documented）
- ✅ **§B-DUMP NCNN 全 0 mat** — Path β 取代 NcnnFRUC 後不再依賴 ncnn forward pass
- ✅ **§K.X auto-wake fix** — Auto Wake-on-LAN toggle 真的 gate `PcMonitorThread` polling
- ✅ **§K.X Linux build alignment** — moonlight-qt Ubuntu noble + Qt 6 + g++ 13 完整 compile + link
- ✅ **§A'-Android port** — luma census + hierarchical diamond + warp 50/50 fallback 對齊 Windows + §B-DUMP-Android cross-platform format + UI i18n × 27 locales + dev-machine serial redact

### post-v1.4.0 patch series 帶走的條目（v1.4.1 → v1.4.9, 2026-05-09）

主軸：**徹底解決「Vulkan + FRUC ON 解碼穩定卡 80-100ms」regression**。

- ✅ **§J.3.e.X β.5.2** Catmull-Rom bicubic 上採 flow + mask（`a8b3204`，v1.4.1）— score 0.95 → 0.97-0.98，warp shader chain +0.4ms 仍進 budget
- ✅ **§J.3.e.X β.5.3 D-lite** inferDim 從 DOWN-rounding 改 UP-rounding 到 /128 multiple（`9d4c237`，v1.4.1）— 解掉 RIFE-v4-lite Add_503 layer 在 inferDim 不是 /128 multiple 時 a.h round 不對齊撞 shape constraint 的問題；副作用是 1080p source 配 cfg 256 變 256x256 而不是 256x128（pixels 翻倍）
- ✅ **§J.3.e.Y 4Y.7 C.1** Conv→BinaryOp(Mul, channel-bcast beta) fusion（`0e6240a`，v1.4.1）— 40 個 fuseable pair 由 Conv shader epilogue 吃掉，chain -2.6ms（13.8 → 11.2ms @ 256x128 baseline）
- ✅ **§J.3.e.2.i.10** Phase 2A async-compute queue infrastructure（`24f6634`，v1.4.3）— probe + alloc dedicated compute QF (RTX 3060 QF=2) + cmd pool + timeline sem。pure plumbing，沒 functional change，留作 Phase 2B+ 把 RIFE compute 拆到 compute queue 用
- ✅ **§J.3.e.2.i.10b** extra_hw_frames=1 for all codecs (HEVC included)（`f1692c1`，v1.4.4）— 防 NV driver 將來改回 dedicated_dpb HEVC 模式時的 latency regression（保險，2026-05-09 live test 證明對 user 的 100ms 沒直接幫助但 documentation-correct）
- ✅ **§J.3.e.2.i.10c** Phase β.9 — TRIPLE 60→180 + Native RIFE coexist（`99f86b5`，v1.4.6）— 拿掉 ctor 的 `m_RifeNativeMode + m_TripleMode` 互斥鎖；β.5.1 chain 重構成 `runOneInferAndWarp(t, outBuf, ds)` helper，TRIPLE 跑 2 次 RIFE inference（t=1/3 + t=2/3）+ 2 個 warp DS 各寫 m_FrucInterpRgbBuf / m_FrucInterpRgbBuf2。β.9 Phase 2 (graph executor timestep-shared frontend split) 仍 active
- ✅ **§J.3.e.2.i.10e** ROOT-CAUSE FIX — kFrucFramesInFlight 4→2（`6b9bb89`，v1.4.8）— v1.4.2 那個 2→4 bump 是錯的方向。Vulkan single-graphics-queue cmd buffer FIFO 序列執行，slot 多只展開 CPU pipelining、不增 GPU throughput。但每 slot 跑完整 chain (~20ms) 才 signal `vkf->sem[0]@V+1` 給 FFmpeg pool 釋放 image，所以 **AVFrame hold cycle = N × chain_time**。N=4 → 80ms hold cycle = 直接對應 user 看到的「decodeMeanMs 穩定 80-100ms」。改回 N=2 後 5-90ms bimodal、好的時段 5-15ms
- ✅ **§J.3.e.2.i.10f Path D** early AVFrame release via two-submit pattern（`9fd8c22`，v1.4.9）— **最終解**。把 renderFrame 拆成兩段 graphics-queue submit。Submit 1 (m_SlotCopyCmdBuf, ~100us) 只做 image→buffer copy + sem signal `vkf->sem[0]@V+1`；submit 2 (m_SlotCmdBuf, ~20ms) 跑 chain + present，不碰 vkf。AVFrame hold cycle 從 40ms 變 ~0.2ms。**live test：decodeMeanMs 0.47-30ms typical，sub-ms 多次出現，跟 v339 / D3D11 / FRUC OFF 同等級**。能做到的關鍵是 §B-quality (d) 2026-05-06 已先把 DUAL+FRUC real-frame display 切換成 m_FrucCurrRgbBuf via m_RealCurrRgbDescSet（不再吃 vkf->img[0] ycbcr sampler），所以 vkf->img[0] 只在 chain 開頭 ~100us 用一次
- ✅ **§J.2 H.3** vkfrucNativeRifeInferDim default 256 → 128 + UI honesty（`cf0df28`，v1.4.10）— mid-range GPU (RTX 3060 Laptop) 在 inferDim=256 chain ~20ms > 16.7ms (60fps slot)，dual-present 從 target 120 掉到 45.34 + p99 28ms judder；inferDim=128 chain ~10ms fit budget, fps 76.69，但 down ratio 15× 讓 flow precision 接近 ME 8×8 block (quality ≈ ME)。default 改 128，256 留給 RTX 4070+ opt-in。SettingsView.qml ComboBox text + RIFE checkbox ToolTip 一起對齊實測值。後續可考慮 async-compute (m_ComputeQueue infra v1.4.3 ready) 把 RIFE inference 搬出 graphics queue 讓 dim=256 fit budget；或 IFRNet-S 5.5MB 試 quality+perf sweet spot
- ✅ **§J.3.e.X β.11** TRIPLE+ME EDGE_AWARE_MV_THRESHOLD 2.0 → 8.0（`fa46a88`，v1.4.11）— TRIPLE+ME 動態邊緣 8×8 馬賽克 fix。kFrucWarpShaderGlsl::sampleMV 內 edge-aware MV smoothing threshold 從 2.0 (1.4 px L2 diff) → 8.0 (2.83 px) 過濾 noise 維持 bilinear smooth；大 motion boundary (>2.83 px diff) 仍切 nearest pick 保 edge 銳利。Hypothesis 1 baseline，仍 active 等使用者實測（見 Active 表 §J.3.e.X Path β.11）
- ✅ **§M.1** Multi-user ownership guards + Web UI session dashboard（`79c47f9`，v1.4.12）— 修「多人共用 server 時 A 客戶端意外殺到 B 的 Steam 遊戲」bug (詳 §M section)

### post-v1.4.12 patch series 帶走的條目（v1.4.13 → v1.4.33, 2026-05-13）

主軸：**§N in-stream 雙向檔案傳輸功能（Send to client / Receive from client）+ §H.4 AMD client 解析度退回 + OSD V-sync 標籤**。

- ✅ **§H.4.send** AMD client 1920×1200 → 1920×1080 silent downgrade — Sunshine `/serverinfo` 加 `<DisplayMode>` 廣播 host primary display；moonlight-qt session.cpp 啟動 stream 前依 `m_Computer->displayModes` 自動 clamp；SettingsView 解析度 dropdown 透過新 `ComputerManager.getMaxHostDisplayMode()` 過濾掉 host 物理無法提供的解析度。NV server / 沒 `<DisplayMode>` 廣播自動 skip 不影響 vanilla 互通
- ✅ **§H.4.osd** Frame draw time OSD 「(含 V-sync; 60Hz 下限 ~16.7ms)」標籤強化 — 解使用者「30ms 看起來很高」的困惑；60Hz 一張 frame 的 v-sync 期就 16.7ms，扣掉之後 AMD 真實 GPU work ~13ms（待 §H.4-perf 處理）
- ✅ **§T.tray-i18n** Tray menu「Open Sunshine」→「Open VipleStream」rebrand（system_tray.cpp）
- ✅ **§N.1** In-stream 檔案傳輸 server 端（Sunshine v1.4.14 → v1.4.32）
  - 新 `src/file_transfer.{h,cpp}` manager — per-paired-client command queue + token registry + chunked I/O + path sanitize + active-user Downloads dir resolver (WTSGetActiveConsoleSessionId + WTSQueryUserToken)
  - 5 個 HTTPS endpoints in `nvhttp.cpp`：`/transfer/poll`（non-blocking poll）/`/transfer/result`/`/transfer/blob`（GET 拉檔 + POST 推檔）/`/transfer/progress`
  - 4 個 confighttp HTTPS endpoints：`/transfer`（HTML page）/`/transfer/listing/latest`（含 `?path=` 換目錄）/`/transfer/pull`（含 `path` + `is_directory` params）/`/transfer/progress-proxy`
  - `src/system_tray.cpp` 加「Send file to client」/「Receive file from client」menu items + `update_tray_playing/stopped` 同步 disabled 狀態 + tray balloon UX 提示
  - Windows native picker `src/platform/windows/fs_picker.cpp`（IFileOpenDialog COM wrapper）+ link `wtsapi32.lib`
  - Receive UI 升級成檔案總管樣式：path input + 麵包屑 + Up/Home buttons + 資料夾可點進去 + folder 顯示「Pull as zip」+ 自動以 client 端 zip 後上傳（Windows PowerShell `Compress-Archive` / Linux `zip`）
- ✅ **§N.2** In-stream 檔案傳輸 moonlight-qt client（v1.4.15 → v1.4.33）
  - 新 `streaming/transfer/filetransferclient.{h,cpp}` — 2s 輪詢 + LIST_DIR / DOWNLOAD_TO_CLIENT / UPLOAD_FROM_CLIENT / CANCEL dispatcher
  - **獨立 QThread + Qt event loop** — Session::exec() 跑 SDL main loop 整段 starve 主執行緒 Qt event loop，QTimer / QNAM 必須住自己的 thread 才會運作
  - Stream-end teardown：先 clear QPointer 再 `abort()` 避免 same-thread synchronous `finished` signal callback 把 reply 提早 delete 造成 null deref crash
  - SSL 端：onSslErrors connected，pinned `m_Computer->serverCert` 比對通過就 `ignoreSslErrors`（同 NvHTTP::handleSslErrors）；每次大檔 transfer 結束 `m_Nam->clearAccessCache()` 強制刷新連線池避免半關閉 socket 重用
  - Folder zip 上傳：PowerShell `Compress-Archive` / Linux `zip` 跑 subprocess 把資料夾打包到 temp，上傳完自動清掉
  - 進度 OSD：`OverlayManager::updateOverlayText(OverlayStatusUpdate, ...)` 每 5% 步進
- ✅ **§N.3** In-stream 檔案傳輸 moonlight-android client（v1.4.19）
  - 新 `transfer/FileTransferClient.java` — OkHttp polling + `MediaStore.Downloads` (scoped-storage) 讀寫 + streaming RequestBody
  - `Game.java` lifecycle 掛載 + `displayTransientMessage` toast 進度
  - `NvHTTP.java` 加 `getLongConnectClient()` getter
- ✅ **§N.4** Server poll endpoint non-blocking bug — manager::poll(timeout=0) 被誤判為「`<=0` 用 30s default」而 block 30s，client 15s watchdog 永遠贏，polling 全 fail。改回真正 non-blocking 才解開整套
- ✅ **§N.4b** Server downloads dir 寫到 SYSTEM profile bug — 服務 LocalSystem 帳號跑 `SHGetKnownFolderPath(nullptr)` 回 `C:\Windows\system32\config\systemprofile\Downloads`，使用者看不到。改用 active console user token 拿正確的 `C:\Users\<user>\Downloads`

**Negative result (走錯後 revert)：**

- ❌ **§J.3.e.Y 4Y.5b** activation fp16 storage — wip/4Y.5b-activation-fp16 branch 留檔 (82afee5)，main 已砍。256x256 場景 chain 反而 +1.7ms（4Y.5a postmortem 的 L1-cache-bound 結論在這裡套上 = bandwidth 不是 bottleneck）
- ❌ **§J.3.e.2.i.9** kFrucFramesInFlight 2→4 bump — v1.4.2 (234d2aa) 走錯方向。在 v1.4.8 (6b9bb89) revert
- ❌ **§J.3.e.2.i.10d** extra_hw_frames=8 (Round 4) — pool size 不是 dominant bottleneck，Round 5 確認後 revert 回 1
- ❌ **§N.client.connection-cache** moonlight-qt FileTransferClient `ConnectionCacheExpiryTimeoutSecondsAttribute=0` — 仿照 NvHTTP 設這個 attribute（NvHTTP 是 GFE-compat 用），在 Sunshine + Qt 6.10 環境下反而讓第一次 poll 直接 hang 15s 被 watchdog 砍掉。已 revert，改在每次 transfer 結束後 `clearAccessCache()` 處理半關閉 socket 重用問題
- ❌ **§J.3.e.X Path β.9 Phase 2** graph executor split — v1.4.42 加 t-dependency analysis (Layer.dependsOnT + Model.timestepDepLayerStart + frontEndCacheBlobs) 實測 RIFE-v4.25-lite 模型 **split@48/389** (front-end 12%, back-end 84%); chain×2 = 200% → split = 12%×1 + 84%×2 = **180%** (只省 20%)，跟原 plan 130% 目標差很多，加上 live-set 78 blobs ≈ 78+ MB cache 開銷 + 400 LOC refactor 風險 = **ROI 太低不做**。Analysis logging 保留供未來模型版本評估。

### v1.4.42 ship 帶走的條目（2026-05-14, same-day v1.4.41 follow-up）

主軸：**§N.5.bug Scenario A diagnostic instrumentation + §J.3.e.X Path β.9
Phase 2 t-dep analysis (logging only)**.

- ✅ **§N.5.bug Scenario A diagnostic** — `Sunshine/src/nvhttp.cpp` `xfer_result` /
  `xfer_blob_post` / `xfer_progress` 3 handlers 加 try-catch ENTER/EXIT/EXCEPTION
  log wrappers (匹配既有 `xfer_poll` / `xfer_blob_get` pattern)。`xfer_blob_post`
  chunk 迴圈加 per-MB progress log，下次 Android Pixel client 端 SSL
  `TLSV1_ALERT_INTERNAL_ERROR` 觸發時 server log 可看到 alert 是 request 起
  前 / chunk loop 中 (第幾 MB) / finalize 後哪一段送出。不需要 user 操作驗證
  (compile pass = primary verify)。
- ✅ **§J.3.e.X Path β.9 Phase 2 t-dep analysis logging** — `rife_native_vk.{h,cpp}`
  加 `Layer.dependsOnT` + `Model.timestepDepLayerStart` + `Model.frontEndCacheBlobs`
  + BFS-based analyzeTimestepDependency() at end of parseParam。實測 RIFE-v4.25-lite
  split@48/389 + live-set 78 blobs，**負面結果**(見 negative-result section 上方)，
  full split implementation 不做。
- ✅ **§J.3.e.Y 4Y.7 C.2** BinaryOp→Activation fusion (`rife_native_vk.{h,cpp}`) —
  40 BinOp→ReLU pairs (RIFE-v4-lite ResBlock residual-sum 全部接 LeakyReLU 0.2)
  folded into BinaryOp shader epilogue。Layer 加 `fuseActKind` + `fuseActSlope`
  fields，BinaryOp shader PC 加 `fuse_act_kind` + `fuse_act_slope` (replace _pad[2])
  + GLSL `apply_act(x)` epilogue 套 ReLU/LeakyReLU/Sigmoid，分發時透過 既有
  `fuseMulOverrideOutput` mechanism 寫入 Activation 的 output blob，Activation 本身
  isFusedAway=true 跳過。`analyzeFuseableBinOpAct(model)` 在 `initialize()` C.1
  之後跑。實測 RTX 3060 Laptop @ 256×256：median per-fwd 19.90→17.50 ms (**−12.1%**)，
  mean 20.89→19.15 ms (−8.3%)，record phase −32%，gpu phase −7.5%；vs-ncnn
  correctness gate 全 PASS 不變。
- ✅ **§J.3.e.Y 4Y.7 C.3** Split elision via blob aliasing
  (`rife_native_vk.cpp`) — 56 Split layers (152 copy operations + barriers)
  elided。ExecState 加 `std::unordered_map<QString,QString> blobAlias`，
  `findBlob` 以 8-hop alias chain 解析。`analyzeFuseableSplits(model, alias)`
  在 `initialize()` 的 buildExecState 後跑：每個 Split 通過三條安全檢查 (input
  唯一 consumer = this Split / 各 output 唯一 writer = this Split / src blob 之後不
  被覆寫) 才標 isFusedAway=true + 把 outputs 全部 alias 回 src 。  RIFE-v4-lite
  全 56 Splits 都符合條件。實測 RTX 3060 Laptop @ 256×256：mean per-fwd
  19.15→18.39 ms (-0.76 ms / -4.0%)，gpu phase 18.35→17.50 ms (-0.85 ms / -4.6%)，
  min 17.29→15.98 ms (best-case -7.6%)；vs-ncnn correctness gate 全 PASS 不變。
  **C.1+C.2+C.3 累積**: mean 20.89→18.39 ms (**-12.0%**)，gpu phase 19.83→17.50 ms
  (**-11.8%**)。

### v1.4.58 ship 帶走的條目（2026-05-14, post v1.4.57 cmpCmd → compute queue）

- 🟢 **§J.3.e.2.i.10 Phase 2B 預設 ON** — flip `VIPLE_RIFE_VK_ASYNC_COMPUTE`
  env gate default。改前 opt-in（env=1 啟用），改後 opt-out（unset 或
  非 "0" 字串 = ON；env=0 才強制走 v1.4.55 single-cmd 路徑作為 bisect /
  driver 退路）。
  - 改動範圍：`vkfruc.cpp` ~5 LOC + log 字串
  - phase2BActive gate 其他條件不變（仍需 DUAL + !TRIPLE + Path D +
    β.5.1 + 所有 cmd buf/sem alloc 成功）；任何條件 false 仍走 v1.4.55
    single-cmd path，所以 flip default 不會在 unsupported device 上
    crash —— `m_AsyncComputeAvailable` 在 single-QF GPU / alloc 失敗時
    自動 demote
  - 跳過原 plan 的「real-stream 一週驗測」門檻 —— 使用者直接 flip 並
    本機部署驗
  - **TRIPLE / SW path renderFrameSw mirror / β.4 fallback 拆分** 仍留
    v1.4.59+ 各自獨立 commit，看真實 stream 出問題的時候再說

### v1.4.57 ship 帶走的條目（2026-05-14, post v1.4.56 cmd-buf split）

- 🟡 **§J.3.e.2.i.10 Phase 2B step 5-6（拆分版第二刀）** cmpCmd 真正切
  到 `m_ComputeQueue` 拿 `s_VkFrucComputeLock`。改動範圍極小
  (`vkfruc.cpp` ~10 LOC + log 字串)，但這是 Phase 2B 的核心 perf
  gain 進入點 —— RIFE inference 真正在 compute QF 平行於 graphics
  submit 跑。
  - `vkQueueSubmit(m_GraphicsQueue, ...)` → `vkQueueSubmit(m_ComputeQueue, ...)`
  - `std::lock_guard<std::mutex> lk(s_VkFrucQueueLock)` →
    `s_VkFrucComputeLock`（v1.4.56 加但 dormant；此版啟用）
  - init log 從 "gfx-cmp(v1.4.56)" 改成 "CMP(QF=%u)" 反映實際 QF
  - **跨 queue sync 鐵三角已 ready**：m_ComputeTimelineSem (Phase 2A)
    + CONCURRENT sharing on m_RifeDownPrev/Curr/Interp + m_RifeFlow/MaskOutBuf
    (v1.4.55) + VulkanCtx::computeQueue/Family 條件 binding (v1.4.55)
  - **TRIPLE / SW path / β.4 fallback / 預設 ON 留 v1.4.58+** 各自獨立
    commit，先讓 NV driver cross-queue timeline sem 穩定性驗測一週
  - env=0（預設）跟 v1.4.55 bit-identical（phase2BActive 短路為 false）

### v1.4.56 ship 帶走的條目（2026-05-14, post v1.4.55 Phase 2B plumbing）

- 🟡 **§J.3.e.2.i.10 Phase 2B step 5-6（拆分版第一刀）** cmd-buf split
  + 4-submit chain，但 cmpCmd 仍在 graphics queue 上（拿到 v1.4.57 才真正
  切 compute queue）。改動範圍：`moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.{h,cpp}`。
  - vkfruc.h 加 `recordRifeDown` / `recordRifeInferOnCompute` /
    `recordRifeWarp` 三個 helper 宣告（DUAL β.5.1 only）
  - vkfruc.cpp 加 `s_VkFrucComputeLock` static mutex（v1.4.57 才真正
    用，本版只為了 commit 完整性引入；目前 cmpCmd submit 仍拿
    s_VkFrucQueueLock）
  - 三個 helper 實作：把既有 `runRifeNativeStage` β.5.1 DUAL path 切
    DOWN（preCmd）/ inference（cmpCmd）/ warp（postCmd）三段。Cross-cmd-buf
    barrier 由 timeline-sem chain 提供 execution dep，每段末端各自 emit
    access-mask transition
  - renderFrame 加 `phase2BActive` gate，**所有條件必須同時成立**：
    `m_AsyncComputeRequested && m_AsyncComputeAvailable && pathDActive
    && dualPresentThisFrame && !triplePresentThisFrame && m_Beta5Enabled
    && m_RifeNativeReady && warp pipe/ds + pre/post/compute cmdbufs + 
    m_ComputeTimelineSem 全 alloc 完成`。任何條件失敗 → fallback 到 v1.4.55
    single-cmd path（bit-identical）
  - phase2BActive 路徑下 record 3 cmd buf + submit 4 chain：
    `Path D copy (既有) → preCmd (signal compute@V_pre) → cmpCmd
    (wait V_pre, signal V_post, **graphics queue in v1.4.56**) → postCmd
    (wait V_post + acquireSem[A,B], signal renderDone[A,B] + fence)`
  - phase2BActive 路徑 mirror 既有 NV-OF marker submit + execute（不 mirror
    會讓 m_NvOfInputCurr/Prev swap 失衡）
  - **暫不 support 的範圍 (留 v1.4.57+)**: TRIPLE 兩次 inference cmpCmd
    編排、SW path `renderFrameSw` mirror、β.4 bilinear-up fallback、真正
    cmpCmd 切 m_ComputeQueue 拿 s_VkFrucComputeLock。第一版 phase2BActive
    全 graphics queue 等價 v1.4.55 single-cmd + 額外 cmd-buf 邊界 cache
    flush — 只驗 split 結構正確，**不期待 perf gain**
  - env=0（預設）跟 v1.4.55 bit-identical（phase2BActive 短路為 false）

### v1.4.55 ship 帶走的條目（2026-05-14, post v1.4.54 Phase 2B step 1）

- 🟡 **§J.3.e.2.i.10 Phase 2B step 2-4** async-compute plumbing
  (`moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.{h,cpp}` +
  `rife_native_vk.h`)。預備 step 5-6 (3-submit chain) 落地前的所有基礎建設：
  - `m_SlotPreCmdBuf[N]` + `m_SlotPostCmdBuf[N]` per-slot graphics
    cmd buffer 陣列 alloc 在 `createInFlightRing`、destroy 對稱
    cleanup；只在 `m_AsyncComputeAvailable` 時 alloc，alloc 失敗會
    demote 回 `m_AsyncComputeAvailable=false` 走 single-submit fallback
  - `m_AsyncComputeNext` atomic uint64 counter for timeline-sem chain
  - `VulkanCtx::concurrentSharingQfs[2]` + `concurrentSharingQfCount`
    新增於 `rife_native_vk.h`，表達 caller 跨多 QF 意圖（目前 RIFE 內
    部 host-coherent blob 不需 CONCURRENT — spec 對 host-coherent 寬
    鬆 + 從不 cross queue，所以 `rife_native_vk.cpp` 端不動）
  - `vkfruc.cpp` createRifeNativeResources::allocBuf 跟
    createFrucComputeResources::allocBuf 都加 sharingAcrossQfs gate：
    當 `m_AsyncComputeAvailable && graphics QF != compute QF` 時，
    cross-queue 對象 buffer 改 `VK_SHARING_MODE_CONCURRENT` 跨兩個 QF
    (m_RifeDownPrev/Curr/Interp, m_RifeFlowOutBuf, m_RifeMaskOutBuf,
    m_FrucInterpRgbBuf, m_FrucInterpRgbBuf2)，graphics-only 對象
    (m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucMvBuf*, m_SwFrucNv12Buf)
    保持 EXCLUSIVE
  - createRifeNativeResources VulkanCtx 條件 binding：
    `asyncCtxActive` 為 true 時 `ctx.computeQueue/Family` 改塞
    `m_ComputeQueue/m_ComputeQueueFamily` 與 concurrentSharingQfs；否
    則沿用 graphics queue (bit-identical fallback)
  - env=0 (預設) 完全 bit-identical 跟 v1.4.54 — 所有改動 wrap 在
    `m_AsyncComputeAvailable` 或 `asyncCtxActive` 條件下
  - **真正的 3-submit chain wiring (Phase 2B step 5-6) 拆到 v1.4.56**:
    runRifeNativeStage 拆三段 (recordRifeDown / recordRifeInferOnCompute /
    recordRifeWarp)、renderFrame 加 pre→cmp→post submit chain、
    s_VkFrucComputeLock、TRIPLE 兩次 inference cmpCmd 編排、SW path
    mirror — 這些需要重寫 vkfruc.cpp `renderFrame` 1000+ 行 record +
    submit 邏輯，單 commit 風險過高，分階段落地

### v1.4.49 ship 帶走的條目（2026-05-14 evening, post v1.4.42-batch）

- ✅ **§J.3.e.Y 4Y.7 C.5** Conv → BinOp(Add residual) → Activation triple
  fusion (`rife_native_vk.{h,cpp}`, commit `fb77551`)。把 RIFE-v4-lite 40 個
  ResBlock tail (Conv → BinOp(Mul, ch-bcast beta) → BinOp(Add, residual) →
  ReLU/LeakyReLU 0.2) 從 4-dispatch chain 壓成 1 Conv dispatch。Conv shader
  3 個 variant (generic / 3x3-s1 tiled / 3x3-s2 tiled) 全 update: bindings
  5→6 (加 residual_buf binding=5), PC 64→80 bytes (加 hasFusedAdd /
  fuseConvActKind / fuseConvActSlope 3 fields), epilogue 加 conditional
  residual add + outer activation。dispatchConvolution + standalone
  runConv2DGpuTest 全 sync。`analyzeFuseableConvAddAct(model)` 在 C.1 後
  C.2 前跑 (initialize() ordering); 3-layer pattern match + 6 safety
  conditions (Conv has C.1 fusion / Conv-output single-consumer is BinOp(Add)
  with op=0 / BinOp(Add)-output single-consumer is Activation / residual
  blob written before Conv / etc)。**stable benchmark warmup=15 iter=50**：
  mean per-fwd 18.10→16.16 ms (**-10.7%**)，median 18.10→16.17 ms (-10.7%)，
  record phase 0.87→**0.44 ms** (HALVED, 40 fewer dispatches)，gpu phase
  17.00→15.48 ms (-8.9%)。**累積 C.1+C.2+C.3+C.5 from session-start
  baseline (pre-C.2)**: mean 20.89→16.16 ms (**-22.6%**)，gpu 19.83→15.48 ms
  (**-21.9%**)，ncnn-vulkan ratio 3.33×→2.58×。vs-ncnn correctness 全 PASS
  不變 (mean=4.72e-05 max=1.35e-02 frac>1e-2=0%); standalone Conv2D /
  BinOp / Activation tests 全 PASS。

### v1.4.41 ship 帶走的條目（2026-05-14, same day as v1.4.40 follow-up）

主軸：**§M.1.f.2 idle reconcile (abnormal disconnect coverage) + §N.7 real
zenity fs_picker + §N.5.linux overlay CJK font bundle**.

- ✅ **§M.1.f.2** Idle reconcile watchdog (`Sunshine/src/process.{h,cpp}` +
  `nvhttp.cpp`) — `_last_activity` + `static std::mutex _owner_mutex` +
  detached watchdog thread (pattern mirrors `_steam_watchdog_gen`).
  auto-release after 60s no-RTSP + no-HTTP activity from owner.
  Static mutex needed because `proc_t` must remain move-constructible
  (`parse()` returns `std::optional<proc_t>` assigned to singleton via
  `std::move`); same constraint that makes `_steam_watchdog_gen`
  static.
- ✅ **§N.7** Linux server fs_picker — replaced v1.4.40 stub with real
  `zenity --file-selection` subprocess via boost::process::v1.
  per-user systemd service inherits `DISPLAY` / `DBUS_SESSION_BUS_ADDRESS`
  so no extra session-handoff plumbing.
- 🟡 **§N.5.linux overlay CJK** (partial) — `moonlight-qt/app/main.cpp`
  Linux-only `QFontDatabase::addApplicationFont(NotoSansCJK-Regular.ttc)`
  (after `QGuiApplication` ctor per v1.2.27 ordering rule); candidate
  path list searches AppImage `$APPDIR/usr/share/fonts/opentype/` first,
  then system paths.  `scripts/build-appimage.sh` has cp logic to bundle
  font from builder's `/usr/share/fonts/opentype/noto/` but v1.4.41
  builder lacked `fonts-noto-cjk` package, so this release's AppImage
  falls back to user-system fontconfig.  Code path verified; bundling
  re-test after `apt install fonts-noto-cjk` on builder = v1.4.42+
  follow-up.

**Out of scope, deferred to v1.4.42+:**

- ❌ **§N.5.linux mouse forward** — visual verification needs GUI Linux
  VM access (Hyper-V Manager console).  Read-only code analysis report
  + 4-hypothesis ranking + fix roadmap recorded in §N.5.linux active
  entry above for future iteration.

### v1.4.34 → v1.4.40 ship 帶走的條目（2026-05-14）

主軸：**§M.1.f 快捷鍵退出後 server-side ownership 釋放（PC + Android）+ §N.5.bug
server stale-state sweep + Linux server build linker fix**。

- ✅ **§N.5.bug (b) server-side stale lock sweep** — `sweep_stale_locked` 60s
  age 條件 pending→failed transition（commit `550100d`，v1.4.34）。實測
  06:11:22 age=59s pending→failed verified
- ✅ **§M.1.f client-side fix** — moonlight-qt `session.cpp` 拆 `shouldQuit`
  為 `shouldNotifyServerCancel` + `shouldQuitClientApp`（commit `cd8f56a`，
  v1.4.38）+ server-side `proc::proc.clear_owner_uuid()` public method
  for 未來 idle reconcile 機制
- ✅ **§N Android UX polish** — Toast → in-stream overlay (bottom-left
  yellow-green text, 0-100% 全程可見, 完成 3s 後自動 hide)（v1.4.38）
- ✅ **§M.1.f race fix revert** — rtsp.cpp `remove()` / `clear()`
  session-count==0 → `clear_owner_uuid()` hook 因 race condition with
  normal stream setup（host log 14:02:50 hook 在 14:02:47 launch 跟
  14:02:50.197 RTSP session insert 之間 fire 把 just-set owner 抹掉）
  → reverted（commit `8a70d6e`，v1.4.39）。`clear_owner_uuid()` method
  保留供 §M.1.f.2 follow-up
- ✅ **§M.1.f Android leg** — moonlight-android `Game.java
  stopConnection()` 在 `conn.stop()` 之前先用 `xferHost / xferHttpsPort /
  xferServerCert` 建臨時 NvHTTP 呼叫 `quitApp()`，best-effort try-catch
  包住（commit `de2e4cb`，v1.4.40）
- ✅ **§N.7 Linux server fs_picker stub** — `Sunshine/src/platform/linux/fs_picker.cpp`
  回 `std::nullopt` 讓 Linux server build 能 link，`cmake/compile_definitions/linux.cmake`
  加進 `PLATFORM_TARGET_FILES`（commit `de2e4cb`，v1.4.40）。real zenity 仍 TODO

---

## §A. 品牌遷移相容性債

### §A.2 WiX installer 的 registry / install paths

**目前：** WiX installer 的 `HKCU\Software\Moonlight Game Streaming Project` 安裝狀態旗標、`InstallFolder = "Moonlight Game Streaming"`、`APPDATAFOLDER = %LOCALAPPDATA%\Moonlight Game Streaming Project` 全沒動。

**影響檔案：** `moonlight-qt/wix/Moonlight/Product.wxs`、`moonlight-qt/wix/MoonlightSetup/Bundle.wxs`。

**注意：** 我們**目前根本沒在用 WiX MSI installer 出貨**，client 是 zip + 手動解開、server 用 `deploy_sunshine.ps1` 部署。所以 WiX 的相容性債是「如果哪天要改用 MSI 出貨才需要清」，不是現在的痛點。

**清的時候要做：**
1. 新 InstallFolder = `VipleStream`、新 APPDATAFOLDER = `%LOCALAPPDATA%\VipleStream`
2. 新 HKCU 路徑 = `HKCU\Software\VipleStream`
3. WiX 升級邏輯：偵測舊 key / 路徑存在 → copy → 砍舊
4. 配新的 `UpgradeCode` GUID，否則 MSI 嘗試 in-place upgrade 找不到舊路徑

### §A.7 協定 / wire-format 字串（**不該清**）

**目前：** `"NVIDIA GameStream"`、cert CN `"Sunshine Gamestream Host"`、UA header `Moonlight/...`、mDNS service type `_nvstream._tcp`、serverinfo 的 `state` 字串 `SUNSHINE_SERVER_FREE` / `SUNSHINE_SERVER_BUSY`、HTTP `/serverinfo` `/launch` 等 endpoint 路徑全部維持上游原名。

**理由：** 這是 GameStream wire protocol 的一部分，改了等於跟整個 Moonlight / Sunshine 生態脫鉤——使用者沒辦法用原版 Moonlight 連 VipleStream-Server，也沒辦法用 VipleStream client 連別人家的 vanilla Sunshine。User 已明確要求「混搭互聯」（v1.2.93 討論）。

**保留到永遠。** 這條留著就是要提醒未來想動的人：不要動。

### §A.8 內部 class names

**目前：** `NvHTTP` / `NvComputer` / `NvApp` / `SunshineHTTPS` / `SunshineHTTPSServer` / `SunshineSessionMonitorClass`（Windows window class）等內部命名沒改。

**清的時候要做：** 真的想改就在獨立 refactor PR 裡 batch rename，**不要**跟 feature / bugfix 混在一起（diff 會無法 review）。優先度極低。

---

## §D. 上游 wiki 連結

**狀態：** 🟡 部分 ship — `moonlight-android/.../HelpLauncher.java` 的 setup guide + troubleshooting 目前指 `https://github.com/finaltwinsen/VipleStream#readme`；GameStream EOL FAQ 維持上游 `moonlight-stream/moonlight-docs` wiki。

**已加但未連線：** [`docs/setup_guide.md`](./setup_guide.md) + [`docs/troubleshooting.md`](./troubleshooting.md) 已寫（pairing / config / FRUC / common failure modes 都有），等 GitHub Pages 或類似 doc site stand 起來後，把 HelpLauncher URL 從 README anchor 換成 docs/* 頁面 anchor。

---

## §F. DirectML FRUC — 架構級優化

DirectML auto-cascade（v1.2.91）已經把「中低階 GPU 跑 DML 會掉幀」的痛點解掉，hot path 已 ship。下面是「如果要推 DML 到 4K120 real-time」才需要的架構改動。

### §F.1 FRUC pipeline 整個搬進 D3D12

**Gain：** 消除 `ctx4->Signal`（D3D11 UMD flush）每 frame 60-80 μs。
**Risk：** 高。整個 renderer 的 present / blit / overlay 管線都要重寫成 D3D12 path。Moonlight 原本的 d3d11va / dxva2 / vaapi 多 backend 抽象要重新設計。
**工時估：** 1-2 週。
**觸發條件：** 4K120 real-time DML 需求，目前不急。

### §F.2 Command bundles for pre-recorded DML pipeline

**Gain：** 30-40 μs / frame。
**Risk：** 中。Bundle 建立後 PSO / root sig 不能改、binding 限制多。
**工時估：** 2-3 天。

### §F.3 Zero-copy D3D11→D3D12 heap alias

**Gain：** 10-20 μs / frame。
**Risk：** 中。Heap alias 的 state 管理比 texture share 嚴格。

---

## §G. DirectML FRUC — 更多 ONNX model variants

§G.2（fp16 model + cascade rewrite）跟 §G.3（IFRNet-S 試水）已 ship。重要的 negative result：**A1000 launch overhead 25 ms floor 不可繞過，任何 ML 模型 op count > 150 都上不了 14 ms budget**。

### §G.1 11-channel 輸入 (RIFE v4 v1)

**Gain：** v1 node count 少 50%（216 vs 456），可能比 v2 快 30-50%。
**重新評估：** A1000 仍可能達不到 14 ms（216 ops × 0.1 ms launch overhead = 21.6 ms 已超），但對 RTX 30/40+ 可能值得；要 ship 還是得搭 §G.4 ModelFetcher 路徑避免 release zip 再加肥（自 v1.3.311–312 起 onnx 已 on-demand download）。
**做法：** `tryLoadOnnxModel` 加 11-channel branch；pack 前加 optical flow CS 或塞 zero flow。

### 已就位的診斷工具

- `VIPLE_DIRECTML_DEBUG=1` — D3D12 debug layer + DML validation
- `VIPLE_DML_RES=540|720|1080|native` — tensor 解析度 override
- `[VIPLE-FRUC-DML]` log — 每 120 frame 印 per-stage EMA μs
- `[VIPLE-FRUC-DML-ORT]` log — ort_concat / ort_run / ort_post / wait_pack / wait_post / wait_concat 拆解

---

## §I.D Android Vulkan FRUC — async compute queue

§I 整套（A recon → B passthrough → C Vulkan FRUC port → C.6 display timing → C.7 settings UI）都已 ship（v1.2.134–161），thermal regression（dual present + per-frame `vkQueueWaitIdle` → +4°C in 60s）的 root-cause fix 是 §I.D.

### Phase D.2.x — multi-queue + cross-queue sem（**已完成**）

D.2.0–D.2.5 全部 ship + Pixel 5 / Adreno 620 真機 verify 過：

- **D.2.0**（3dc860a + 81e50d4）: multi-queue acquisition. Adreno 620 = 1 family / queueCount=3 / [GFX CMP]，driver 真給 3 distinct VkQueue handles + 接受 concurrent same-family submit。
- **D.2.1**: init-time `compute init clear` submit 拆 gfx→compute 兩個 cmd buffer 用 binary `VkSemaphore` chain。
- **D.2.2 / D.2.3**: hot-path `render_ahb_frame` ring 拆兩 cmd buffer。compute 側收 AHB import barrier + (dual mode) dispatch_fruc，submit 在 `computeQueue` signal `fSlotComputeDoneSem[slot]`。gfx 側收 render passes，wait acquireSem(s) + computeDoneSem。Single mode 仍 submit compute 的 barrier-only cmd buf 維持 sem 對稱。
- **D.2.4**: 60fps single-mode pre-vs-post split baseline — p50 +0.07ms (1.29→1.36ms)，p99 +0.52ms (2.94→3.46ms)，120s GPU +0.52°C。Cost < 1ms p99 可接受。
- **D.2.5 mailbox opt-in** (commit `0217426`): `debug.viplestream.mailbox=1` sysprop 切 MAILBOX → FIFO_RELAXED → FIFO，default off。Pixel 5 真機 A/B 3 runs × 60s × dual mode @ 90Hz：FIFO p50 14.13/p90 14.79/p99 15.70 vs MAILBOX p50 14.21/p90 14.88/p99 15.86，Δ +0.08/+0.09/+0.16 ms（noise 內），無顯著 win。

**Dual entry threshold 從 1.05× 放寬到 1.40×**（commit `1a5cc5b`）：60fps Sunshine input + 90Hz panel 在原 1.05× 永遠進不了 dual（2×60=120 > 94.5）；FIFO present 對 surplus 是 driver-side drop 不是 stutter，p50 跟 ideal 1:2 dual 一致。改後 60fps@90Hz 預設進 dual 92.6%，使用者只要設「90 fps + 開 FRUC」就能拿到 interp。

### 還沒做的 — 自然 45fps source on 90Hz panel ideal 1:2 比例測試

Pixel 5 panel 只支援 60Hz/90Hz mode，GameManagerService 又把 app default frame rate override 為 60fps，現有 hardware combo 拿不到 ≤ 45fps stable input。要驗 ideal 比例得換 LTPO panel 手機（S24+/Pixel 8）。這是 hardware-pending，**不是 client codebase 缺東西**。

### 鐵律

1. 每 Phase 都要有 baseline 對比。沿用 `scripts/benchmark/android/`，量出來不如預期就停。
2. **GLES path 至少保留到「Vulkan 穩定 6 個月」之後才移除**。期間 FRUC bug 修兩次（GLES + Vulkan）是已知 +20% 維護成本。

### 已就位的診斷工具

- `scripts/benchmark/android/android_baseline.sh` — thermal + fps + jank 採集（**local-only**，scripts/ 自 v1.4.0 起 gitignored）
- `scripts/benchmark/android/analyze_baseline.py` — 30s bucket fps + pixel-thermal 高頻交叉驗證（**local-only**）
- `debug.viplestream.vkprobe=1` system property — opt-in Vulkan FRUC backend
- `debug.viplestream.mailbox=1` system property — opt-in MAILBOX present mode (D.2.5)
- Settings UI toggle「FRUC 後端：GLES (預設) / Vulkan (實驗)」（v1.2.161 ship）
- `[VKBE-D20]` / `[VKBE-D21]` / `[VKBE-D22]` log lines 證明 multi-queue / cross-queue handoff 真在 runtime 跑

---

## §J. Desktop Client Vulkan-native（active）

**動機：** §I.F NCNN-Vulkan FRUC backend 在 D3D11 主 renderer 上踩到結構性瓶頸：

1. **D3D11 → Vulkan bridge cost ~30-40ms/frame**（CPU staging 路徑），讓 RIFE inference 21ms 只佔總耗時的 33%。
2. **Shared texture 路線在 NVIDIA 596.84 driver device lost** — `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` 任何 raw Vulkan command 都被 reject。要 ID3D11Fence + VkSemaphore 完整同步基礎設施才有機會（v1.3.43 三次嘗試都失敗，見 §J.1）。
3. **D3D11 deprecation pressure** — Microsoft 主推 D3D12 + WinUI；D3D11 video decoder 不太可能加新 codec（AV2 / VVC）。
4. **Cross-platform 一致性** — Android client 已是 Vulkan-native，desktop 也走 Vulkan 後 codebase 大幅簡化。

### 階段表

| Phase | 內容 | 狀態 |
|---|---|---|
| **§J.1** D3D11.4 fence ↔ VkSemaphore bridge | NCNN shared path 同步基礎建設 | ❌ DEAD END NV 596.84 — 4 種 ablation 全 device lost |
| **§J.1 路線 A** | 完整 ID3D12Device bridge (~250 LOC) | ⏳ 未驗 |
| **§J.2** Vulkan post-processing pipeline | FRUC、HDR shader、blit、swapchain 全進 Vulkan | 🟡 跟 §J.3.e 大幅重疊 |
| **§J.3** VK_KHR_video_decode 完整 Vulkan-native pipeline | 透過 ffmpeg 8.x Vulkan hwaccel + PlVkRenderer | ✅ 基礎已存（HEVC 直通）；§J.3.e FRUC 整合進行中 |
| **§J.4** HEVC + H264 decode + 跨平台驗證 | Linux full coverage；macOS 路徑決策 | 規劃中 |
| **§J.5** 整合測試 + fallback hardening + 預設切換 | NV / AMD / Intel bench；舊 driver 退回 D3D11；預設改 Vulkan | 規劃中 |

### §J HEVC 1440p120 SW decode cap — **resolved by §J.3.f**

歷史症狀：RS_VULKAN + VkFrucRenderer 走 SW HEVC decode 在 mobile CPU 上 1440p120 卡 ~50 fps（FRAME-threading throughput 限制）；2026-05-03 兩端 pktmon pcap 確認 wire 0 loss、root cause 在 client decoder。

**§J.3.f rebuild（FFmpeg 8.1 + `hevc_vulkan` hwaccel）後解掉**：HEVC HW Vulkan 1440p120 跑 119–122 fps、decodeMean 0.30 ms、networkDropped 0。整合 commit b2b7afd 起 RS_VULKAN 預設走這條，使用者無須額外勾選。SW HEVC 1440p cap 仍存在（CPU 上限），但走 RS_D3D11 (DXVA HW) 或 RS_VULKAN (HW Vulkan) 都能避開。

**保留的診斷工具**（之後其他 throughput 問題還會用）：[`docs/diag_wire_loss.md`](./diag_wire_loss.md) 雙端 pktmon SOP、`[VIPLE-NVENC-RATE]` / `[VIPLE-BCAST-RATE]` / `[VIPLE-NET]` 三段 rate trace。

### §J.3.e VkFrucRenderer（Android architecture port，**目前主戰場**）

既有 `moonlight-android/.../jni/moonlight-core/vk_backend.c` ~4220 行 production-tested native Vulkan FRUC，PC 端 port 70% 直接可用。

#### Sub-phase 表

| Sub-phase | 內容 | 狀態 |
|---|---|---|
| **§J.3.e.2.i.1-7** Init / device / swapchain / FFmpeg-Vulkan bridge | full set | ✅ ship |
| **§J.3.e.2.i.8 Phase 1.x** H.265 native VK_KHR_video_decode | NvVideoParser + VkVideoSessionKHR + cross-queue timeline sem | ✅ v1.3.251（84.78fps PARALLEL stable）|
| **§J.3.e.2.i.8 Phase 1.5c** ONLY mode device-lost graceful degrade | DEVICE_LOST → m_DeviceLost flag → render/decode 早 return | ✅ v1.3.295 |
| **§J.3.e.2.i.8 Phase 1.6** Aftermath GPU crash dump | NV Nsight Aftermath SDK 1.6 + checkpoints + auto-write `.nv-gpudmp` | ✅ v1.3.298 |
| **§J.3.e.2.i.8 Phase 1.7e** ONLY env rename → `*_DANGEROUS` | 5 個變體都繞不過 NV 596.36 bug，預設 PARALLEL | ✅ v1.3.302 |
| **§J.3.e.2.i.8 Phase 2** H.264 native VK decode port | nvvideoparser H.264 sources + submitDecodeFrameH264 | ✅ v1.3.275 |
| **§J.3.e.2.i.8 Phase 2.5** FRUC NV12 source 整合 native decode | per-slot buffer 改善大半，殘留小 race 等 J.5 整體切換時補完 | 🟡 v1.3.277，不擋使用 |
| **§J.3.e.2.i.8 Phase 3** AV1 native VK decode plumbing | parser + submitDecodeFrameAv1 | ✅ v1.3.272，submit 預設 OFF（pending Phase 3d.6）|
| **§J.3.e.2.i.8 Phase 3d.6** AV1 native decode GPU-side grey | 9 個 parser real bug 修了仍 grey，driver-level | 🟡 deferred indefinitely — AV1 走 libdav1d SW |
| **AMD ycbcr** descriptor pool dynamic sizing | `VkSamplerYcbcrConversionImageFormatProperties::combinedImageSamplerDescriptorCount` × N | ✅ v1.3.307 |
| **Vulkan demoted to experimental** | 預設 `RS_D3D11`、Vulkan 標 [實驗性] | ✅ v1.3.308 |

#### SW Vulkan path 優化（**1080p120 × 3 codec 全 PASS at v1.3.323**）

最近一波收尾：

- **v1.3.318** libdav1d threads + `max_frame_delay=1`：AV1 1080p120 p95 16.32 → 14.52ms（commit `83cc409`）
- **v1.3.320** HEVC FRAME threading + 1440p/2160p × 3 codec measurement（commit `0786791`）：H.264 + AV1 1440p120 PASS；HEVC 1440p120 卡 60fps 是 §J HEVC 1440p server cap（上面）
- **v1.3.321** per-slot staging buffer + async memcpy ↔ GPU upload pipelining（commit `0666a1d`）：close 了 v1.3.276 註解承認的 staging-buffer WAW race，4K 場景 marginal win（decoder-bound）
- **v1.3.323** SSE2 YUV420P→NV12 UV interleave（commit `440204b`）：4K UV memcpy 1240us → 370us，**4K AV1 SW decode 62 → 76fps**

1080p120 final state（30s × 3 codec, RS_VULKAN + VDS_FORCE_SOFTWARE, FRUC off）：

| codec | mean fps | p50 ms | p95 ms |
|---|---|---|---|
| H.264 | 120.02 | 8.47 | 15.95 |
| HEVC | 119.89 | 8.39 | 11.18 |
| AV1 | 120.01 | 7.35 | 14.52 |

4K@120 SW decode 仍是 CPU codec-bound（H.264 90fps / AV1 76fps / HEVC 28fps），需要更新世代 CPU（Zen 5 / Raptor Lake）才有可能往上推；client SW path 的 Vulkan 上傳路徑本身已不是瓶頸（renderFrameSw 整段 < 3ms 即使 4K）。

### §J.3.e.X 手刻 RIFE Vulkan pipeline（**✅ correctness milestone DONE 2026-05-05**）

**達成：** 從零實作 RIFE-v4.25-lite 全套 Vulkan inference，不依賴 ncnn。
17 個 commit (`8807886..cba641d`) 涵蓋 .param parser → 11 種 op shader →
389-layer graph executor → vs-ncnn correctness gate。完整 milestone 細節
跟 commit map 在 [`docs/J.3.e.XY_native_rife_pipeline.md`](J.3.e.XY_native_rife_pipeline.md)。

**Correctness：** Final.1 vs-ncnn `mean=4.72e-05 max=0.014 frac>1e-2=0.00%`
（4Y.5a 後）。99.49% pixel diff ≤ 0.01 視覺等價，0% pixel diff > 0.01.

**Production 整合狀態：** 標準的 standalone 路徑可由 env var
`VIPLE_RIFE_NATIVE_VK_TEST=1` 觸發跑全套 self-test。production
streaming 路徑**沒有改動**，仍走 NcnnFRUC. Final.3b（接成 NcnnFRUC
的 Linux fallback）刻意 deferred 直到 Linux test VM 有 ncnn-build-fail
環境可重現驗測。

**2026-05-08 strategic priority bump — Final.3b 升 active：** §B / §B-NVOF
量化驗測（compare_fruc_engines warp ratio）發現我們所有自家 ME→warp→blend
pipeline ceiling ~30%，跟 NvOFFRUC.dll 47.7% 沒法追平（架構性 N-stage gap）。
**ML-based interpolation 是唯一 ceiling 60-80% 的路**。Final.3b 從 deferred
升到 active，原 deferral reason（Linux test VM 驗測前不啟動）由「production
default 切換前才需要 Linux 驗測」取代 — 現在先做 Windows production
integration（env-var gated default-OFF），Linux 驗測併入後續 ship 流程。

**Final.3b 工程計劃：**
1. 在 NcnnFRUC 加 `RifeNativeExecutor` 替代路徑（drop-in，相同 input/output 介面）
2. 由 env var `VIPLE_RIFE_NATIVE_VK_PROD=1` 切換 ncnn ↔ native，預設 ncnn 不變
3. Per-frame 跑 native inference，輸出餵下游 D3D11→Vulkan upload + present 跟 NCNN path 共用
4. 第一輪只在現有 RIFE-v4.25-lite model 跑 1080p / 720p，4K 等 §J.3.e.Y 4Y.5b/4Y.6 latency 優化降到 budget 內再 unlock

**預期 warp ratio**：60-80%（基於 DirectML 同 RIFE 模型 27.6% baseline + 推估 native 沒 D3D 框架 overhead 的進一步贏面）。

**2026-05-08 Path β 實做 + 量測（commits `67a7c89` β.1+β.2 / `20507da` β.4）：**

NCNN drop-in 那條（Final.3b）撞到 NV driver 596.144 dual-VkDevice
`VK_ERROR_INITIALIZATION_FAILED` —— 已被 **Path β** 取代：直接把
`RifeNativeExecutor` 接到 `VkFrucRenderer` 既有的 VkInstance/VkDevice/
universal queue，繞過 NCNN 整條路。架構落地的三個 commit：

- β.1 (env-var gate `VIPLE_VKFRUC_NATIVE_RIFE=1`) + β.2 (chain swap via
  新的 `runInferenceGpu(cmd, slotIdx, in0, in1, t, out)` API + per-slot
  descPool —— `runFrucComputeChain` 在 stage 0 NV12→RGB 之後 RIFE 接管，
  ME/median/warp 完全跳過)
- β.4 (bilinear down/up wrapper)：1920×1080 → 256×128 → RIFE → 256×128
  → bilinear up → 1920×1080。default infer dim 256×128（更高 dim 撞
  RIFE-v4.25-lite 自身的 layer `Add_503` shape constraint —— a.h round
  到 next /128 multiple，跟 b.h 不對齊；inferShapes 應該也跟著計算 padding，
  TODO 修）

**RIFE 真的在跑**（live test stderr 出 `[VIPLE-VKFRUC-RIFE-β] chain swap
active`，無 SDL Error/Warn，無 dispatchBinaryOp fail）.

**Quality 量測 (256×128 default)：** verify_dump_interp.py 抓 5 對
real/interp 看 `diff(real_N, interp) / motion_mag` score 都 1.7-2.7（理想
1.0 = 完美 midpoint）.  乍看 FAIL，但 root cause 拆解後不是 RIFE 錯：

  pair_adj (mean |real_N - interp|) ≈ 4.04
  bilinear roundtrip 1080p→256×128→1080p 自身就 4.48 mean abs loss

也就是 **bilinear 7.5× downscale → upscale 自帶的 detail loss 已經完整
解釋住觀察到的 residual** —— RIFE 在 256×128 推論本身是有效 midpoint，
但接出來的圖被 bilinear up smooth 掉細節（高頻內容遺失）使得跟 1080p
real frame 的 pixel-level diff 偏大。

**已 ship（commits 67a7c89 → d5f2812 + 3.4 v6 dump-skip）：**

- β.4 v4 解開 Add_503 root cause：`Resize_47` hardcoded /32 + 兩個 stride-2
  conv = encoder /128 → decoder ×128，inputH 不是 /128 整除時 a.h round 上
  跟 b.h 不對齊。修法 inferW/H round 到 /128 multiple（不是 /32）。
- β.4 v5 timing instrumentation：runRifeNativeStage 寫 ts[2..4] 進
  m_FrucTimerPool，[VIPLE-VKFRUC-GPU-PROF] 顯示 nv12rgb / DOWN / RIFE / UP /
  copy 分別 latency.
- β.4 v6 dump-skip：m_RifeNativeReady 時跳過 §B-DUMP 的 vkCmdCopyBuffer
  流程（避免 m_FrucMvFilteredBuf 未寫 + interp barrier mask 對不上）。

**Latency table on RTX 3060 Laptop (live test 量得)：**

| inferDim | RIFE inference | total chain | 60fps DUAL 吃得起？ |
|---|---|---|---|
| 256×128 | 11.9ms | 12.4ms | ✓ 60fps 穩 |
| 512×256 | 29.1ms | 29.7ms | ✗ ~33fps drop |
| 768×384 | ~50ms 推估 | ~51ms 推估 | ✗ |
| 1024×512 | ~80ms+ 推估 | ✗ |

預設 256×128。更強 GPU 可 `VIPLE_VKFRUC_RIFE_INFER_DIM=512` 拉到 quality
明顯提升的 dim（必須 /128，否則撞 Add_503）。

**已知問題：**

- ~~**Path β 30-90s device-lost crash**~~ ✅ **RESOLVED v1.4.0 (β.6 stability fix)** — Nsight Graphics `nv-aftermath-format -p VipleStream.pdb -g <shader-dbg>` symbolize 出 `fragment_01` 不是 `vkfruc.frag` 而是 `vkfruc_overlay.frag`，page-fault site 是 `VkFrucRenderer::drawOverlayInRenderPass` 的 use-after-free（`drainOverlayStash` 在 perf overlay surface dim 變動時 immediate destroy 舊 `m_OverlayImage[type]`，但 Pacer renderThread 同時可能有 2-3 個 in-flight cmd buffer 還會 sample 舊 view）。Path β chain 14ms 比 block-match 4ms 多 in-flight cmd buffers，所以一個 overlay resize 就有 race window。修法：`drainOverlayStash` line 3317-3341 在 destroy 舊 image 前 `vkDeviceWaitIdle`。Cost: overlay resize 才 fire，一次 stream 通常 0-2 次。**驗測 7m49s 連續 0 crash 0 dump 0 device-lost log**。詳見 commit `a72b886`。

- ~~**SW decode mode + §B-DUMP 加速 crash**~~ — 跟上一條同根因（drainOverlayStash race），β.6 fix 一併解。`commit 2a5732e` 的 dump-skip guard 仍保留作 belt-and-braces。

**還沒做的下一步候選：**
1. 修 SW-mode device lost — 使 §B-DUMP 自動驗測 work
2. **§β.5.3 D 全套** — inferShapes / dispatchBinaryOp 處理 asymmetric
   center-crop pattern 解開「inferDim 不對齊 /128 但仍 valid」的情形
   （解開 192/320/448 中間 dim + 讓 4Y.5b 重做時有真的可選的小 dim）.
   目前 D-lite UP-rounding (β.5.3, `9d4c237`) 是 stop-gap，副作用是
   1080p source 配 cfg 256 變 256x256 不是 256x128（pixels 翻倍 + 4Y.5b
   negative result 直接源於這個）.
3. ~~β.3 TRIPLE 支援~~ ✅ **β.9 Phase 1 ship in v1.4.6 (`99f86b5`)** —
   t=1/3 + t=2/3 兩次 inference 已落地，互斥鎖解除. 剩 β.9 Phase 2
   (graph executor timestep-shared frontend split) active，使 chain × 2
   從 200% 降到 ~130%
4. 找更聰明的 upscaler 取代 bilinear up，減 8× 上採 blur — ✅ **β.5.2
   Catmull-Rom bicubic ship in v1.4.1 (`a8b3204`)** for flow + mask
   upsampling. warp shader 對 source RGB 仍用 bilinear，可進一步升 bicubic
   （使用者回報的「補幀畫面糊」可能是這條造成）→ §β.11 active item

### §J.3.e.Y native RIFE perf optimisation（**✅ milestone DONE 2026-05-06**）

**達成：** §J.3.e.X 的 86 ms cold-GPU baseline 壓到 ~22 ms（4× 加速），
9 個 commit (`876ce45..068c9b2` + 文件 `e4e4dfc`)。詳細 phase map 在
[`docs/J.3.e.XY_native_rife_pipeline.md`](J.3.e.XY_native_rife_pipeline.md)。

| Phase | 內容 | 結果 |
|---|---|---|
| 4Y.1a | `findHostVisibleMemoryType` 偏好 BAR/ReBAR | 86 → 43.6 ms (2.0×) |
| 4Y.0 | per-phase wall-clock instrument | (找出 readback 是 24% 大頭) |
| 4Y.1b | out0 host-cached staging buffer | 43.6 → 24.0 ms (3.6×) |
| 4Y.4 | tiled shared-mem Conv2D for k=3 s=1 | 24.0 → 21.5 ms (4.0×) |
| 4Y.4-stride2 | s=2 tiled — barrier overhead | (negative, reverted) |
| 4Y.5a | fp16 weight storage | perf 持平、**vs-ncnn 9× 收緊** |
| 4Y.6 Step 1+2 | cooperative_matrix probe + hello-world | infrastructure 完成 |
| 4Y.6 Step 3 | Conv2D-via-GEMM coopmat shader + unit test | isolated PASS |
| 4Y.6 Step 4 | dispatch path 整合 + env-var gate | **opt-in default-OFF** |

**4Y.6 為什麼 opt-in（RTX 3060 Laptop 量到的 trade-off）：**

| 指標 | OFF（4Y.4 路徑） | ON（coopmat） |
|---|---|---|
| Final.1 mean abs err | 4.72e-05 | 3.21e-03（70× 差） |
| Final.1 max abs err | 0.0135 | 0.530（39× 差） |
| Final.1 verdict | PASS | **FAIL** |
| Final.3a native median | 19.25 ms | 17.95 ms（**僅 7% 加速**） |

Precision regression 是 double-fp16 quantisation 跨 40 layer 累積（σ × √40
≈ 3.2e-3，數學可預期，shader 本身正確）；perf 只贏 7% 是因為 RIFE-v4-lite
每層 conv 太小，Tensor Core fixed-cost 吃掉大半 throughput。7% 遠小於
thermal-throttle 噪音 ~30%，不值得 ship 預設。`VIPLE_RIFE_VK_COOPMAT=1`
opt-in 留著當 known-working capability，未來推 4090 或更大 RIFE 模型時
直接 build on。

**Thermal noise 警告：** GPU sustained workload vs short cold-GPU benchmark
行為差很大（ncnn 退化 10× vs native 2×）。進一步 1.5× 量級的優化 benchmark
驗測需要 fixed clock (`nvidia-smi --lock-gpu-clocks`) 或 cool-down protocol。

**還沒做但有 ROI 的候選（沒人推之前 deferred）：**

- **4Y.6 shader 改 multi-subgroup WG** — 1 WG = 1 subgroup 的設計使 SM
  利用率僅 ~50%；改成 4 subgroups/WG 共享 im2col tile load 預期再 1.5-2×
  攤平 fixed cost。但 RIFE-v4-lite 規模太小，可能仍贏不過 4Y.4 多少
- **4Y.7 Conv→ReLU dispatch fusion** — C.1 (Conv→Mul channel-bcast beta)
  已 ship in 0e6240a (v1.4.1)；Conv→ReLU 合併還沒做，預估 chain -10%

**Negative result (走錯後 revert，2026-05-09)：**

- **4Y.5b activation fp16 storage** — wip/4Y.5b-activation-fp16 branch
  留檔 (82afee5)。256x256 (1080p 配 D-lite UP) 場景下 chain 反而 +1.7ms
  是負收益，跟 4Y.5a postmortem「weight L1-cache-bound 不是 bandwidth
  bound」結論吻合。要重做必須先解 D-lite asymmetric center-crop（§β.5.3
  D 全套）讓 256x128 真的可選；目前 cfg=256 強制變 256x256，bandwidth
  mention 的 ~485 blobs × 2 reads + 1 write 跟 weight L1 比仍不是 dominant

**Final.3b 接 NcnnFRUC：** 仍 deferred until Linux env。預估 4-6h 工程，
medium-high risk（動 production hot path）。

### §B Vulkan HW path FRUC integration（**📦 maintenance 2026-05-08，A' 收尾、Path B 不啟動**）

**2026-05-08 strategic decision — pipeline 收尾 + ML pivot：**

`compare_fruc_engines.ps1` 量化驗測 7 engines 後得 warp ratio：

| Engine | Warp Ratio | 說明 |
|---|---|---|
| 03 d3d11_nvidia_of (NvOFFRUC.dll) | **47.7%** | 標竿，NV 私有 SDK 全套 N-stage pipeline |
| 04 d3d11_directml (RIFE ML) | 27.6% | ML-based，但「慢到不堪用」latency |
| **06 vkfruc_nvof (A' 修後)** | **7.5%** | A'.2 consensus-max NVOF converter |
| **01 vkfruc_bm (A' 修後)** | **6.8%** | A'.1 luma + range expansion |
| 02 d3d11_generic (HLSL 沒改) | 3.0% | A' 沒動到 D3D11 HLSL shader |
| 05 d3d11_ncnn | 2.4% | known broken (§B-DUMP NCNN all-zero output) |

**A' (commit `XXX`，2026-05-08)** 兩條 shader 改動：

- **A'.1** [plvk.cpp ME shader](../moonlight-qt/app/streaming/video/ffmpeg-renderers/plvk.cpp)：R-channel-only census → luma census；diamond steps `[3, 1]` (±4 px) → `[32, 16, 8, 4, 2, 1]` (±63 px)
- **A'.2** [vkfruc.cpp NVOF converter](../moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.cpp)：4×4 average → consensus-max with noise floor

具體量化：MV stats `max|x|` 從 5 LSB Q1 (=2.5 px) → 58 LSB Q1 (=29 px)，符合 testufo / 遊戲場景 30+ px/frame UFO motion 的真實值。

**Path B 預估完成後仍只 ~20% warp ratio**（occlusion mask + reverse-flow + multi-scale + Q5 sub-pixel 全做完，[詳細評估在這次 commit 的對話中]），跟 NvOFFRUC 47.7% / ML 60-80% 都差太遠。**結構性 capacity gap**：NvOFFRUC 是 8-stage 整合 pipeline + NV 多年調教，我們 4-stage stub-level 實作；加 4 stage 變 6-stage 仍不如 8-stage。

**結論：A' 是現有 ME-based pipeline 的合理 ceiling。Path B 不啟動，資源轉投 ML 路線（§J.3.e.X Final.3b production integration）。**

---

直到 §B 之前，RS_VULKAN HW mode 雖然 frucMode/dualMode 都自動為 true，
但 `renderFrame()` 從來沒接 `runFrucComputeChain` 或 dual-present —
等於 production HW path 完全沒在補幀（解釋了使用者觀察「testufo
30→60 補幀沒效果」的根因）。

| Step | Commit | 內容 | 狀態 |
|---|---|---|---|
| A baseline | — | 1080p60 SW path 量 GPU-PROF baseline (15 samples) | ✅ nv12rgb=410 me=411 warp=214 total=1248us |
| B1a | `03a962f` | HW path 加 image-to-buffer copy + FRUC chain dispatch | ✅ total=750us（比 SW 快 40%） |
| B1b | `a86df9f` | HW path 加 dual-present（real + interp 兩次 swapchain acquire/render/present） | ✅ overlay「已啟用」、cumul real:interp = 1:1 |
| B-quality (a)(b)(d) | `cfe5396` `1f774b9` `24e9895` | 演算法品質里程碑（見子章節） | ✅ SSIM 0.88 → 0.998 |
| B-quality benchmark infra | `8898e1d` `fa2a2fd` `1cf94ed` `c18d0a6` | 2-stage 抖動量測腳本 + analyze_motion.py + DIAG-OVERLAY log | ✅ ship |
| **B2** TRIPLE 60→180 | `bc88eba` | infrastructure 全套（兩個 interp output / 第三 swapchain acquire / pacer +2） | ✅ env-var opt-in，**UI 整合 pending** |
| **B-NVOF** | 11 commits Phase 1..7B | NVIDIA Optical Flow Vulkan extension 取代 software block-match ME | 🟡 7B async ship，**production 切換 + UI 整合 pending** |

#### §B-quality 補幀演算法品質（主要里程碑：commit `24e9895` algo_d）

**(d) real path 改走 compute-NV12→RGB（治本，2026-05-XX `24e9895`）**

dual-present 兩條 path 之前用不同 NV12→RGB conversion：interp 走 compute
shader 寫 fp32 buffer 經 vkfruc_interp.frag 顯示；real 直接吃
`VkSamplerYcbcrConversion`。色彩矩陣 / chroma 上採樣 / sub-pixel 對齊都
可能不一致，30Hz dual-present 交替時看到「同幀內容、不同 RGB convert」
造成的 Y 軸抖動。先前的 warp 演算法疊代（cheap-adaptive / fixed 50/50 /
d3d11 Quality / no-MV）全抖 — 因為跟 warp 完全無關。

修法：real render pass 也改吃 `m_FrucCurrRgbBuf`（compute shader 的 fp32
output），跟 interp 走同條 path。`VIPLE_VKFRUC_REAL_USE_CRGB` default ON，
escape hatch 設 0 退舊 ycbcr。Single-mode 不抖（沒 dual-present），維持
ycbcr sampler 路徑。

量化（testufo 1080p60 1920×40 band，60s）：

| run | SSIM | OF_30Hz |
|---|---|---|
| baseline_v3 (median noop) | 0.88 | 5.5% |
| algo_a (median ON, `cfe5396`) | 0.88 | 4.4% |
| algo_b (block 8→16, `1f774b9`) | 0.84 | 1.93%（block 16 對水平 motion Y 軸引 noise，後 (d) revert） |
| **algo_d (real_use_crgb, `24e9895`)** | **0.998** | **2.75%** |

User 主觀：「不抖、顏色看不出異常」。block_size 16 在 (d) 一併 revert 回 8。

**還沒做的（B-quality 收尾）：**

- (c) 從未 commit；blend factor 0.5 hardcoded、occluded/disocclusion 沒
  特殊處理 —— `24e9895` 後改由 §B-NVOF 用 HW optical flow 取代 software
  block-match 來解 ME 噪聲，`(c)` 概念上由 NVOF 路線吸收
- 自然 video 驗測（目前都 testufo trajectory band，非真實內容）

#### §B2 TRIPLE 60→180（infrastructure ship at `bc88eba`，opt-in）

VkFrucRenderer dual-present (60→120) 擴展為 triple-present (60→180)。
每 server frame compute 出 2 張 interp（1/3 + 2/3 點）+ real，3 張
swapchain image present 給 180Hz display。`VIPLE_VKFRUC_TRIPLE=1` 啟用，
gated by m_DualMode + m_FrucMode + 180Hz display，預設 OFF。

關鍵改動 (`bc88eba`)：
- warp shader push constant 6→8 欄，加 `tFraction` 控制 sample offset +
  blend weight（0.5 = DUAL midpoint，1/3+2/3 = TRIPLE 兩張 interp）
- `runFrucComputeChain` TRIPLE 模式 dispatch warp 兩次（不同 desc set +
  tFraction，輸出 `m_FrucInterpRgbBuf` / `m_FrucInterpRgbBuf2`，GPU 並行）
- `renderFrame` 加第三個 swapchain acquire (`m_SlotAcquireSem[][2]`) +
  第三 render pass + submit 4-sem 變體 + 第三 present；order: interp_1 →
  interp_2 → real
- swapchain image count 4→5（防 burst-acquire 3 張撞 caps.minImageCount）
- session.cpp：server fps = user_fps / 3（DUAL 時是 / 2）；toggle FRUC
  在 TRIPLE 模式不發 LiRequestFpsChange（NVENC encoder timebase 不可動
  態升 ~2× 上限會卡頓，Sunshine `video.cpp:2154` 提示需 stream restart）
- `lastFrameInterpolatedCount()` 介面取代 `lastFrameHadFRUCInterp`，
  TRIPLE 回傳 2 讓 effectiveFps 算對（overlay 顯示 ~180）
- `scripts/benchmark/launch_warp.cmd` 加 triple 入口

驗測（1080p60 testufo，`bc88eba`）：
- GPU profile：warp 兩次 dispatch GPU 並行，total chain 0.93ms（DUAL ~0.9ms）
- client present rate：cumul real:interp = 1:2，60+120 = 180 presents/sec
- DIAG tint test：3 張 interp 確實獨立 present，紅 (interp_1) + 藍
  (interp_2) + 正常 (real) 在 60Hz fusion 邊界 blend 為紫色點狀

**已知限制（`bc88eba` 寫進 commit）：** block-matching ME 精度上限影響補幀
品質 — testufo 小快速物體在 disocclusion 邊界 ME 噪聲 → warp ghost。User
主觀對比 60→180 vs 60fps 看不出明顯 smoothness 提升（block-matching 本
質限制，非實作 bug）。下一步：探勘 NVIDIA Optical Flow Vulkan extension
取代 block-matching → §B-NVOF。

**還沒做的（B2 收尾）：**

- Settings UI 整合：`VIPLE_VKFRUC_TRIPLE` env var → SettingsView.qml
  選項（與 fps mode 一起判斷顯示 / 隱藏）；目前只能 env-var 開
- TRIPLE × NVOF cross-test（§B-NVOF Phase 7E，analyze_motion.py 已 fix
  OOM bug `83157fa`，可跑了）
- 自然 video 驗測（非 testufo）

#### §B-NVOF NVIDIA Optical Flow Vulkan（**🟡 active，Phase 7B 最新 2026-05-07**）

**動機：** §B2 TRIPLE 跟 §B-quality (d) 後，剩下的補幀品質瓶頸在
software block-match ME 本身（411us / frame，但 disocclusion 邊界
產 noisy MV，跨幀 alternation 高）。NV Ada / Ampere 起 Vulkan 新增
`VK_NV_optical_flow` extension，可用 NVOFA HW 直接給 SFIXED5 (1/32 px)
sub-pixel flow image。整套 §B-NVOF 把 Stage 1 (block-match ME) +
Stage 2 (median) 替換成「flow image → SFIXED5→Q1 converter compute」，
warp shader (Stage 3) 跟 mv_filtered buffer contract 不變，所以 §B-quality
的 blend mode 都自動受惠。

| Phase | Commit | 內容 |
|---|---|---|
| 1+2 | `b9da1cc` | VK_NV_optical_flow extension probe + queue scaffolding |
| 3a+3b | `de2095e` | SDK header import + nvofapi64.dll load + funcList init |
| 3c | `b7a19b5` | OF session lifecycle (`nvCreateOpticalFlowVk` + `nvOFInit`) |
| 3d | `67f6146` | input/output VkImage alloc + `nvOFRegisterResourceVk` |
| 4a | `fcb0e06` | OF queue cmd pool + timeline sem + flow staging buffer |
| 4b | `61ea72e` | `nvOFExecuteVk` smoke test，OF runs each frame，SUCCESS |
| 5 + 4d | `8daf9e2` | SFIXED5→Q1 converter compute shader + chain consumes HW OF flow |
| 6 | `3de8507` | docs entry + 收尾，`VIPLE_VKFRUC_NV_OF=1` opt-in 整段 path |
| 7C+7D | `71d1592` | `VIPLE_VKFRUC_NV_OF_PERF` (slow/medium/fast) + `_GRID` (1/2/4) env，**default grid=2 perf=med 是甜蜜點** |
| 7F | `49b736c` | `nvOFGetCaps` device probe（informational log）+ Phase 7A Q5 嘗試 revert（precision != quality，Q1 quantisation 意外當 temporal smoother） |
| 7B | `c82196e` | async cross-queue：CPU `vkWaitSemaphores` ~3-5ms 阻塞改 1-frame async lag，timeline sem 在下幀 main submit wait list 攔住 race |

**Quality benchmark（testufo 1080p60 60s，2026-05-07）：**

| 配置 | SSIM | OF_30Hz | OF_cv | block_outlier |
|---|---|---|---|---|
| production block-match (algo_d, `24e9895`) | 0.998 | 2.75% | 1.94 | 4.3% |
| NVOF grid=4 perf=med（前預設）| 0.999 | 5.99% | 0.86 | 4.6% |
| NVOF grid=4 perf=slow | 0.999 | 4.25% | 0.78 | 4.8% |
| **NVOF grid=2 perf=med（新預設）🏆** | **0.999** | **2.66%** | **0.86** | 4.6% |
| NVOF grid=1 perf=slow | 0.999 | 3.17% | 1.75 | 4.3% |

NVOF grid=2 perf=med 在三個關鍵指標全面 ≤ production：OF_30Hz 終於不
輸 block-match（2.66% < 2.75%），SSIM 持平（0.999 ≥ 0.998），motion
smoothness 大幅勝（cv 0.86 << 1.94）。grid=1（更高解析度 flow）反而
cv 升回接近 production，因 8×8 average 在 converter 過度 smoothing motion
magnitude variation。**證明更高解析度 flow 不一定更好，要跟 converter 端
averaging 互動。**

**7B async（`c82196e`）後實測：**
- nvOFExecuteVk #0/#300/#600/#900 status=0 持續成功
- chain consumes HW OF flow each frame
- 沒 VK_ERROR_DEVICE_LOST，沒 race
- Frame timing: n=152 fps=30.24 ft_mean=33.07ms p50=33.36 p95=34.16 p99=46.72
- p99 比 sync 路徑稍寬但無明顯 perf regression

**還沒做的（§B-NVOF 收尾）：**

- **Settings UI 整合**：`VIPLE_VKFRUC_NV_OF` / `_PERF` / `_GRID` env var →
  SettingsView.qml；目前 env-var opt-in，production 預設仍走 block-match
- **production 預設切換**：grid=2 perf=med 7B async 已贏 block-match，
  穩定一段後可考慮 default ON（`m_NvOfReady` 自動偵測 + fallback 完整保留）
- **Phase 7E TRIPLE × NVOF cross-test**：analyze_motion.py OOM 已 fix
  (`83157fa` chunk to_gray uint32 acc)，benchmark 可跑了；但 60s × 180fps
  Farneback OF 後分析 ~10+ min CPU，建議改 20s window 或 chunk OF
- **4K120 × NVOF stress**：7B async 提供的 perf headroom 預計可吃 4K120，
  未量過
- **Linux nvofapi.so.1 dlopen 路徑**：目前 Windows-only，Linux 需要
  `libnvidia-opticalflow.so.1` 找路徑（Sunshine apt deps 有 nvidia
  driver 通常自帶）

### §J.3.f AV1 / HEVC / H.264 Vulkan hwaccel via ffmpeg（**✅ DONE 2026-05-03 / integration b2b7afd**）

**達成：** rebuild 出 minimal FFmpeg 8.1 client DLL（`avcodec-62.dll` 5.2 MB），含 `--enable-vulkan --enable-hwaccel=h264_vulkan,hevc_vulkan,av1_vulkan --enable-libdav1d`。整合 commit `b2b7afd` 起，**RS_VULKAN preference 自動觸發 Vulkan HW decode + FRUC + DUAL**（不再需要 `VIPLE_USE_VK_DECODER=1` / `VIPLE_VK_FRUC_GENERIC` / `VIPLE_VKFRUC_HW` env 三件組）。env var 仍保留作 explicit override / debug fallback。

#### 1440p120 即時實測（v1.3.333，env-var opt-in 路徑）

| Codec | received fps | decodeMeanMs | networkDropped | hostLatencyAvgMs | Vulkan HW |
|---|---|---|---|---|---|
| **H.264** | 121–123 | **0.26–0.55 ms** | 0 | 7.3 ms | ✅ |
| **HEVC** | 119–122 | **0.30 ms** | 0 | 3.5 ms | ✅ |
| **AV1** | 121–125 | **5–8 ms** | 0 | 2.7 ms | ✅ |

#### 4K120 + FRUC + DUAL × 3 codec（v1.3.333+ b2b7afd integration，full-power GPU）

| Codec | fps | decode | 備註 |
|---|---|---|---|
| **H.264** | 92 | 0.6 ms | host NVENC 4K H.264 編碼上限 |
| **HEVC** | 101–103 | 0.4–0.7 ms | host NVENC 4K HEVC 編碼上限 |
| **AV1** | 116–119 | 4.2–4.8 ms | 達 120fps target，dedicated_dpb 5ms 底線 |

5-env-var 路徑跟 RS_VULKAN auto-detect 路徑量到完全相同 fps，整合無 regression。

**對比之前 SW HEVC 1440p120 cap：** received 50→122 fps（2.4×）、decodeMean 100ms→0.3ms（**300×加速**）、networkDropped 34–51→0。§J HEVC 1440p120 cap 完全解決。

**AV1 latency tune（2026-05-03）：** ffmpeg native AV1 decoder 在 [`vulkan_decode.c:1368`](https://git.ffmpeg.org/gitweb/ffmpeg.git/blob/HEAD:/libavcodec/vulkan_decode.c#l1368) 硬寫死 `|| AV_CODEC_ID_AV1` 強制 dedicated_dpb（out-of-place decode + DPB→output copy）。此設計對 game streaming 有 trade-off：copy step 成本 ~5ms，但讓 Pacer 持有 output 不會 block 解碼器。`extra_hw_frames=4` 的預設 pool 太深（max_refs 8 + 1 + 4 = 13 slots × Pacer 持有 ~4 = 9 frames pipeline = 75–100ms）；改 `extra_hw_frames=1` 後 pool=10、pipeline 1 frame，降到 5ms steady。試過 `=0`（pool 太緊 → 卡 Pacer + 78ms startup spike）、`=2 + FLAG2_FAST`（variance 大）、`EXPORT_DATA_FILM_GRAIN`（無效）、patch ffmpeg `vulkan_decode.c:1368` 拿掉強制 dedicated_dpb（**反而更糟** — Pacer 持有 output 會 block 下一 frame 的解碼，因為 output 即 DPB 共用）。**5ms = AV1 vulkan 在這條 driver+架構下的硬底線**，HEVC 0.3ms 是因為它的 reference pattern 不衝突 Pacer hold。

#### 跟 §J.3.e.2.i.8 Phase 3d.6 對比

Phase 3d.6 自製 raw `vkCmdDecodeVideoKHR` + 我們自己跑 nvvideoparser，9 個 parser bug 修完仍 grey。**ffmpeg 包裝走 ffmpeg 內建 parser + DPB 管理，避開了我們自製 parser 的問題**，同樣的 NV driver 上對 H.264/HEVC/AV1 三個 codec 都能 init + decode 成功。Phase 3d.6 可以 deprecate（Vulkan 路線改走 ffmpeg 包裝）。

#### AV1 latency 警告

AV1 雖 throughput 達標，但 NV driver 的 `av1_vulkan` 解碼路徑單 frame wall-clock 80–106 ms（HEVC 是 0.3 ms，差 200×）。Pipeline depth 撐住 120fps throughput，但端到端遊戲延遲偏高。建議使用者預設挑 HEVC，AV1 留作頻寬限制下的選項。NV driver 升級或許可改善。

#### 涉及的檔案 / 改動

- **rebuilt FFmpeg 8.1** in `moonlight-qt/libs/windows/{include,lib}/x64/`：`avcodec-62.dll` (5.2 MB)、`avutil-60.dll` (1.8 MB)、`swscale-9.dll` (2.7 MB) + matching `.lib` + headers (libavcodec 62.28.100)
- **6 個 mingw runtime DLL**：`libdav1d-7.dll` / `libiconv-2.dll` / `zlib1.dll` / `libwinpthread-1.dll` / `libva.dll` / `libva_win32.dll`（總計 +5 MB ship）
- [`build-tools/build_moonlight_package.cmd`](../build-tools/build_moonlight_package.cmd) 加上述 6 個 DLL 到 deploy allowlist
- [`ffmpeg.cpp:1590-1605`](../moonlight-qt/app/streaming/video/ffmpeg.cpp#L1590) + [`:2029-2096`](../moonlight-qt/app/streaming/video/ffmpeg.cpp#L2029) 兩處 SW-force escape：`VIPLE_USE_VK_DECODER=1` 時跳過短路
- [`ffmpeg.cpp:2098+`](../moonlight-qt/app/streaming/video/ffmpeg.cpp#L2098) §J.3.f auto-prefer：`VIPLE_USE_VK_DECODER=1` 時自動走 native h264 / hevc / av1 by name（繞過 av_codec_iterate 不會走到 native 的問題）
- [`plvk.cpp:4492-4540`](../moonlight-qt/app/streaming/video/ffmpeg-renderers/plvk.cpp#L4492) `prepareDecoderContextInGetFormat` override：在 get_format() callback 時 alloc `AVHWFramesContext`（鏡 d3d11va 同名 method），ffmpeg 的 *_vulkan hwaccel 才能 init

#### 後續

- [x] ~~Settings UI toggle 取代 env var~~ — done by b2b7afd（RS_VULKAN preference 直接 auto-trigger，不需要勾選框）
- [x] ~~把 §J.3.e.2.i.8 Phase 3d.6 標 deprecated~~ — 在 priority overview 標記
- [x] ~~4K AV1 baseline benchmark~~ — done in b2b7afd 整合測試（4K120+FRUC+DUAL × 3 codec 全 PASS）
- [ ] AV1 5ms 底線等 NV driver 升級 / AMD / Intel 試或 ffmpeg patch 重 design dedicated_dpb 行為

### §J.3.g FRUC ME 解析度下放 — **NEGATIVE RESULT（2026-05-03，已 revert）**

**原假設（錯）：** Vulkan HW + FRUC + DUAL 在 4K120 卡 77-84 fps，是因為 RTX 3060 mobile FRUC ME compute (block matching @ 480×270 blocks @ 4K) 太慢。預估 ME 下放到 1080p 解析度 4× 加速，總 FRUC time 0.30 + 0.70/4 = 0.47× → 2.1× speedup → 預期 110-120 fps。

**實作完成，未達預期：** 完整實作 NV12→RGB 雙 shader（含新增 `kVkFrucNv12RgbDownsampleShaderGlsl`、bilinear-Y / nearest-chroma downsample）+ buffer 重新 size（`m_FrucMeWidth/Height` + `sizeRGBMe` vs `sizeRGBSrc` 拆分）+ warp shader 改造（PC 加 `meWidthF/meHeightF`、`mvScale = src/me`、`fetchPrev/CurrRGB` 改用 me dims）+ cascade 決策（src ≤ 1440p 不下放、4K 下放 1080p、env `VIPLE_FRUC_ME_RES=540|720|1080`）。

**實測（2026-05-03）：**

| 配置 | 4K120 + FRUC + DUAL fps |
|---|---|
| §J.3.g 前（ME @ 4K） | H.264 77-81 / HEVC 79-84 / AV1 80-84 |
| §J.3.g（ME @ 1080p）| H.264 77-78 / HEVC 75-80 / AV1 不可用（host 卡 720p） |
| §J.3.g（ME @ 720p）| HEVC 75-81 |
| §J.3.g + DUAL=0（FRUC 但單 present）| HEVC 76-81 |

**結論：fps 完全沒變**。隔離測試也排除 DUAL present 是 cost。**ME compute / DUAL 都不是 bottleneck**。

**真正的 bottleneck 推測（未驗證）：**

1. **Warp shader（per-pixel @ 4K）— `§J.3.g` 沒動到** — 8.3M threads × bilinear sample + adaptive blend，估 2-3 ms/frame
2. **Memory bandwidth** — 4K RGB buffer 95 MB × 多次 read/write × 120fps，bandwidth 壓力大
3. **Sync barriers between cmd buffers** — `computeBufBarrier()` 在 4 個 dispatch 之間
4. **Vsync / Pacer 互動** — FIFO_RELAXED 在 GPU 趕不上時 tearing，但 GPU 很慢時 fps 還是受 vsync 拖

**下次定位需要的工具：** GPU timestamp queries（`VkQueryPool` + `VK_QUERY_TYPE_TIMESTAMP`）量出 NV12→RGB / ME / Median / Warp 各自時間。沒這個資料純猜瓶頸只會繼續錯。

**已 revert 的程式碼：**
- `vkfruc.h`: `m_FrucMeWidth/Height/Downsample` 成員拿掉
- `vkfruc.cpp`: cascade decision、buffer sizing 兩 size 變數、`kVkFrucNv12RgbDownsampleShaderGlsl` shader、dispatch 參數改動全部 revert
- `plvk.cpp`: warp shader 的 `meWidthF/meHeightF` PC、`mvScale`、fetch/sampleBilinear 改用 me dims 全 revert

**保留的觀察：**
- `VIPLE_VKFRUC_DUAL=0`（單 present、FRUC compute on）跟 DUAL=1 同 fps — DUAL 不是 cost
- 1080p120 + FRUC 在 §J.3.g 前後也都 ~90 fps，不到 120 — FRUC compute 在 1080p120 也碰邊界
- HEVC 4K decode 0.3-0.6ms / AV1 4K decode 4-7ms — 解碼確實不是瓶頸

**下次接手這條路時：** 不要再先動 ME。**先加 GPU timestamp 量出 per-stage 時間**，看到 warp 真的占大頭，再針對 warp 動（例：dispatch 拆成 8×8 tile + scratchpad、或 warp shader inline bilinear、或 pre-baked MV→pixel-offset 表）。

### 不可動的鐵律 (§J)

1. **Fallback 機制保留** — v1.3.41 的 3-fail fallback、v1.3.44 的 process-lifetime singleton 都不能移除。Phase J 改動失敗時不能讓 user crash。
2. **預設 D3D11 (v1.3.308 起)** — Vulkan 改實驗性次要選項。Phase J.5 真正切 Vulkan 為預設前，新 user 第一次啟動只看到 D3D11；既有 user 設定不被動。
3. **D3D11 renderer 是穩定主線**（不再只是 legacy fallback）— Phase 1.7 系列確認 NV driver 596.36 對 native VK_KHR_video_decode + ONLY mode 有結構性 bug，五個變體都繞不過。D3D11 + DXVA hardware decode 是所有 NV / AMD / Intel Windows 環境的穩定路徑。
4. **每 Phase 都要有 baseline 對比** — 沿用 §I 的 baseline.sh 設計，desktop 版見 `scripts/benchmark/vk_sw_codec_120.ps1` (v1.3.318 起)。
5. **build script 不能無聲拿舊 binary** — `build-tools/build_moonlight_package.cmd` 的 staging step 必須 errorlevel-check rmdir / copy，否則 zombie process 鎖檔會讓 release zip 內含過時 binary（v1.3.299~306 連 8 個 zip 都中招的事故，見 v1.3.307 commit message 5183cee）。

### 已就位的診斷工具（會用到）

- `[VIPLE-FRUC-NCNN]` / `[VIPLE-VKFRUC]` / `[VIPLE-VKFRUC-SW-PROF]` / `[VIPLE-AFTERMATH]` log family
  - SW-PROF 細到 `mem_Y` / `mem_UV` / `fence` / `submit` / `present` 五段 percentile
- `[VIPLE-NVENC] / [VIPLE-NVENC-RATE] / [VIPLE-BCAST-RATE] / [VIPLE-NET]` (server + client 端 packet pipeline rate trace, v1.3.327)
- `VIPLE_USE_VK_DECODER=1` — opt-in Vulkan-first cascade（HEVC 完整 Vulkan-native pipeline）
- `VIPLE_VKFRUC_NATIVE_DECODE=1` — opt-in nvvideoparser feed + native VkVideoSessionKHR
- `VIPLE_VKFRUC_NATIVE_DECODE_ONLY_DANGEROUS=1` — opt-in ONLY mode（NV 596.36 NVDEC device-lost 已知）
- `VIPLE_VKFRUC_NATIVE_AV1_SUBMIT=1` — opt-in AV1 vkCmdDecodeVideoKHR submit（**default OFF**，pending §J.3.e.2.i.8 Phase 3d.6 grey 修法）
- `VIPLE_VKFRUC_VULKAN_DEBUG=1` — VK_EXT_debug_utils + 路 validation 訊息進 SDL log
- `VIPLE_VKFRUC_NO_FRUC=1` — 暫時關 FRUC + dual mode（diagnostic）
- `tools/aftermath_decode/` — standalone CLI 解 `.nv-gpudmp` → JSON
- `scripts/benchmark/vk_sw_codec_120.ps1` — RS_VULKAN + VDS_FORCE_SOFTWARE × {H.264, HEVC, AV1} 在 parametric res/fps 下的 PresentMon + [VIPLE-VKFRUC-Stats] 採集 + pass/fail verdict
- `tools/android_vulkan_probe/` — Android Vulkan queue-family probe (probe.c + probe2.c)，cross-compile via NDK，用來在新硬體 verify §I.D 假設

每個子 phase 的 commit message 用 `vX.Y.Z: §J.N.M — <短摘要>` 格式。

---

### §B-DUMP NCNN-Vulkan RIFE 輸出全 0 — known issue（**deferred 2026-05-07**）

**症狀：** D3D11VARenderer + frucBackend=NCNN 在 RTX 3060 Laptop + NVIDIA
596.144 driver + ncnn 20220729 build vintage 上：
- NCNN init 全部 OK（model load PASS、Vulkan device PASS、phase B.3 shared
  handle export PASS、probe `~25ms inference time` PASS）
- 但 `ex.extract("out0", out_mat)` 回傳的 `out_mat` 是**全 0 fp32 buffer**
- 結果：interp Present 出來的 BMP 全黑（只看得到 HUD overlay 在黑底上）

**已驗：**
1. `matToStaging → CopyResource → OutputSRV → blit` pipeline 完全正常
   （`VIPLE_FRUC_NCNN_DIAG=1` 強制 out_mat 寫 magenta，BMP 確實顯示 magenta）
2. fp16 → fp32 fallback (`VIPLE_FRUC_NCNN_FP16=0` default) 不能修，依然 0
3. SHARED_NTHANDLE / UAV bind flag drop 無效
4. Probe 25ms 是 GPU dispatch 時間，沒驗證 output 正確性 → 沒攔住 cascade

**最可能元兇（沒實際確認）：**
1. `rife.Warp` custom Vulkan layer 的 pack4/pack8 SPIR-V `set_optimal_local_size_xyz`
   在這台 GPU 上挑到產生 0 的 workgroup size
2. ncnn 20220729 跟 NV 596.144 driver 之間 fp16 storage path 有問題
3. RIFE-v4.25-lite model 跟此 ncnn 版本的 op set 微小相容性差

**為什麼 deferred：**
- `§J.3.e.X / §J.3.e.Y` 的手刻 native RIFE Vulkan pipeline 已經 milestone
  完成，準備在 production 路徑取代 NCNN（Final.3b NcnnFRUC integration）。
  花時間修 NCNN-Vulkan dependence 是反方向。
- D3D11 frucBackend=Generic + NVIDIA OF 兩條路在這台 GPU 上 production-ready，
  使用者不需要 NCNN
- 真要修需要：重新 compile rife.Warp Vulkan compute shaders 並強制 fixed
  workgroup size + 寫 ncnn-vulkan output validation harness（pack1 fallback
  vs pack4/pack8 的逐 pixel 比對）→ 1-2 天工程

**現有 mitigation：**
- 第一個觸發時 `[VIPLE-FRUC-NCNN] RIFE output is all-zero` warning log 印
  在 SDL log，告訴 user 切去 Generic backend
- frucBackend cascade 沒攔住但 user 切 backend 即可避開

**清的時候要做的：**
- Final.3b 整合 native RIFE pipeline 進 NcnnFRUC（drop ncnn-vulkan dep）
- 同時把這條從 todo.md 拿掉，§J.3.e.X integration milestone 替代

---

## §M. 多使用者並發 streaming

### §M.1 Phase 1 — Defensive guards + ownership（**✅ SHIPPED in v1.4.12, 2026-05-12**）

修「多人共用 server 時 A 客戶端 (/cancel 或 /launch) 意外中斷 B 正在玩的 streaming session、連帶可能殺到 Steam 遊戲」的 bug。Root cause：Sunshine 三個 HTTPS endpoint (`/launch` `/resume` `/cancel`) 沒做 client identity check，任何 paired client 都能影響別人的 session。

**主要改動：**
- nvhttp.cpp 三個 endpoint 加 caller cert→uuid plumbing (SSL ex_data) + 比對 caller 與 proc_t.owner_uuid，非 owner / 非 admin 友善拒絕 (503/403) 附 owner_name
- proc_t 加 `_owner_uuid` / `_owner_name` 欄位 + `detach()` 方法讓 Steam-source app takeover 走軟交接（不殺 process tree、跳過 undo_cmd 與 display revert）
- named_cert_t 加 `is_admin` flag 持久化於 sunshine_state.json (load_state `get_optional` 預設 false 向下相容)
- confighttp 新增 `/api/current-session` (GET) + `/api/force-cancel` (POST, admin-auth)
- Web UI 首頁加 Current Session 卡片（poll 每 5 秒）+ Force Disconnect 按鈕；troubleshooting 頁面 paired devices 列表加 Admin toggle
- Simple-Web-Server Request class 加 `native_handle()` public method 讓 handler 從 request 取 SSL handle

**Phase 1 out of scope：**
- Sunshine 仍 single-session（一次只跑一個 app；多 user 並發改造延後到 §M.2）
- 不改 Moonlight client；非 admin 想 takeover 仍走 Web UI Force Disconnect（原生 Moonlight 不送 `?takeover=1`）

**完整設計 + 測試 SOP：** `~/.claude/plans/steam-linked-volcano.md`

### §M.1.f Follow-up bug — stale ownership lock 沒 timeout（**Active P0**, 2026-05-13 觀察）

**觀察：** 使用者實機回報「沒有任何人連線，其中一台設備要連線時顯示已有設備佔線」。即 single user 自己也被 §M.1 的 ownership check 誤鎖 — 上次 session 結束後 server 仍記得 `proc::proc.running_owner_uuid()`，新 /launch 走 line 1085 deny path「Server in use by another paired device. Confirm takeover and retry」。

**Root cause hypothesis：**

§M.1 在 `nvhttp.cpp:1063-1095` 的 /launch deny 邏輯只看 `proc::proc.running()` 跟 owner_uuid 比對 caller_uuid。沒檢查 `rtsp_stream::session_count()` 是否真的 > 0。當 client 異常斷線（強殺 app / WiFi drop / 設備睡 / TLS error 之類），server 端：

- `rtsp_stream::session_count()` 可能歸 0（RTSP teardown handler 跑了）
- 但 `proc::proc` 仍持有 owner_uuid（session detach 路徑沒走到 `proc.terminate()` / `proc.detach()` 清空 _owner_uuid）

下個 client /launch 時，`proc.running() > 0 && owner_uuid != caller` → deny。

對照：§M.1 takeover 邏輯需要 client 顯式送 `?takeover=1`（原生 Moonlight 不送），所以日常 single-user 場景沒有 retry 路徑。

**修法方向（待設計階段決定走哪條，可組合）：**

1. **倒空 owner on session-count-zero**：`rtsp_stream` session teardown handler 加 hook，若 session_count drop 到 0 → 呼叫 `proc::proc.clear_owner_uuid()`。最 surgical 但要找對 teardown 點。
2. **/launch 自動 takeover 條件**：若 `proc.running()` 但 `rtsp_stream::session_count() == 0` → 視同 stale，直接允許新 caller takeover 不送 503。比 (1) 更防禦性。
3. **last_activity timestamp**：proc_t 加 `_last_activity_ms` 欄位，rtsp_stream / control / video / input packet 收到時 bump。/launch 收到時若 last_activity > 30s ago (TODO: tune) → auto takeover。實作量最大但對所有 stale 路徑都有效。

**建議實作順序：** 先做 (1) + (2) 組合 — (1) 從根本清掉 stale state，(2) 是防禦性 fallback。(3) 留給 Phase 2 多 session 時更精細的 idle detection 用。

**檔案範圍：**
- `Sunshine/src/nvhttp.cpp` line 1063-1095 (`launch` handler 加 session_count check)
- `Sunshine/src/process.cpp` / `process.h` (`proc_t` 加 `clear_owner_uuid()` method)
- `Sunshine/src/rtsp.cpp` (session teardown hook 呼叫 `clear_owner_uuid()`)

**嚴重性：** 產品 blocker — 不影響 §M.1 多 user 用例的設計初衷（多人共用 server 時 ownership check 仍正確），但每次 single user 異常斷線都要等使用者手動「Force Disconnect via Web UI」或 server restart，日常使用體驗很差。

**驗測 SOP（修完）：**
1. Client A 連 → stream → 強殺 app (swipe out / adb kill)
2. 等 5-10 秒
3. Client A 重新 launch → 預期：自動 takeover、不出現「Server in use by another paired device」
4. Web UI 「Current Session」卡片應顯示 active 或自動歸零
5. Force Disconnect 按鈕仍保留作 admin 手動 override 路徑

### §M.2 Phase 2 — Ubuntu VM 雙 user 並發 streaming 驗測

**目標：** 在 VipleStream 既有的 Ubuntu test VM (`<test-user>@<linux-test-vm>`) 上端到端跑通雙 user 並發 streaming 流程，驗證 Sunshine + per-user systemd + Xdummy/EVDI + PipeWire stack 可行。**不在 Phase 1 改 Sunshine multi-session、不在這階段驗 NVENC 並發 license — 那等實體測試機。**

**前提：** 使用者「盡快生出測試用實體設備」表態 (2026-05-11)。

**可在 VM 上驗（軟編碼路線，無 GPU passthrough）：**
- 雙 user account 建立 (`viple_a`, `viple_b`)
- 每 user systemd `--user` viplestream-server.service（port 47989 / 48089，利用 `network.cpp:197 map_port = config::sunshine.port + offset` 既有機制）
- Xdummy virtual display per user (X server :1 / :2)
- PipeWire per-user audio sink
- udev rule per-user input device
- 兩個 Moonlight client 同時連 → libx264 軟編 → 走通 RTSP / control / video / audio / input 全 path
- 兩個 streaming session 之間是否互相干擾（display / audio / input 串擾）

**必須等實體機（有 NVIDIA GPU passthrough 或 spare hardware）：**
- NVENC 並發 2 session frame rate / latency / VRAM 占用 (RTX 5060 Ti, driver R522+ 支援 5 concurrent NVENC)
- NVIDIA Linux driver Wayland 穩定性 (driver 545+ explicit sync GA)
- 實際遊戲 Proton 相容性 / HDR / DLSS / RT / anti-cheat (areweanticheatyet.com)

**進度：** v1.4.12 ship 後等 §M.1 取得使用者實測 sign-off (Task 11 Scenario 1-4) 再開工。

**Phase 2 預期產出：**
- `docs/multi-user-linux.md` setup SOP
- `scripts/setup_second_user_linux.sh` 自動化雙 user setup（不入 git，per `reference_linux_build_pipeline.md`）

### §M.3 Phase 3 — Linux host migration（待 §M.2 通過 + 實體機到位）

延後規劃，視 §M.2 結果 + 使用者實體機到位後評估：distro 選擇 (Bazzite KDE NVIDIA / Ubuntu LTS / 其他) / X11 vs Wayland 路線 / Proton 相容性實測 / 是否真執行 host OS migration。

---

## §H. AMD client + 解析度誠實化

§H.4.send / §H.4.osd 已 ship in v1.4.14 → v1.4.20 (詳見上面 patch series 條目)。剩下：

### §H.4-perf VkFrucRenderer AMD draw-time perf fix

**現況：** OSD 「Frame draw time (含 V-sync; 60Hz 下限 ~16.7ms)」標籤已說明 60Hz 為什麼一張 frame 至少 16.7ms。AMD client 上 m_FrucMode=false 的真實 GPU work 仍 ~13ms（NV 同 path 通常 ~2-4ms），明顯偏高。

**何時清：** 需要 user 提供 `[VIPLE-VKFRUC-GPU-PROF]` + `[VIPLE-VKFRUC-SW-PROF]` 5-10 行 log 才能精準改 — 不知熱點落在 nv12rgb / descriptor update / pipeline barrier / present-block 哪段就盲改 9843 行的 hot path 太冒險。

**修法分支：**
- Step 2A `m_FrucMode=false` fast-path — 跳過 FRUC-only descriptor updates + cmd records（ME/median/warp/copy）
- Step 2B 縮減冗餘 GPU pass — sampler chroma reconstruction LINEAR→NEAREST、tone-mapping `m_HdrActive` 守衛
- Step 2C swapchain present mode — image_count ≥ 3 或 MAILBOX 避免 driver-side stall

---

## §N. In-stream 雙向檔案傳輸

§N.1 / §N.2 / §N.3 / §N.4 / §N.4b 已 ship in v1.4.14 → v1.4.32 (詳見上面 patch series 條目)。剩下：

### §N.5 moonlight-android FileTransferClient runtime verify

**現況：** APK 建置 + JNI hooked + Game.java lifecycle 通；`MediaStore.Downloads` scoped-storage 路徑寫對；但實機未在串流 session 中真正跑過 Send/Receive flow，可能有 Pixel 5 / Pixel 9 specific Android quirks（譬如 backgrounding 中 OkHttp 連線維持、power manager 影響 polling）。

**何時清：** 下次 Android client 整段 streaming session 驗測時順帶過一輪 Send 小檔 → 完成後再回頭看 Receive listing UI 是否需要任何 Android 端的調整（譬如 listing 預設路徑該不該是 `MediaStore.Downloads.EXTERNAL_CONTENT_URI` 而非 host-style absolute path）。

### §N.6 moonlight-qt Cancel hotkey (Ctrl+Alt+Shift+X)

**現況：** `FileTransferClient::cancelCurrent()` 已實作，會 abort reply + POST `kind=canceled` 到 server。但沒接到 SDL session keystroke handler，所以 user 在 stream 中無法用 hotkey 取消。目前的替代方案：透過 Web UI 重新整理（清掉狀態）或者結束 stream（觸發 abort_all）。

**何時清：** 找到 session.cpp 中處理 Ctrl+Alt+Shift+* combo 的位置，把 X 也綁進去呼叫 `m_FileTransferClient->cancelCurrent()`。預估 ~10 行 code。

### §N.7 Sunshine Linux fs_picker (zenity)

**現況：** Windows server 端 `IFileOpenDialog` (COM) 已實作。Linux server 端「Send to client」流程觸發 file picker 還沒有實作 — `Sunshine/src/platform/linux/fs_picker.cpp` 該檔尚未建立，呼叫會回 nullopt → tray callback 直接 return（沒檔可送）。

**何時清：** Linux server 真實使用者出現時。用 `zenity --file-selection` subprocess 做最簡解（跨 GTK/Qt/各 DE 都行），預估 ~80 行。

**Receive 方向 Linux server 已可用：** 沒走 native file picker；server 端只負責接 client 上傳的 blob 寫到 `~/Downloads`，路徑解析跟 Windows 共用 `manager::downloads_dir()`（XDG_DOWNLOAD_DIR / HOME/Downloads fallback）。

---

## §K. Linux build pipeline

GitHub release 規範改成「每個 release 必 ship 完整三件、全同版號」（見 `CLAUDE.md` Release 規範）後，下一步把 Linux artifact 也加進完整 ship。

### §K.1 Linux x86_64 兩端（**SHIPPED 2026-05-05 in v1.3.337**）

**Artifact 已正式進入 release：**
- `VipleStream-Client-1.3.337-linux-x64.AppImage`（59 MB，從 `moonlight-qt/scripts/build-appimage.sh`，三段式手工組裝繞過 linuxdeployqt-on-noble 限制）
- `VipleStream-Server-1.3.337-linux-x64.deb`（9.4 MB，從 `Sunshine/scripts/linux_build.sh` + CPack DEB generator + `--skip-libva` workaround）

**完成的 source fix（一次性 rebase 後 land 進 main）：**
- Sunshine `src/stream.cpp:1054` — `std::max<long long>` 顯式 template
- Sunshine `src/relay.cpp` / `src/stun.cpp` — `closesocket` macro 改 `(::close(fd))` 避 unqualified lookup
- Sunshine `src/nvenc/nvenc_base.cpp` — NVENC API v12 vs v13 dual-support，`#if NVENCAPI_MAJOR_VERSION >= 13` gate
- Sunshine `packaging/linux/` — 5 個 `dev.lizardbyte.app.Sunshine.*` rename 為 `app.viplestream.server.*`
- Sunshine `scripts/linux_build.sh` — `SUNSHINE_EXECUTABLE_PATH=/usr/bin/viplestream-server`、`CMAKE_PREFIX_PATH=/usr/local`、`SUNSHINE_ENABLE_VAAPI=OFF`-on-skip-libva
- Sunshine `.gitattributes` — 強制 LF on `src_assets/linux/misc/{postinst,preinst,prerm,postrm}` 避 dpkg `#!/bin/sh\r` ENOENT
- Sunshine `src_assets/linux/misc/postinst` — auto-enable + `loginctl enable-linger` for `$SUDO_USER`，配 `postrm` 清 dangling symlink
- Sunshine `src_assets/linux/assets/apps.json` — drop `Low Res Desktop` (xrandr HDMI-1 hardcode) + `Steam Big Picture` (setsid dep)
- Sunshine `src/platform/linux/wayland.cpp` — `wl_log_set_handler_client` libwayland EPIPE 防護 + dispatch() defensive
- Sunshine `app-app.viplestream.server.service.in` — `Restart=always` + `StartLimitBurst=30`，client 斷線後 8 秒自動回來
- moonlight-qt `app/streaming/video/ffmpeg-renderers/plvk.{h,cpp}` — ncnn 整段 `#ifdef VIPLESTREAM_HAVE_NCNN` 隔離 + Linux 用系統 ncnn `/usr/local/lib/libncnn.so`
- moonlight-qt `vkfruc.cpp` — Linux POSIX 等價 `_wfopen` / `strncpy_s` / VK_KHR_X*_SURFACE 字串字面量
- moonlight-qt `3rdparty/nvvideoparser/nvvideoparser.pro` — `*-msvc { /arch:AVX2 } else { -mssse3 -mavx ... }` gate
- moonlight-qt `scripts/build-appimage.sh` — qmake6-on-noble moc race workaround：build 前先 `make compiler_moc_source_make_all`
- moonlight-qt `app/deploy/linux/com.piinsta.{desktop,appdata.xml}` — appdata id 改 reverse-DNS + Exec= 修正

**§K.1 Linux pipeline 視為 done。** `release_full.cmd` 一鍵化包進 Linux 兩端是後續 nice-to-have。

### §K.4 Wayland XDG portal teardown root-cause（追隨 §K.1，deferred）

§K.1 ship 了 systemd `Restart=always` 緩解 client 斷線後 server 死掉的問題（option A），但 libwayland EPIPE 從何處發出未根因。需可重現 streaming 環境（VM 沒 GPU 不能驗 encoder + portal grab combo）。Pi 5 / 真 GPU Linux 機驗到時再修進 `wayland.cpp` / `portalgrab.cpp` 的 EPIPE 路徑。

### §K.2 Raspberry Pi 5 client（aarch64，等 §K.1 通後）

**目標 artifact：**
- `VipleStream-Client-X.Y.Z-rpi-aarch64.deb`（Pi 5 + 64-bit RPi OS Bookworm 用）

**為什麼選 Pi 5、放掉 Pi 3 / Pi 4 / armhf 32-bit：**
- Pi 5 自家 video block + V3DV mesa Vulkan + Bookworm 64-bit 是 RPi 主流現在式
- Pi 3 / Pi 4 32-bit RPi OS 走 MMAL legacy 路徑，現代用戶幾乎不在這條，不值得三條 ARM target 都 ship
- Pi 4 64-bit Bookworm 走 V4L2 + DRM/KMS，跟 Pi 5 部分共用，但 GPU 跑 Vulkan 邊緣，FRUC 補幀絕對沒救

**Pi 上的 fork 改動相容性：**
- FRUC backend 全 disable（Pi 5 V3DV 跑不動 4K compute，1080p 也勉強）—— 退回 vanilla Moonlight 體驗
- DirectML / NCNN-Vulkan / Aftermath 全 win32 only，Pi 自動 skip
- core streaming + DRM/KMS render + V4L2 / Vulkan HW decode 走上游現成 path

**Build pipeline 候選（待選）：**
1. **GitHub Actions arm64 runner** —— 現在 GH-hosted arm64 runner 已 GA，但 build time 跟 cost 待測
2. **Native build on Pi 5** —— 一次 setup，每次 release SSH 觸發，build time ~30 min；可靠
3. **QEMU cross-compile (aarch64-linux-gnu-gcc + Qt sysroot)** —— 設 sysroot 痛苦，但本機 build time 接近 native x86 速度
4. **Docker buildx multi-arch** —— `Sunshine/docker/ubuntu-24.04.dockerfile` 改 cross-build，可一次出 amd64 + arm64

**進度：** 待 §K.1 通過再開工。

### §K.3 macOS client（optional，hw-pending）

上游 `build-win-mac.yml` GitHub Actions matrix 含 macOS-15 + Qt 6.10.2 + create-dmg；產出 `Moonlight-X.Y.Z.dmg`。本地 fork 沒 Mac 機器驗測，**won't-add** 直到使用者有 mac dev 環境。

---

## 如何使用這份清單

- 動某條相容性債 → **不要**單獨 commit「改名」，要一起 ship migration code + release notes
- 完成的事項移到 git log（`vX.Y.Z: §X.N — short summary` commit message 是唯一紀錄來源）
- 新發現的債照 §A.N / §J.N 風格加進來
- 真的不打算做的（§A.7）就明確寫「不該清」並給理由
