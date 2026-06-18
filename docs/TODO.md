# VipleStream — 待辦清單

只列**尚待 / 進行中 / 等硬體 / 等驅動**的條目。
已完成項目 → git log。歷史背景 → git log 與 docs/。

---

## 優先級總覽

| 優先級 | 條目 | 待做事項 |
|---|---|---|
| **Active (verify)** | **§β.11.b** warp edge MV threshold | Settings UI slider (2-20, default 8) 已上線 v1.4.195；setting 實機 confirm 生效 (2026-05-23, registry vkfrucEdgeMvThreshold=8 + ctor log)；剩主觀畫質比較：keep 8 / 試 4（更多邊緣保護）/ 試 12（更 smooth）|
| **Active (Phase 2.1 完成 / v1.5.249 61f8b0fd)** | **§FRUC-VALIDATE** FRUC 科學驗測方案（科學數據優先、體感降第二線）| drop-and-reconstruct vs GT 取代體感。**Phase 1 已 commit (4833c02d / v1.5.247)：** app 內 `fruc-offline` CLI harness（走真 renderFrame(AVFrame*) + §B-DUMP）+ vkfruc.cpp SW-dump null-timeline-sem crash 修。**Phase 2.1（2026-06-17，scripts/ 本機）強結論：** 結構化內容（make_shapes_pan 硬邊+平滑底剛性平移）+ 公平 NV12 控制（用引擎自己 dump 的 real 幀，修 measurement asymmetry）+ paired bootstrap CI + 驗過的 oracle（warp 正確性 ✓、光流改用閉式解因 Farneback 失效）+ gate 敏感度負對照下，**RIFE@256 在全部 4 標準指標（PSNR/SSIM/IE/LPIPS）+ temporal 一致性上統計顯著勝過 naive-blend，且優勢隨運動難度成長**（slow 36px→fast：PSNR +2.73→+4.08）。**誠實：** block-match-8 在 12-36px 全域運動誠實輸 blend（標準指標），只在 M3 edge-IE（獎勵 source-res 銳利邊）贏；inferDim 是關鍵 cross-engine 共變量（128→256 翻 3 指標）。tooling：fruc_metrics/decimate/compare_dump/oracle_pass/inject_regression/run_validate_round（gitignored，跑 .venv）。**seq-VMAF 已加**（RIFE@256 68 vs blend 42）。**NVOF 改走真實串流驗測成功**（取代 SW renderer 手術——判定那不是小修）：register win-builder + .226 host，throwaway client 串流 TIER=5 → log 證 NVOF execute + 餵 warp（chainBusy ~1.6ms 最快）。**6 引擎覆蓋：** Vulkan 家族 3 引擎完整（block-match/RIFE GT-quality + 全 3 真路徑 perf，NVOF 解鎖）；**D3D11 家族（Generic/NVOF/DirectML）此自動化 session 不可達**（client 硬選 VkFruc，需互動桌面）。**剩：** D3D11 家族（互動桌面）、L1 端到端 GT-quality、跨 build σ。|
| **Active (verify, hw-blocked)** | **§FRUC-XPLAT** 非-NVIDIA RIFE 跨平台啟用 | **已 ship v1.5.249**（1b213e8f / ca05f64b / e4523449 / 7aa47d62）：非-NVOF 設備直起 RIFE@128 + 非-NVIDIA 自動啟用 native RIFE（vendor-gate 綁 hardware 0x10DE，避開 NV 的 device-lost）+ inferDim cap 128 + overlay 顯示真實補幀引擎名。AMD RADV live stream 已證 RIFE 真 engage（frucMode=1 / rifeNative=1 / T3 穩定）惟當時跑 inferDim=256（cap commit 前）。**剩：** inferDim=128 在 AMD 的經驗驗測（offline + 串流 + fps + 補幀數）卡在 **AMD GPU errno -13**（headless 筆電蓋著→logind seat-ACL 缺→amdgpu ACCEL_WORKING 失敗→llvmpipe）。決定性值已 deterministic+committed |
| **Planned (designed, 未動工)** | **§FRUC-GENERIC** 通用 block-match 畫質改良 | plan-mode 計畫已成形：分階段 in-place 改 Vulkan block-match（1A 邊緣不退 crossfade→occlusion-gated 單向 warp／1B in-shader sub-pixel parabolic refine／1C bilinear→Catmull-Rom），每步用 §FRUC-VALIDATE 離線 GT harness 客觀量、贏過 naive blend 即停。根因已讀碼確認（warp 邊緣三層 fade 退回 crossfade＝在邊緣做 naive blend，是輸 blend 主因）。零新 dispatch、binding 不變、純 fp32 跨廠商 |
| **Active (verify + polish)** | **§Q** MP-QUIC 多路徑網路聚合 | Phase 5 全 Sprint + §Q.path-recovery 完成 (v1.5.133)。Fix L–Q.5 (v1.5.192–202) 完成 failover/failback + 遠端單路徑韌性：Fix P IDR-QUIC-FIRST、Fix Q RTT 自適應 jitter timeout (1.5×RTT 夾 10-40ms)、Fix Q.2 §Q-STALE headroom gate（修 cwnd 健康卻丟光 video 的死亡螺旋）、Fix Q.3 FRUC throttle startup grace（修啟動 5s 模糊）、Fix Q.4 顯示器 graceful fallback + NVENC vbvInitialDelay（修啟動畫質）、Fix Q.5 lateAfterSkip/fecRedundantLate 診斷分離。**已驗證：** server FEC ✅、per-path queue ✅、0-RTT ticket ✅、1080p60 穩定 ✅、Audio QUIC ✅、Jitter buffer ✅、Ethernet failover/failback ✅ (v1.5.196)、遠端 WARP/Tailscale 單路徑 ✅ (v1.5.199 §Q-STALE 修後)、**Android moonlight-common-c 全量對齊 + Pixel 5 實機 MP-QUIC 串流驗證 ✅** (v1.5.201-202，lateAfterSkip=0/fecRedundantLate 實機確認)。**待做：** 壓力測試 1440p120；遠端高碼率若仍吃緊再評估 ABR（自適應 bitrate）|
| **Active (verify)** | **§SLIM** release zip 瘦身 (106→48 MB) | phase 1-4 已 ship。**Lazy fetch 兩路徑皆已接 ModelFetcher（2026-05-29 程式碼確認）：** NCNN (ncnnfruc.cpp → ensureRifeModelDir) + Vulkan Native RIFE (vkfruc.cpp:9324 在 RifeNativeExecutor::initialize 前呼叫 ensureRifeModelDir，回傳 empty→block-match fallback 不 crash)。`ensureRifeModelDir` 已 expose 到 modelfetcher.h 兩端共用。**剩（環境受限）：** 無 VC++ Runtime 的乾淨 Win10 系統實測 missing vcruntime140 提示 |
| **Deferred (hw-bound)** | **§B Phase B** HEVC D3D11VA → Vulkan composite | Code 已 ship v1.4.184-185；阻塞：AMD 780M 測試機 HEVC D3D11VA 本身不可用。需換另一台 D3D11VA HEVC HW decode 可用的 AMD 機器驗 B7 import + B9 FRUC chain |
| **Active (test pending)** | **§B-NVOF autotier** NVOF 成為 NV 最佳 tier | Code 已 ship v1.4.117-138；待使用者 PixArk 20+ 分鐘實測，看 NVOF-PROF drop% 是否接近 0%、chain_mean 是否穩定 < 2ms。若 OK → reapply early-kickoff + NVOF 列 NV best tier；若 drop% 高 → 需 Option E skip 機制 |
| **Active (follow-up)** | **§R2 PASSIVE FRUC** ratio controller | v1.4.169-186 已 ship ratio alignment gate + extreme floor。**T2→T0 大幅跳降 ✅** (2026-05-23：gate 後僅 stream 初期 decode-warmup 1 次，立刻 promote 接住)。**display Hz=0 fallback ✅** (2026-05-29 程式碼確認 ffmpeg.cpp:2874/2909/2942 全守：Hz=0→recvPct fallback + 120Hz budget 預設 + 跳過 server-fps 請求但本地 ratio 仍調，無除零，不需補)。剩：進 2x 後 server recv ~180→~140 是否仍有（alignment gate 應已消除，待串流複查）|
| **Active (verify)** | **§M.parity Android** wire/UX | v1.4.160 ship W1-W5+U6+U7。**Pixel 5 v1.5.202 實機驗 (2026-05-29)：** 串流 .226 桌面成功，codec 協商正常（c2.qti.avc/hevc.decoder.low_latency 選用）→ **W1 server-cap 交集邏輯確認 live**。`[VIPLE-PARITY]` 遮蔽 log 為條件式（僅當 server SCM 排除 client codec 時才印）；本次 Pixel5 codec ⊆ .226 caps 故未觸發＝正解（無需遮蔽）。剩 W2-W5/U6-U7 UX 流程主觀驗 |
| **Active (verify)** | **§K.dd.revert.1 / §K.dd.fallback** display device | v1.4.156 ghost-check + v1.5.201 §K.dd.fallback（configure_display apply 前用 enumerate_devices 驗證 output_name 存在；不存在→跳過 DD 設定 + 用預設顯示器續行，避免 Windows 拓樸 API 卡死）已 ship。.226 指定顯示器曾卡死服務 (v1.5.200 期間，log 停寫 35 分鐘)，已暫設 `disabled` 解卡。待重設 `ensure_only_display` + 指定有效顯示器，確認 fallback 生效不卡死 |
| **Active (P0)** | **§N.5.bug** Android 檔案傳輸 SSL 錯誤 | 待使用者在 Android 重現 + 拿 server log，確認錯誤在哪個階段送出 |
| **Active (verify)** | **§N.5** Android FileTransferClient runtime | 待實機在串流 session 跑 Send/Receive flow，確認 Pixel 5 Android quirks |
| **Active (long-running)** | **§J.3.e.2.i.8 Phase 2.5** FRUC native source | 殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Active (β.10)** | **§J.3.e.X Path β.10** Linux/AMD/Intel 覆蓋 | §β.12.fix v1.4.193 PASS；剩：視覺品質主觀確認（tiled Conv vs coopmat）+ Intel iGPU 驗測 |
| **Deferred (driver-bound)** | **§K.4** Wayland portal teardown | `Restart=always` 緩解已 ship；需可重現串流環境（有 GPU 的 Linux 機）才能根治。dev/cli-quic-linux branch hygiene fix：startup portal warning 的雙 register noise 改用 QT_DESKTOP_FILE_NAME env var 預設 + Linux 上 gate 掉 app.setDesktopFileName()——但 Qt 內部仍會 double-register，warning 沒完全消，且這是 client startup noise，跟 server-side PipeWire portal teardown 是不同層問題 |
| **Medium** | **§K.2** Raspberry Pi 5 client (aarch64) | 待 §K.1 通過後再開工；FRUC 全 disable |
| **Low** | **§J.3.e.Y 4Y.5b** native RIFE activation | 256×256 chain +1.7ms 負收益，revert；重做要先解 §β.5.3 D 全套 |
| **Low** | **§H.4-perf** VkFrucRenderer AMD draw-time | m_FrucMode=false ~13ms 偏高；需使用者提供 GPU-PROF log 才能精準改 |
| **Low** | **§A.2 / §A.8** WiX installer / 內部命名 | 沒用 MSI 出貨，優先級極低 |
| **Low** | **§D** HelpLauncher URL | docs/ 已寫；等 doc site 立起來再換 URL |
| ❌ **Won't-fix** | **Linux mouse scale** | 使用者明確放棄，維持現狀 |
| ❌ **Won't-fix** | **NVOF SDK buffer 輸出** | SDK 硬體限制，無法做 buffer 變體 |

