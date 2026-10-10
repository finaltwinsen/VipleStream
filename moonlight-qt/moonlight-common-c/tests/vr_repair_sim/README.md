# vr_repair_sim：§VR-REPAIR-RTT 的佇列模擬器與測試

補包（§VR-LINK-REPAIR）的時間預算改成跟著量到的往返時間走（§VR-REPAIR-RTT，規則見 `docs/vr_protocol.md` §4.10）。
這個目錄守兩件事：

1. **host 的主開關關著時，佇列的每一步都和改動前相同**（golden）。
2. 新規則的每一條都有測試，而且弄壞任何一條會有測試失敗（變異測試）。

模擬器把**真的** `RtpVideoQueue.c` 接上假的時鐘與封包事件，server 端用 `Sunshine/src/vr/vr_multilink_logic.h` 的
真邏輯。它是模型，不是實測：沒有幀內的延遲抖動、沒有解碼器、沒有真的鏈路。數字只能拿來比規則之間的差別。

這個目錄不在 `check_commonc_sync.ps1` 的範圍內，也不接進任何建置腳本；全部是獨立編譯，只讀 repo 的原始碼。

## 怎麼跑

需要 gcc／g++（C11、C++20）、bash、python 3。Windows 上用 MSYS2 ucrt64：

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
bash run_tests.sh                 # 全部（不含變異測試），約 3 分鐘
bash run_tests.sh --quick         # 只跑單元測試與 golden
bash run_tests.sh --ref <提交>    # 另外和「改動前的佇列」直接逐字比（見下面的 golden）
bash run_tests.sh --mutate        # 最後再跑變異測試，約多 10～15 分鐘
```

產物放在 `$VRSIM_BUILD`（預設是這個目錄底下的 `build/`，已列在 `.gitignore`）。最後一行印 `ALL PASS` 才算過。

個別跑：

| 指令 | 內容 |
|---|---|
| `bash build.sh` | 編模擬器（`sim_final`、`sim_final_inline`）與 `VrRepairRtt.h` 的單元測試 |
| `build/test_vrrr`、`build/test_vrrr_cpp` | `VrRepairRtt.h` 的單元測試，同一份原始碼以 C 與 C++ 各編一次（1,986 項檢查） |
| `python golden.py check` | 現行規則對 golden：2,844 個情境的時間軸逐字相同（三種跑法，見下面） |
| `python fuzz.py all` | 亂數情境的不變式（2,500 個），並和 `fuzz_expected.txt` 比 |
| `python scen.py` | 逐項情境（設計審查時的每一個反例），和 `scen_expected.txt` 逐字比 |
| `python matrix.py` | 行為表（往返 × 掉法），和 `matrix_expected.txt` 逐字比 |
| `bash srv_gtest.sh` | server 這一側的 googletest 獨立編譯並執行（見下面） |
| `python mutate.py` | 變異測試，和 `mutation_expected.txt` 比 |

## 各個檔案

| 檔案 | 說明 |
|---|---|
| `sim.c` | 模擬器本體：照 `VideoStream.c` 接收迴圈的做法餵佇列。檔頭有全部的參數 |
| `server_model.cpp` | 模擬器的 server 端：把 `vr_multilink_logic.h` 的真邏輯串起來（`hub_t::on_nack` 的那幾行）。改 `on_nack` 時要跟著改 |
| `test_vrrr.c` | `VrRepairRtt.h` 的單元測試（不依賴測試框架）。同一批期望值另有 googletest 版：`Sunshine/tests/unit/test_vr_repair_rtt.cpp` |
| `scenarios.py`、`golden.py`、`golden_legacy.txt` | 「開關關著時逐步相同」的情境清單、比對、每個情境的 SHA-256 |
| `fuzz.py`、`fuzz_expected.txt` | 亂數不變式與固定種子下的對照數字 |
| `scen.py`、`scen_expected.txt` | 逐項情境 |
| `matrix.py`、`matrix_expected.txt` | 行為表 |
| `srv_gtest.sh` | server 的單元測試獨立編譯 |
| `mutate.py`、`mutation_expected.txt` | 變異測試與結果 |
| `build.sh`、`run_tests.sh` | 編譯與一次跑完 |

## golden：開關關著時逐步相同

假時鐘每讀一次前進 1 µs，所以佇列在現行規則的路徑上只要多讀一次時鐘、多印一行、早一步回報，時間軸就不同。
`golden_legacy.txt` 是「改動前的佇列＋改動前寫在 `VideoStream.c` 接收迴圈裡的逐封包設定」對 2,844 個情境的輸出
（每個情境一個 SHA-256）：單一連線 1,080 個、強制「兩條在送」旗標 660 個、真正兩條連線 720 個、每幀 2～4 個 FEC
block 288 個、多 block＋兩條連線 96 個；其中 1,082 個有放棄的幀、1,821 個有回報。

`golden.py check` 拿現在的佇列比三次，三個都要和 golden 完全相同：

- `sim_final mode=legacy`：每個封包的決定走 `vrrrDecide()`（和現在的 `VideoStream.c` 一樣），協商了新規則、但這一刻
  輪到現行規則那一邊（同場 A/B 的狀態）；
- `sim_final_inline mode=legacy`：決定用原樣搬來的舊算式；
- `sim_final mode=rtt nego=0`：client 願意用新規則、server 沒同意——host 的主開關關著（或舊 host）時真正的狀態。

不想相信 golden 檔的時候，直接和改動前的原始碼比（`4028b316` 是佇列還沒有 §VR-REPAIR-RTT 的提交，golden 就是從它
產生的）：

```bash
bash build.sh --ref 4028b316 && python golden.py three
```

這會用 `git archive` 取出那個提交的 common-c，編出改動前的佇列，三個執行檔（改動前＋舊算式、改動前＋`vrrrDecide`、
現在的）逐字比。`python golden.py diff <編號>` 印出某一個情境差在哪裡；`python golden.py generate <提交>` 重新產生
golden（只有情境清單改了才需要）。

golden 是在 Windows（MSYS2 ucrt64 的 gcc）產生的。別的平台若因為 printf 的進位不同而對不上，用上面的三方比對。

## 新規則的測試

- **單元測試**（`test_vrrr.c`）：往返視窗、三個估計、等待上限、門檻與遲滯、重報、期限、放棄後的暫停、預先回報的
  上限、idle 的做法、保險、每個封包的決定（含現行那一支對舊算式的窮舉、規則切換）。
- **亂數不變式**（`fuzz.py`）：不當掉、不卡住；第一條連線上沒掉任何封包的幀一定收齊；它被補包機制壓住不超過
  「等待上限＋2 ms」。六種產生器共 2,500 個情境，違反數要是 0。另外印出和現行規則的對照，固定種子下是確定的。
  - `design`／`judge`／`toggle`／`multi`：指定「哪幾幀掉一段」、空白、停頓、估計值脫鉤、途中切換規則、多 block 與兩條連線。
  - `burst`：成串掉包——一到三段、每段 30～300 ms、段內逐包掉 3～45%，連續好幾幀各缺一部分（補包也照掉）。前四種
    做不出這種情境；現行規則本來有效的條件（區網、60～90 Hz）佔多數，對照另外依條件分組印。
  - `frozen`：`judge` 的情境，假時鐘不自動前進（`clock=0`）。平常每讀一次時鐘前進 1 µs，「同一微秒」的邊界永遠
    走不到；真的時鐘連讀兩次可以拿到同一個值。只拿來檢查不變式，逐項情境不用它逐字比：時鐘不前進時 RTT 規則
    的結果只差在時間欄位的進位，但現行規則的「退還」（回報的時刻要晚於上一個封包到達的時刻，`nkNoteArrival`）
    在同一微秒不成立，回報的份數會比平常少。
- **逐項情境**（`scen.py`）：估計值偏低、補包那一輪掉了、估計取錯連線、目前的 block 先報過後面才發現整幀沒到、長空白、
  幀內停頓、規則切換的那一刻、整段空白與 60 Hz、保險、放棄之後的暫停、預先報的幀收到 `REPAIR_GONE`。
- **行為表**（`matrix.py`）：往返 5～50 ms × 幾種掉法，現行規則與新規則各跑一次。

`*_expected.txt` 是目前規則的輸出。規則有意改動之後，先看差異是不是預期的，再用各自的 `--update` 重寫。

## server 的單元測試

專案的建置把 Sunshine 的測試關掉（`-DBUILD_TESTS=OFF`），正式的一次是 Linux 上用 `Sunshine/scripts/linux_build.sh`
建出來的測試檔。開發機要先看結果時用 `srv_gtest.sh`：它只編不依賴 Sunshine 其他部分的那幾個測試檔
（預設 `test_vr_multilink.cpp`、`test_vr_repair_rtt.cpp`、`test_vr_fec_spread.cpp`），`tests_common.h` 換成只有 gtest 的
替身，googletest 用 `Sunshine/third-party/googletest` 的原始碼。等同於：

```bash
g++ -std=c++20 -I<放替身 tests_common.h 的目錄> -ISunshine -ISunshine/third-party \
    -ISunshine/third-party/googletest/googletest/include -ISunshine/third-party/googletest/googletest \
    unit/test_vr_multilink.cpp unit/test_vr_repair_rtt.cpp unit/test_vr_fec_spread.cpp \
    Sunshine/third-party/googletest/googletest/src/gtest-all.cc Sunshine/third-party/googletest/googletest/src/gtest_main.cc \
    -o vr_ut && ./vr_ut
