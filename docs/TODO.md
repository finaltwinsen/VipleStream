# VipleStream — 待辦清單

只列**尚待 / 進行中 / 等硬體 / 等驅動**的條目。
已完成項目 → git log。歷史背景 → git log 與 docs/。

---

## 優先級總覽

| 優先級 | 條目 | 待做事項 |
|---|---|---|
| **Active (verify)** | **§β.11.b** warp edge MV threshold | Settings UI slider (2-20, default 8) 已上線 v1.4.195；待使用者實測：keep 8 / 試 4（更多邊緣保護）/ 試 12（更 smooth）|
| **Active (Phase 1-4 done)** | **§Q** MP-QUIC 多路徑網路聚合 | v1.5.0-1.5.5 ship：Phase 1 基礎 + Phase 2 multipath scheduler (ECF/REDUNDANT/MIN_RTT/AGGREGATE) + failover + FEC 互動 + Settings UI + Phase 3 OSD overlay + server 30s stats + Phase 4 0-RTT/BBR congestion/dual-stack/cellular keepalive。picoquic submodule (private-octopus, d9c705a4) pinned。Linux WSL build PASS（moonlight-qt viplestream 54MB + Android app-debug.apk）。**待：** 真實兩台機器壓測 + Sunshine Windows MSVC 端 build 驗證 |
| **Deferred (hw-bound)** | **§B Phase B** HEVC D3D11VA → Vulkan composite | Code 已 ship v1.4.184-185；阻塞：AMD 780M 測試機 HEVC D3D11VA 本身不可用。需換另一台 D3D11VA HEVC HW decode 可用的 AMD 機器驗 B7 import + B9 FRUC chain |
| **Active (test pending)** | **§B-NVOF autotier** NVOF 成為 NV 最佳 tier | Code 已 ship v1.4.117-138；待使用者 PixArk 20+ 分鐘實測，看 NVOF-PROF drop% 是否接近 0%、chain_mean 是否穩定 < 2ms。若 OK → reapply early-kickoff + NVOF 列 NV best tier；若 drop% 高 → 需 Option E skip 機制 |
| **Active (follow-up)** | **§R2 PASSIVE FRUC** ratio controller | v1.4.169-186 已 ship ratio alignment gate + extreme floor。剩：觀察 latency pattern 是否還有 T2→T0 大幅跳降；display Hz=0 legacy renderer fallback 是否需補 |
| **Active (verify)** | **§M.parity Android** wire/UX | v1.4.160 ship W1-W5+U6+U7；Pixel 5 待 install + adb logcat 確認 `[VIPLE-PARITY]` log 真實生效 |
| **Active (verify)** | **§K.dd.revert.1** display device revert | v1.4.156 ghost-check fix 已 ship；待使用者把 `dd_configuration_option` 從 `disabled` 改回 `ensure_only_display`，stream + disconnect 確認不再卡死 |
| **✅ Completed** | **§K.linux VAAPI→Vulkan bridge** | K.3 + §β.12.fix PASS v1.4.188-193：Vega 10 VAAPI + RIFE + FRUC dual-present end-to-end 通，frame#720 無 crash |
| **Active (P0)** | **§N.5.bug** Android 檔案傳輸 SSL 錯誤 | 待使用者在 Android 重現 + 拿 server log，確認錯誤在哪個階段送出 |
| **Active (verify)** | **§N.5** Android FileTransferClient runtime | 待實機在串流 session 跑 Send/Receive flow，確認 Pixel 5 Android quirks |
| **Active (post-v1.4.12)** | **§M.2** 雙使用者並發 streaming 驗測 | Ubuntu VM 軟編碼路線可先跑；NVENC 並發等實體機 |
| **Active (long-running)** | **§J.3.e.2.i.8 Phase 2.5** FRUC native source | 殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Active (β.10)** | **§J.3.e.X Path β.10** Linux/AMD/Intel 覆蓋 | §β.12.fix v1.4.193 PASS；剩：視覺品質主觀確認（tiled Conv vs coopmat）+ Intel iGPU 驗測 |
| **Deferred (driver-bound)** | **§K.4** Wayland portal teardown | `Restart=always` 緩解已 ship；需可重現串流環境（有 GPU 的 Linux 機）才能根治 |
| **Medium** | **§K.2** Raspberry Pi 5 client (aarch64) | 待 §K.1 通過後再開工；FRUC 全 disable |
| **Low** | **§J.3.e.Y 4Y.5b** native RIFE activation | 256×256 chain +1.7ms 負收益，revert；重做要先解 §β.5.3 D 全套 |
| **Low** | **§N.7** Sunshine Linux fs_picker (zenity) | zenity subprocess 實作 ~80 行；有 Linux server 使用者時再做 |
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

### §R2 PASSIVE FRUC 剩餘觀察項目

- autotier T2→T0 大幅跳降是否還出現（alignment gate ship 後應少很多）
- 進 2x 後 server recv 從 ~180 掉 ~140 是否仍有（alignment gate 應消除）

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
