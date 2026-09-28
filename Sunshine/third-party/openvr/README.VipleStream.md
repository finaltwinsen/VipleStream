# OpenVR SDK（vendored）

VipleStream §VR M1b S2-01。SteamVR driver（`driver_viplestream.dll`）與開發工具
`vr_probe` 用到的 OpenVR SDK 檔案，**原樣**從上游取來，沒有做任何修改。

| 項目 | 值 |
|---|---|
| 上游 | https://github.com/ValveSoftware/openvr |
| tag | `v2.15.6`（annotated tag 物件 `41bc3825fd35b04047610c86fee26fb33b017b29`） |
| commit | `0924064316de3effbcd1acf1e309182a2deb1c05`（「OpenVR SDK 2.15.6」，2026-03-27） |
| 取檔日期 | 2026-09-28 |
| 授權 | BSD-3-Clause（`LICENSE` 為上游原文） |
| 版本常數 | `openvr_driver.h`、`openvr.h` 第 18-20 行：`k_nSteamVRVersionMajor/Minor/Build = 2/15/6` |

## 檔案清單

路徑相對於本目錄，也等於上游 repo 內的路徑。

| 檔案 | 大小（byte） | sha256 | git blob（上游 tree） |
|---|---:|---|---|
| `headers/openvr_driver.h` | 199942 | `1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab` | `fcd675583622df0ea1a54d4f88c2b34c195c199b` |
| `headers/openvr.h` | 296217 | `1e6ed57199896cc1f7c5484e50fa18955e97be15be690beb28d998c877ead7fd` | `7b25ab4e2c0048db92458ef5f6dcf03929cb809d` |
| `lib/win64/openvr_api.lib` | 5500 | `a0bf57c5920f569e8d21ab3e5bc95bac4b73e2016217f8b5b93495a2a7197bbb` | `76523aceca8101109014dbb8d17e1ac9b09042e2` |
| `bin/win64/openvr_api.dll` | 837272 | `bab8ac6ef64e68a9ca53315b0014d131088584b2efdfa6db511d67ec03cfcb4a` | `83b201974728158157be0bf6fc3b43caed34ab11` |
| `LICENSE` | 1488 | `f56ff606104d4ef18e617921a75c73ad73b5a1a1d70c69590c29de16919e04ad` | `ee83337d7fcb726d14cc10f7dd2fda6799d8a135` |

本地新增、不屬於上游的檔案：`README.VipleStream.md`（本檔）、`.gitattributes`、`.gitignore`。

## 取檔方式與驗證

1. 逐檔下載：`https://raw.githubusercontent.com/ValveSoftware/openvr/0924064316de3effbcd1acf1e309182a2deb1c05/<路徑>`。
2. `git hash-object --no-filters <檔>` 與 GitHub API
   `GET /repos/ValveSoftware/openvr/git/trees/0924064316de3effbcd1acf1e309182a2deb1c05?recursive=1`
   列出的 blob sha、size 逐一比對：五個全部相同。
3. 另外下載 release tag 的原始碼封包
   `https://github.com/ValveSoftware/openvr/archive/refs/tags/v2.15.6.tar.gz`
   （sha256 `e184cb625010fab7043a9d5e1e000fdeb3067a152bb3169ef53f64dfac37164c`，154998016 byte），
   解出同樣五個檔案，sha256 與上表完全相同。v2.15.6 的 GitHub release 沒有另外附檔，
   所以這個 tag 封包就是「release 檔」。
4. `refs/tags/v2.15.6` → tag 物件 `41bc3825…` → commit `0924064316de3effbcd1acf1e309182a2deb1c05`，已由 API 確認。

驗證本目錄（Git Bash）：

```sh
cd Sunshine/third-party/openvr
sha256sum headers/openvr_driver.h headers/openvr.h lib/win64/openvr_api.lib bin/win64/openvr_api.dll LICENSE
```

## 二進位檔的其他事實

- `bin/win64/openvr_api.dll`：PE32+、machine `0x8664`（x64），Authenticode 簽章有效，
  簽署者 `CN=Valve Corp., O=Valve Corp., L=Bellevue, S=Washington, C=US`（2026-09-28 在 `.195` 以
  `Get-AuthenticodeSignature` 驗證）。
- **注意**：這顆 DLL 的版本資源是 `FileVersion 1.1.1`（`FileVersionRaw 1.1.1.0`），**不是** 2.15.6。
  這是上游一直沒更新的版本資源，不代表取錯檔；任何檢查都要用上表的 sha256，不要用 DLL 的 FileVersion。
- `lib/win64/openvr_api.lib`：x64 匯入庫（ar 封存，import object machine 全為 `0x8664`），對應
  `openvr_api.dll`，匯出 `VR_InitInternal2`、`VR_ShutdownInternal`、`VR_GetGenericInterface`、
  `VR_IsInterfaceVersionValid`、`VR_GetInitToken`、`VR_IsHmdPresent`、`VR_IsRuntimeInstalled`、
  `VR_GetRuntimePath` 等 18 個符號。

## 使用規則

- **不要修改這裡的任何上游檔案**。`openvr_driver.h` 的 sha256 是 driver 建置新鮮度檢查的一部分；
  它的預設實作會觸發 MSVC C4100，建置時一律用 `/external:I <本目錄>\headers /external:W0`
  隔離（設計文件 §G），不要改標頭、也不要對我們自己的程式碼關掉 `/WX`。
- **只有 `vr_probe`（OpenVR client）連 `openvr_api.lib`／帶 `openvr_api.dll`**。SteamVR driver
  只用 `openvr_driver.h`（純標頭），`dumpbin /dependents` 出現 `openvr_api.dll` 即視為建置錯誤（§G）。
- 出貨的 driver 套件要附 `LICENSE-OpenVR.txt`（= 本目錄 `LICENSE` 原文）與 `THIRD_PARTY_NOTICES.md`。
- `.gitattributes` 對本目錄全部設 `-text`、`*.lib`／`*.dll` 設 `binary`：repo 根目錄的
  `* text=auto` 加上 `core.autocrlf=true` 會在 checkout 時把 LF 標頭轉成 CRLF，sha256 就不再等於上游。
- `.gitignore` 以確切路徑反向放行 `lib/win64/openvr_api.lib` 與 `bin/win64/openvr_api.dll`，
  因為 `Sunshine/.gitignore` 全域排除了 `*.dll`（第 17 行）與 `*.lib`（第 27 行）。
- 升級 SDK 時：整組換新（五個檔案一起）、重算上表、更新 tag／commit／取檔日期，並以**單獨一個 commit** 提交。
