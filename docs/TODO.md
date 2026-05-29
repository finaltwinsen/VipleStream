# VipleStream — 待辦清單

只列**尚待 / 進行中 / 等硬體 / 等驅動**的條目。
已完成項目 → git log。歷史背景 → git log 與 docs/。

---

## 優先級總覽

| 優先級 | 條目 | 待做事項 |
|---|---|---|
| **Active (verify)** | **§β.11.b** warp edge MV threshold | Settings UI slider (2-20, default 8) 已上線 v1.4.195；setting 實機 confirm 生效 (2026-05-23, registry vkfrucEdgeMvThreshold=8 + ctor log)；剩主觀畫質比較：keep 8 / 試 4（更多邊緣保護）/ 試 12（更 smooth）|
| **Active (verify + polish)** | **§Q** MP-QUIC 多路徑網路聚合 | Phase 5 全 Sprint + §Q.path-recovery 完成 (v1.5.133)。Fix L–Q.5 (v1.5.192–202) 完成 failover/failback + 遠端單路徑韌性：Fix P IDR-QUIC-FIRST、Fix Q RTT 自適應 jitter timeout (1.5×RTT 夾 10-40ms)、Fix Q.2 §Q-STALE headroom gate（修 cwnd 健康卻丟光 video 的死亡螺旋）、Fix Q.3 FRUC throttle startup grace（修啟動 5s 模糊）、Fix Q.4 顯示器 graceful fallback + NVENC vbvInitialDelay（修啟動畫質）、Fix Q.5 lateAfterSkip/fecRedundantLate 診斷分離。**已驗證：** server FEC ✅、per-path queue ✅、0-RTT ticket ✅、1080p60 穩定 ✅、Audio QUIC ✅、Jitter buffer ✅、Ethernet failover/failback ✅ (v1.5.196)、遠端 WARP/Tailscale 單路徑 ✅ (v1.5.199 §Q-STALE 修後)、**Android moonlight-common-c 全量對齊 + Pixel 5 實機 MP-QUIC 串流驗證 ✅** (v1.5.201-202，lateAfterSkip=0/fecRedundantLate 實機確認)。**待做：** 壓力測試 1440p120；遠端高碼率若仍吃緊再評估 ABR（自適應 bitrate）|
| **Active (verify, partial bug)** | **§SLIM** release zip 瘦身 (106→48 MB) | phase 1-4 已 ship。**NCNN backend lazy fetch path 確認接通** (ncnnfruc.cpp:1063/2714 → ensureRifeModelDir → ModelFetcher)。**⚠️ Bug：Vulkan Native RIFE 路徑沒接 ModelFetcher** — rife_native_vk.cpp 4 處 (line 3669/4998/7413/7705) 直接 modelDir+/flownet.bin 載入；2026-05-23 實機驗：cache 空時 RifeNativeExecutor init failed → fallback block-match path (鐵律 OK 不 crash)，但 lazy fetch 沒生效。**Fix：** VkFrucRenderer 對 RifeNativeExecutor::initialize 的 caller 端先 ensureRifeModelDir(opts.modelDir)。剩餘原項：無 VC++ Runtime Win10 系統實測 missing vcruntime140 提示 |
| **Deferred (hw-bound)** | **§B Phase B** HEVC D3D11VA → Vulkan composite | Code 已 ship v1.4.184-185；阻塞：AMD 780M 測試機 HEVC D3D11VA 本身不可用。需換另一台 D3D11VA HEVC HW decode 可用的 AMD 機器驗 B7 import + B9 FRUC chain |
| **Active (test pending)** | **§B-NVOF autotier** NVOF 成為 NV 最佳 tier | Code 已 ship v1.4.117-138；待使用者 PixArk 20+ 分鐘實測，看 NVOF-PROF drop% 是否接近 0%、chain_mean 是否穩定 < 2ms。若 OK → reapply early-kickoff + NVOF 列 NV best tier；若 drop% 高 → 需 Option E skip 機制 |
| **Active (follow-up)** | **§R2 PASSIVE FRUC** ratio controller | v1.4.169-186 已 ship ratio alignment gate + extreme floor。**T2→T0 大幅跳降實機觀察 (2026-05-23)：** alignment gate ship 後僅在 stream 初期 1 次出現 (latency=409.31ms 屬 decode-warmup)，立刻被 promote 機制接住 (T0→T2→T3→T4)，stream 穩定階段未再出現 ✅。剩：display Hz=0 legacy renderer fallback 是否需補 |
| **Active (verify)** | **§M.parity Android** wire/UX | v1.4.160 ship W1-W5+U6+U7；Pixel 5 待 install + adb logcat 確認 `[VIPLE-PARITY]` log 真實生效 |
| **Active (verify)** | **§K.dd.revert.1 / §K.dd.fallback** display device | v1.4.156 ghost-check + v1.5.201 §K.dd.fallback（configure_display apply 前用 enumerate_devices 驗證 output_name 存在；不存在→跳過 DD 設定 + 用預設顯示器續行，避免 Windows 拓樸 API 卡死）已 ship。.226 指定顯示器曾卡死服務 (v1.5.200 期間，log 停寫 35 分鐘)，已暫設 `disabled` 解卡。待重設 `ensure_only_display` + 指定有效顯示器，確認 fallback 生效不卡死 |
| **✅ Completed** | **§K.linux VAAPI→Vulkan bridge** | K.3 + §β.12.fix PASS v1.4.188-193：Vega 10 VAAPI + RIFE + FRUC dual-present end-to-end 通，frame#720 無 crash |
| **Active (P0)** | **§N.5.bug** Android 檔案傳輸 SSL 錯誤 | 待使用者在 Android 重現 + 拿 server log，確認錯誤在哪個階段送出 |
| **Active (verify)** | **§N.5** Android FileTransferClient runtime | 待實機在串流 session 跑 Send/Receive flow，確認 Pixel 5 Android quirks |
| **Active (post-v1.4.12)** | **§M.2** 雙使用者並發 streaming 驗測 | Ubuntu VM 軟編碼路線可先跑；NVENC 並發等實體機 |
| **Active (long-running)** | **§J.3.e.2.i.8 Phase 2.5** FRUC native source | 殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Active (β.10)** | **§J.3.e.X Path β.10** Linux/AMD/Intel 覆蓋 | §β.12.fix v1.4.193 PASS；剩：視覺品質主觀確認（tiled Conv vs coopmat）+ Intel iGPU 驗測 |
| **Deferred (driver-bound)** | **§K.4** Wayland portal teardown | `Restart=always` 緩解已 ship；需可重現串流環境（有 GPU 的 Linux 機）才能根治。dev/cli-quic-linux branch hygiene fix：startup portal warning 的雙 register noise 改用 QT_DESKTOP_FILE_NAME env var 預設 + Linux 上 gate 掉 app.setDesktopFileName()——但 Qt 內部仍會 double-register，warning 沒完全消，且這是 client startup noise，跟 server-side PipeWire portal teardown 是不同層問題 |
| **Medium** | **§K.2** Raspberry Pi 5 client (aarch64) | 待 §K.1 通過後再開工；FRUC 全 disable |
| **Low** | **§J.3.e.Y 4Y.5b** native RIFE activation | 256×256 chain +1.7ms 負收益，revert；重做要先解 §β.5.3 D 全套 |
| ✅ **Completed** | **§N.7** Sunshine Linux fs_picker (zenity) | v1.4.41 ship：src/platform/linux/fs_picker.cpp 116 行 zenity subprocess + cancel/exit-code/CRLF handling，已掛 cmake build (cmake/compile_definitions/linux.cmake:108) |
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

