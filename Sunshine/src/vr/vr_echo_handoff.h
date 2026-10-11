/**
 * @file src/vr/vr_echo_handoff.h
 * @brief VipleStream 2.0 §VR-DIRECT-KICK（2026-10-11）：回聲年齡從擷取執行緒交到 bridge 執行緒的那一小段緩衝。
 *
 * 判定「多一格」的偵測器（vr_direct_kick.h 的 detector_t）只給 bridge 執行緒用、沒有鎖；回聲年齡卻是擷取執行緒
 * （display_vr_t 的 capture 迴圈）算的。這個類別是兩邊唯一共用的東西：一個固定大小、上鎖的緩衝，加一個「現在收
 * 哪一個 generation」的閘門。只用標準函式庫、不配置記憶體，可以脫離整個 server 單獨編譯測試
 * （tests/unit/test_vr_echo_handoff.cpp）。數值是什麼單位它不管，照原樣交過去（bridge 放的是 QPC tick）。
 *
 *   寫入方（任何執行緒；實際是擷取執行緒）只呼叫 push()：先比一次 atomic 的閘門，對不上就返回、不取鎖——
 *     開關關著、偵測器不動作、沒有連線時閘門是關的，擷取端多的只有這一次比對。對得上才取鎖放進去；緩衝滿了
 *     （讀取方卡住）就丟掉這一筆。
 *   讀取方（只有一條執行緒：bridge）呼叫 open()／close()／drain()。drain() 把目前有的全部倒出來，在鎖外面一筆
 *     一筆交給呼叫端；緩衝是空的時候只讀一個 atomic、不取鎖（bridge 連線中每 2 ms 呼叫一次）。
 *
 * 每一筆都帶著自己的 generation：push() 通過閘門之後、取到鎖之前，讀取方可能已經關掉閘門又開給下一個 generation
 * （拆線緊接著新的握手）；那一筆會留在緩衝裡，drain() 時因為 generation 對不上而丟掉，不會交給新的偵測器。
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace vr::kick {

  /// 緩衝的大小。讀取方每 2 ms 倒一次，平常裡面只有 0～1 筆；放滿表示它卡住了：60 Hz 的 64 筆是 1.07 秒、
  /// 72 Hz 0.89 秒、90 Hz 0.71 秒。滿了之後丟的是新來的（留著先到的那 64 筆）。
  /// 卡到會丟樣本的時候，偵測器連續成立的計時幾乎都已經重來了：bridge 恢復後先 poll() 才倒樣本，那一次評估的
  /// 1 秒視窗裡只剩卡住之前的那一小段——卡超過 0.6（60 Hz）～0.73 秒（90 Hz）就不到 24 筆；卡超過 1 秒更是直接
  /// 算不連續。唯一「丟了樣本、計時卻沒有重來」的是 90 Hz 卡 0.71～0.73 秒：丟的是最新的一兩筆，倒出來之後視窗裡
  /// 還有八十幾筆，判定不受影響（tests/unit/test_vr_echo_handoff.cpp 的 VrEchoHandoffWithDetector）
  inline constexpr std::size_t k_handoff_slots = 64;

  class echo_handoff_t {
  public:
    /**
     * @brief 讀取方：清掉緩衝裡留下來的，再把閘門開給這個 generation。
     * @param generation 之後只收這個 generation 的樣本；0＝不收（等於 close()，另外把緩衝清掉）
     */
    void open(uint64_t generation) {
      {
        std::lock_guard lk(mtx_);
        count_ = 0;
        pending_.store(false, std::memory_order_release);
      }
      gate_.store(generation, std::memory_order_relaxed);
    }

    /// 讀取方：關上閘門。已經在緩衝裡的不動（下一次 open() 會清掉，或在 drain() 時照 generation 過濾）
    void close() {
      gate_.store(0, std::memory_order_relaxed);
    }

    /// 現在收哪一個 generation（0＝關著）
    uint64_t gate() const {
      return gate_.load(std::memory_order_relaxed);
    }

    /**
     * @brief 寫入方：交一筆。
     * @param generation 這一筆屬於哪一個 generation（從 1 起；0 一律不收）
     * @param t 時刻
     * @param age 回聲年齡
     * @return 有放進緩衝為 true；閘門對不上、緩衝滿了都是 false
     */
    bool push(uint64_t generation, int64_t t, int64_t age) {
      if (generation == 0 || gate_.load(std::memory_order_relaxed) != generation) {
        return false;
      }
      std::lock_guard lk(mtx_);
      if (count_ >= buf_.size()) {
        return false;
      }
      buf_[count_++] = item_t {generation, t, age};
      pending_.store(true, std::memory_order_release);
      return true;
    }

    /**
     * @brief 讀取方：把緩衝倒空，屬於 generation 的每一筆照放進去的順序呼叫 fn(t, age)（在鎖外面呼叫）。
     *        其他 generation 的丟掉。
     * @return 呼叫了 fn 幾次
     */
    template<class F>
    std::size_t drain(uint64_t generation, F &&fn) {
      if (!pending_.load(std::memory_order_acquire)) {
        return 0;
      }
      std::array<item_t, k_handoff_slots> batch;
      std::size_t n = 0;
      {
        std::lock_guard lk(mtx_);
        n = count_;
        for (std::size_t i = 0; i < n; ++i) {
          batch[i] = buf_[i];
        }
        count_ = 0;
        pending_.store(false, std::memory_order_release);
      }
      std::size_t used = 0;
      for (std::size_t i = 0; i < n; ++i) {
        if (batch[i].generation == generation) {
          fn(batch[i].t, batch[i].age);
          ++used;
        }
      }
      return used;
    }

  private:
    struct item_t {
      uint64_t generation = 0;
      int64_t t = 0;
      int64_t age = 0;
    };

    std::atomic<uint64_t> gate_ {0};  ///< 讀取方寫、誰都可以讀
    std::atomic<bool> pending_ {false};  ///< 緩衝裡有東西；只在 mtx_ 內改，讀取方先看它
    std::mutex mtx_;  ///< 保護 buf_ 與 count_；拿著它的時候不取別的鎖、不呼叫外面的程式
    std::array<item_t, k_handoff_slots> buf_ {};
    std::size_t count_ = 0;
  };

}  // namespace vr::kick
