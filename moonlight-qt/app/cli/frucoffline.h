// VipleStream §FRUC-VALIDATE — 離線 FRUC 品質量測（dev-only）。
//
// 把一段 PNG 序列當輸入幀，餵進真實的 VkFrucRenderer（software upload +
// FRUC + frame dump），dump 出 real/interp 幀到 dump 目錄，供
// scripts/benchmark/fruc_metrics.py 對 ground-truth 比對。
//
// 走真實出貨碼路（renderFrameSw → FRUC chain → §B-DUMP），所以量到的就是
// 使用者實際看到的補幀演算法，而非另寫的模型。

#pragma once

#include <cstdlib>  // main.cpp 透過本 header 取得 std::_Exit（跑完即離）

class FrucOfflineCommandLineParser;

// 跑離線 FRUC 量測。回傳 process exit code（0=成功，非 0=各階段失敗碼）。
// 此函式同步阻塞直到所有 frame 餵完且 dump flush 完成。
int runFrucOffline(const FrucOfflineCommandLineParser& args);