### §Q Phase 5 完成狀態（2026-05-26, v1.5.126）

全部 5 Sprint 實作完成 + LAN 實機驗測通過。

| Sprint | 工作 | 狀態 |
|---|---|---|
| 1 | 5e Android JNI bridge + 5f 0-RTT ticket 持久化 | ✅ done + server 跨重啟驗證 |
| 2 | 5a Jitter buffer（sliding-window reorder, 10ms timeout） | ✅ done + client log 確認（delivered=41817 reordered=1 dropped=0 timedOut=7） |
| 3 | 5b FEC（4+2 Reed-Solomon, server encode / client decode） | ✅ server encode 驗證 + client decode 就緒（LAN loss=0% 無觸發機會，屬預期） |
| 4 | 5c re-enable multipath（standby/available, ECF scheduler） | ✅ done |
| 5 | 5d per-path datagram queue（picoquic fork API） | ✅ done + §5d.fix backup path exclusion |

**LAN 驗測結果（v1.5.128, 1080p60 40Mbps）：**
- `pendingPeak=0`（§5d.fix 前 2160）
- `p0 RTT=0.5ms 18Mbps loss=0.0%` — active path 穩定（3 subflow: Ethernet + Tailscale standby + Wi-Fi）
- `p1 tx=2KB` — Tailscale standby 正確閒置
- `FEC 4+2 RS ratio=1.5x` server encode confirmed
- `Ticket key loaded` 跨 service 重啟成功
- `§5a Jitter[AUDIO]: delivered=41817 reordered=1 dropped=0 timedOut=7` — client jitter buffer ✅
- `AUDIO: 13340 dgrams 4749KB 0.9Mbps` — audio QUIC 傳輸 ✅（桌面有音訊時）
- `§K.16 cwnd LOW` 降頻 ✅（5s rate limit，warning 從數百次降至 3 次）