---

## 各章節待辦補充

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
| Q.r13 | 驗測基礎設施 | **P2** | host worker 包補 net-throttle op + COWORK_TASK_SPEC 注入（collect 參數化）；failover_test_auto.ps1 加 log 收集/斷言/IP 參數化 |
| Q.r17 | §SC-HID driver 簽章重建 | ✅ **完成（working tree，待 commit）** | **2026-06-18 git/簽章核實：** §SC-QUEUE-SERIAL-FIX 已編入 DLL（DLL 6/12 23:37 比 source 20:36 新）+ `Get-AuthenticodeSignature`=**Valid**（CN=VipleStream SC HID Driver，到 2036）。**build 防呆已加+測試：** 守門腳本放**追蹤位置** `Sunshine/src/platform/windows/sc_hid_driver/check_schid_driver_fresh.ps1`（版控、跨機；精準只追 `VipleSCHid_Driver.*`，排除 server-side `VipleSCHid.cpp` 避免誤判），build_sunshine.cmd 打包前呼叫它（source 比 DLL 新→`exit /b 1` 中止打包，鐵律 4）。**注意：** build_sunshine.cmd 與 build-tools/ 本身 gitignore（local-only by design），故「呼叫點」留在本機、不入 repo；守門邏輯本身已入 repo。驗測：5 case 單測（fresh/stale/missing/server-edit）+ 批次整合（fresh→continue、stale→abort）+ 搬移後複測全 PASS、現況不誤判 |
| Q.r16 | review batch 2 已知殘留（驗測觀察項） | **P3** | **驗測 PASS (2026-06-16)**：L1 30s smoke ✅、L2 R.3 firewall ✅、L3 kill+reconnect×3 ✅。殘留觀察項（不阻 commit）：①S5 stats 快取 idle 凍結（§K.10 log 顯示舊值，診斷限定）②invokeRecvHandler heap alloc per datagram（可改 shared_ptr+atomic swap）③LiGetEstimatedRttInfo 無鎖讀 peer（pre-existing TOCTOU）④A1 tick 在 video 全死但 control 活時仍 ramp（既有假設）|

