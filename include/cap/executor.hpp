#ifndef FLOWCONTROL_EXECUTOR_HPP
#define FLOWCONTROL_EXECUTOR_HPP

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <cassert>
#include <oneapi/tbb/task_arena.h>
#include <tbb/blocked_range.h>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_invoke.h>
#include <tbb/parallel_reduce.h>
#include <tbb/parallel_sort.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include <type_traits>
#include <utility>
#include <vector>

namespace flowcontrol {
/* ---------------------------------------------------------------------------
 * Role: Hard cap TBB threads and split work into:
 *  - Foreground arena for user-facing work (N or N-1 threads).
 *  - Optional background arena (1 thread) for maintenance.
 *
 * Notes:
 *  - task_arena(1) limits concurrency; it does NOT create a dedicated OS
 * thread.
 *  - global_control is process-wide; multiple live controls combine via MIN
 * cap.
 * -------------------------------------------------------------------------- */
class Executor {
public:
  using Ptr = std::shared_ptr<Executor>;

  explicit Executor(int total_threads, bool reserve_bg = true);
  ~Executor() noexcept;

  int fg_threads() const noexcept;
  int bg_threads() const noexcept;

  template <class F> void run(F &&f) {
    using Fn = std::decay_t<F>;
    auto sp = std::make_shared<Fn>(std::forward<F>(f));

    fg_.execute([sp]() {
      tbb::this_task_arena::isolate([sp]() { std::invoke(*sp); });
    });
  }

  template <class F>
  auto run_ret(F &&f) -> std::invoke_result_t<std::decay_t<F> &> {
    using Fn = std::decay_t<F>;
    using R = std::invoke_result_t<Fn &>;

    auto sp = std::make_shared<Fn>(std::forward<F>(f));
    std::exception_ptr ep;

    if constexpr (std::is_void_v<R>) {
      try {
        fg_.execute([&] {
          tbb::this_task_arena::isolate([&] {
            try {
              std::invoke(*sp);
            } catch (...) {
              ep = std::current_exception();
            }
          });
        });
      } catch (...) {
        ep = std::current_exception();
      }

      if (ep)
        std::rethrow_exception(ep);
      return;
    } else if constexpr (std::is_reference_v<R>) {
      using NR = std::remove_reference_t<R>;
      NR *ptr = nullptr;

      try {
        fg_.execute([&] {
          tbb::this_task_arena::isolate([&] {
            try {
              ptr = std::addressof(std::invoke(*sp));
            } catch (...) {
              ep = std::current_exception();
            }
          });
        });
      } catch (...) {
        ep = std::current_exception();
      }

      if (ep)
        std::rethrow_exception(ep);
      // ptr must be valid if no exception
      return *ptr;
    } else {
      // handling non reference and non void options
      std::optional<R> out;
      try {
        fg_.execute([&] {
          tbb::this_task_arena::isolate([&] {
            try {
              out.emplace(std::invoke(*sp));
            } catch (...) {
              ep = std::current_exception();
            }
          });
        });
      } catch (...) {
        ep = std::current_exception();
      }

      if (ep)
        std::rethrow_exception(ep);
      return std::move(*out);
    }
  }

  void run_many(std::vector<std::function<void()>> tasks);

  // Background (mostly serial) work.
  // Returns false if rejected (e.g., shutdown already in progress).
  bool enqueue_bg(std::function<void()> f);

  void wait_bg_idle();

  // Prevent new BG work and wait for inflight BG tasks to drain.
  void shutdown() noexcept;

private: // functions
  static int normalize_(int n) noexcept;

private:
  // Config
  int total_{0};
  bool reserve_bg_{false};

  // TBB primitives
  tbb::global_control ctrl_; // process-wide cap
  tbb::task_arena fg_;       // foreground capacity (N or N-1)

  // Dedicated BG worker
  std::thread bg_thread_;
  std::deque<std::function<void()>> bgq_;
  std::mutex bg_mu_;
  std::condition_variable bg_cv_;

  // Drain barrier + shutdown flags
  std::atomic<uint64_t> inflight_{0};
  std::atomic<bool> shutting_{false};
  bool bg_stop_{false};
};
} // namespace flowcontrol

#endif