**WiFi 斷線 failover 測試（v1.5.130, 1080p60, 3 subflow）：**
- ✅ **Failover 成功：** Disable-NetAdapter "Wi-Fi 2" → path[2] DEAD loss=100%，stream 靠 Ethernet path[0] 不中斷繼續 98.3Mbps
- ✅ **FEC 過渡恢復：** recovered=16 failed=0，WiFi 斷開瞬間丟失封包全部由 FEC 恢復
- ✅ **Tailscale 連帶偵測：** Tailscale (path[1]) 底層走 WiFi，同步標記 DEAD
- ✅ **0-RTT ticket save (Q.r3)：** graceful close 時 `§5f ticket save: OK (ret=0)`，`quic-tickets.bin` 305 bytes 正確含 server IP + ALPN

**Ethernet 斷線 failover + failback 測試（v1.5.196, 1080p60, 3 subflow）：**
- ✅ **Failover 成功：** 關閉乙太網路 → §Q-MP-FAILOVER-DEDUP 跳過 VPN promotion → WiFi 成為唯一 available path → video 在 WiFi 上存活
- ✅ **Failback 成功：** 恢復乙太網路 → §Q-INTERFACE-FAILBACK 偵測介面回歸 → Ethernet 恢復為 primary → 串流正常
- ✅ **FAILOVER-DEDUP (Fix O)：** 防止 DYN-STANDBY-ALIVE + FAILOVER 雙重 promotion（WiFi 已 promote 時不再 promote 底層依賴 Ethernet 的 VPN）
- ✅ **FAILBACK slot 修正 (Fix N)：** §Q-INTERFACE-FAILBACK 用 interfaceIndex 匹配 path-recovery 建立的 subflow
- ⚠️ **已知：** VPN（Tailscale）底層路由走 Ethernet → Ethernet 斷 = VPN 也斷。FAILOVER-DEDUP 正確處理此情境

**§Q.path-recovery 實作（v1.5.133）：** 背景復原執行緒（`quicRecoveryThreadProc`）每 10 秒掃描介面，對 deleted / 新介面做 ICMP 探測 + `quicAddSubflowEx()`。含 rejected cache 避免重複探測不可達介面（如 Tailscale route-check fail），介面集合變動時自動清空快取重試。驗證：rejected cache 生效（Tailscale 只探測一次）、thread 優雅關閉。WiFi 實際 recovery 需手動連線後驗測（Windows 11 CLI 無法觸發 WiFi 掃描/連線）

### §Q 剩餘待做項目