```

## 變異測試

`mutate.py` 把每一條規則各弄壞一次（一次只換一行裡的一小段，改在 `$VRSIM_BUILD/mut/` 的複本上，repo 的檔案不動），
重編、依序跑測試，記下第一個失敗的是哪一組。結果在 `mutation_expected.txt`：47 個變異全部被抓到，涵蓋門檻、
三個估計、等待上限、重報、預先回報、放棄後的暫停、保險、規則切換、現行規則不變、NACK 的旗標、協商、server 的
兩個放寬。另有 1 個標成「等價」：`nkNextDueUs` 裡第一份回報的可行性檢查在走得到的狀態下和 `mlHeadDeadlineUs` 的
條件重複（理由寫在那一列），拿掉它沒有任何測試會失敗，也不該有。

加了新規則就在 `mutate.py` 的表裡加一列；改了原始碼、表裡要換的字串對不上時腳本會直接報錯。

## 模擬器沒有涵蓋的

- `VideoStream.c` 接收迴圈本身沒有被執行：`sim.c` 照它的做法另外寫了一份（把連線層的狀態填進 `VRRR_IN`、把結果
  填進佇列）。兩邊共用的只有 `vrrrDecide()` 與 `RtpVideoQueue.h` 的 `RtpvSetRepairDecision()`／`RtpvSetRepairMeas()`。
  改 `VideoStream.c` 那一段時要對著 `sim.c` 的 `setQueueFields()` 一起看。
- `VrMultiLink.c`（往返樣本的收集、`LINK_HELLO` 的旗標）與 XR 那一側（LATCH 樣本、`[VIPLE-VR-MTP10]`）沒有模型。
- 影像有應用層加密時「放棄之後才到齊」的量測走另一條路（解密前就丟的封包），模擬器沒有這條路。