備註：standalone cmake configure picoquic（除錯 picoquic 本身）需 pkg-config + OpenSSL dev headers；一般走 build script 不需。

### §SLIM Vulkan Native RIFE fix

`ncnnfruc.cpp:61` 的 `ensureRifeModelDir(modelDir)` helper 是 anonymous namespace
static。要：

1. 把 helper expose（搬到 modelfetcher.h / 自己拉成 shared util）
2. VkFrucRenderer 對 `RifeNativeExecutor::initialize(opts)` 的 caller 端先呼叫
   `ensureRifeModelDir(opts.modelDir)`，把回傳的 cache dir 取代 `opts.modelDir`
3. 驗測：清 `%LOCALAPPDATA%\\VipleStream\\fruc_models\\` → registry vkfrucEnableNativeRife=true → stream → 預期看 [VIPLE-MODELFETCH] download flownet.bin 5-10s + SHA-256 OK + RIFE init success

### §R2 PASSIVE FRUC 剩餘觀察項目

- ~~autotier T2→T0 大幅跳降是否還出現~~ **2026-05-23 confirmed：alignment gate 後只在 stream 初期 decode-warmup 出現 1 次，立刻 promote 接回** ✅
- ~~display Hz=0 legacy renderer fallback 是否需補~~ **2026-05-29 程式碼確認：不需補。** ffmpeg.cpp ratio controller 對 displayHz 全程守 `>0`：Hz=0 時 `recvPctOfDisplay` fallback 到 recvPct (2874)、`frameBudgetMs` fallback 120Hz=8.33ms (2909)、server-fps 變更請求被 `if (displayHz>0 ...)` 跳過 (2942) 但本地 `setEffectiveFrucRatio` 仍正常 (2950)，無除零 ✅
- 進 2x 後 server recv 從 ~180 掉 ~140 是否仍有（alignment gate 應消除，待串流複查）

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