| # | 項目 | 優先級 | 狀態 |
|---|---|---|---|
| Q.r1 | Audio QUIC 傳輸 | — | ✅ **非 bug**。WASAPI loopback 在桌面無音訊時回傳 timeout → encodeThread 不產生封包。桌面播放音訊後 audio 立刻走 QUIC datagram（AUDIO: q=8611 0.9Mbps） |
| Q.r2 | Client FEC decode + jitter buffer 驗證 | — | ✅ §5a Jitter buffer 確認運作。§5b FEC decode ✅ v1.5.130 WiFi 斷線測試：recovered=16 failed=0，過渡期封包損失全由 FEC 恢復 |
| Q.r3 | Client 端 0-RTT ticket store | — | ✅ v1.5.130 實機驗證通過。`quicDisconnect()` → `picoquic_save_session_tickets()` → `§5f ticket save: OK (ret=0)`。`quic-tickets.bin` 305 bytes 含 server IP + ALPN "viplestream" |
| Q.r4 | §K.16 cwnd LOW warning 降頻 | — | ✅ 已修（quic_server.cpp 5 秒 rate limit），驗測期間 cwnd warning 從數百次降到 3 次 |
| Q.r5 | Android 多路徑實機驗測 | **P2 (hw-bound)** | ⚠️ 阻塞。Pixel 5 無 SIM 卡（gsm.sim.state=ABSENT）→ cellular 不可用；gnirehtet v2.5.1 USB 反向網路共享在 Android 15 (API 35) VPN 建立失敗（app 靜默退出，tun0 未建立）。需：①插 SIM 卡走 WiFi+cellular 或 ②換 Android 12-13 裝置測 gnirehtet |
| Q.r6 | 壓力測試 | **P3** | 待做。1440p120 + 3 subflow + 模擬 10% packet loss，確認 FEC recovery + jitter buffer 在極端條件下表現 |
| Q.r7 | Fix L.2 IDR request via QUIC datagram | — | ✅ v1.5.193。video 降級時 client 送 IDR request（×3 重複）要求 server 送新關鍵幀，加速畫面恢復 |
| Q.r8 | Fix M DYN-STANDBY-ALIVE + failover promotion | — | ✅ v1.5.194。standby 路徑在 primary stale 時自動 promote；FAILOVER 搜尋 standby 候選 |
| Q.r9 | Fix N FAILBACK slot + input 診斷 + log 降噪 | — | ✅ v1.5.195。§Q-INTERFACE-FAILBACK 用 interfaceIndex 匹配；§Q-INPUT-DIAG server/client 雙端計數器；DYN-STANDBY-FAILOVER-GUARD 改 state-transition log |
| Q.r10 | Fix O FAILOVER-DEDUP + secondary failover + server 診斷 | — | ✅ v1.5.196。防止雙重 promotion（WiFi 已 available 時跳過 VPN）；非 primary 路徑全死時嘗試二次 failover；server §Q-SERVER-PATH-DIAG 追蹤 path status 變化 |

### §Q Client picoquic build 備忘

Client 端 picoquic 已透過 moonlight-common-c 整合（QuicTransport.c + 靜態連結
picoquic），build_moonlight.cmd 在 `VIPLE_MPQUIC=ON` 時帶入。LAN 實測 client 端
QUIC streaming 正常運作（v1.5.126 驗證）。

若需 standalone cmake configure picoquic（除錯 picoquic 本身），需安裝
pkg-config + OpenSSL dev headers。一般開發走 build script 不需額外安裝。

### §SLIM Vulkan Native RIFE fix

`ncnnfruc.cpp:61` 的 `ensureRifeModelDir(modelDir)` helper 是 anonymous namespace
static。要：

1. 把 helper expose（搬到 modelfetcher.h / 自己拉成 shared util）
2. VkFrucRenderer 對 `RifeNativeExecutor::initialize(opts)` 的 caller 端先呼叫
   `ensureRifeModelDir(opts.modelDir)`，把回傳的 cache dir 取代 `opts.modelDir`
3. 驗測：清 `%LOCALAPPDATA%\\VipleStream\\fruc_models\\` → registry vkfrucEnableNativeRife=true → stream → 預期看 [VIPLE-MODELFETCH] download flownet.bin 5-10s + SHA-256 OK + RIFE init success

### §R2 PASSIVE FRUC 剩餘觀察項目

- ~~autotier T2→T0 大幅跳降是否還出現~~ **2026-05-23 confirmed：alignment gate 後只在 stream 初期 decode-warmup 出現 1 次，立刻 promote 接回** ✅
- 進 2x 後 server recv 從 ~180 掉 ~140 是否仍有（alignment gate 應消除）
- display Hz=0 legacy renderer fallback 是否需補

### §M.2 雙使用者並發 VM 驗測清單

- 雙 user account + per-user systemd service（不同 port）
- Xdummy virtual display per user
- 兩個 client 同時連 → 確認 display / audio / input 不串擾

### §N.5 Android 檔案傳輸驗測

- Send 小檔 → 看 `[VIPLE-XFER]` server log 確認 chunk 正確收到
- Receive listing UI 路徑是否需要調整

---

## 鐵律（任何改動都不能破壞）

1. **Fallback 機制保留** — renderer cascade 失敗不能讓使用者 crash
2. **D3D11 renderer 是穩定主線** — Vulkan 仍標實驗性，J.5 切換前不改預設
3. **每個 Phase 都要有 baseline 對比** — 改動前量好基準
4. **build script 不能無聲帶舊 binary** — staging step 必須做 errorlevel-check
5. **版號唯一來源** — `version.json` + `build-tools/version.ps1`，不手改子專案
