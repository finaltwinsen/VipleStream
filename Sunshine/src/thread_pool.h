/**
 * @file src/thread_pool.h
 * @brief Declarations for the thread pool system.
 */
#pragma once

// standard includes
#include <atomic>
#include <thread>

// local includes
#include "platform/common.h"
#include "task_pool.h"

namespace thread_pool_util {
  /**
   * Allow threads to execute unhindered while keeping full control over the threads.
   */
  class ThreadPool: public task_pool_util::TaskPool {
  public:
    typedef TaskPool::__task __task;

  private:
    std::vector<std::thread> _thread;

    std::condition_variable _cv;
    std::mutex _lock;

    std::atomic<bool> _continue;

    // §ENC-PACE（2026-11-27）：延遲工作的準時叫醒。MinGW 的 condition_variable::wait_until 至少等一個
    // 15.6 ms 的系統刻度（host 實測：等 10 ms 實際 11.6～24 ms，計時器調到 0.5 ms 也一樣），
    // 滑鼠左鍵放開延遲 10 ms 變 16 ms、按鍵連發 25 Hz 掉到約 21 Hz。這條執行緒用平台的高精度計時器睡到
    // 最早的到期時刻（每次最多 2 ms，好接住中途插進來、更早到期的工作）再 notify；沒有延遲工作時只等 _waker_cv。
    std::thread _waker;
    std::condition_variable _waker_cv;

  public:
    ThreadPool():
        _continue {false} {
    }

    explicit ThreadPool(int threads):
        _thread(threads),
        _continue {true} {
      for (auto &t : _thread) {
        t = std::thread(&ThreadPool::_main, this);
      }
      _waker = std::thread(&ThreadPool::_waker_main, this);
    }

    ~ThreadPool() noexcept {
      if (!_continue) {
        return;
      }

      stop();
      join();
    }

    template<class Function, class... Args>
    auto push(Function &&newTask, Args &&...args) {
      std::lock_guard lg(_lock);
      auto future = TaskPool::push(std::forward<Function>(newTask), std::forward<Args>(args)...);

      _cv.notify_one();
      return future;
    }

    void pushDelayed(std::pair<__time_point, __task> &&task) {
      std::lock_guard lg(_lock);

      TaskPool::pushDelayed(std::move(task));

      _cv.notify_all();
      _waker_cv.notify_all();
    }

    template<class Function, class X, class Y, class... Args>
    auto pushDelayed(Function &&newTask, std::chrono::duration<X, Y> duration, Args &&...args) {
      std::lock_guard lg(_lock);
      auto future = TaskPool::pushDelayed(std::forward<Function>(newTask), duration, std::forward<Args>(args)...);

      // Update all timers for wait_until
      _cv.notify_all();
      _waker_cv.notify_all();
      return future;
    }

    void start(int threads) {
      _continue = true;

      _thread.resize(threads);

      for (auto &t : _thread) {
        t = std::thread(&ThreadPool::_main, this);
      }
      if (!_waker.joinable()) {
        _waker = std::thread(&ThreadPool::_waker_main, this);
      }
    }

    void stop() {
      std::lock_guard lg(_lock);

      _continue = false;
      _cv.notify_all();
      _waker_cv.notify_all();
    }

    void join() {
      for (auto &t : _thread) {
        if (t.joinable()) {
          t.join();
        }
      }
      if (_waker.joinable()) {
        _waker.join();
      }
    }

  public:
    void _waker_main() {
      platf::set_thread_name("TaskPool::waker");
      auto timer = platf::create_high_precision_timer();
      if (!timer || !*timer) {
        return;  // 沒有高精度計時器：工作執行緒的 wait_until 照舊（約 16 ms 一格）
      }
      std::unique_lock lk(_lock);
      while (_continue.load(std::memory_order_relaxed)) {
        const auto tp = next();
        if (!tp) {
          _waker_cv.wait(lk);
          continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (*tp <= now) {
          // 到期了：叫醒工作執行緒；它若正忙，0.5 ms 後再叫一次（只在有到期工作時才會這樣輪）
          _cv.notify_one();
          lk.unlock();
          timer->sleep_for(std::chrono::microseconds(500));
          lk.lock();
          continue;
        }
        const auto wait = std::min<std::chrono::steady_clock::duration>(*tp - now, std::chrono::milliseconds(2));
        lk.unlock();
        timer->sleep_for(wait);
        lk.lock();
      }
    }

    void _main() {
      platf::set_thread_name("TaskPool::worker");
      while (_continue.load(std::memory_order_relaxed)) {
        if (auto task = this->pop()) {
          (*task)->run();
        } else {
          std::unique_lock uniq_lock(_lock);

          if (ready()) {
            continue;
          }

          if (!_continue.load(std::memory_order_relaxed)) {
            break;
          }

          if (auto tp = next()) {
            _cv.wait_until(uniq_lock, *tp);
          } else {
            _cv.wait(uniq_lock);
          }
        }
      }

      // Execute remaining tasks
      while (auto task = this->pop()) {
        (*task)->run();
      }
    }
  };
}  // namespace thread_pool_util
