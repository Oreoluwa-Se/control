#ifndef FLOWCONTROL_BASE_TEST_HPP
#define FLOWCONTROL_BASE_TEST_HPP

#include "cap/executor.hpp"
#include "cap/scheduler.hpp"
#include <numeric>

namespace flowcontrol_test_helpers {
inline void fail(const char *msg) {
  std::cerr << "[flowcontrol_tests] FAIL: " << msg << "\n";
  std::abort();
}

inline void require(bool cond, const char *msg) {
  if (!cond)
    fail(msg);
}

// ---------------- Concurrency probe ----------------
struct ConcurrencyProbe {
  std::atomic<int> in{0};
  std::atomic<int> peak{0};

  void enter() {
    const int v = in.fetch_add(1, std::memory_order_relaxed) + 1;
    int p = peak.load(std::memory_order_relaxed);
    while (v > p &&
           !peak.compare_exchange_weak(p, v, std::memory_order_relaxed)) {
    }
  }

  void leave() { in.fetch_sub(1, std::memory_order_relaxed); }
};

// Busy loop to encourage overlap without relying on sleeps.
inline void spin_work(int iters = 20000) {
  for (volatile int k = 0; k < iters; ++k) {
  }
}

} // namespace flowcontrol_test_helpers

namespace test {
using namespace flowcontrol_test_helpers;

inline void fg_cap_respected() {
  std::cout << "---> 1) Testing Foreground Cap" << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(/*total_threads=*/4,
                                                    /*reserve_bg=*/true);
  const int fg = ex->fg_threads();

  ConcurrencyProbe probe;

  ex->run([&] {
    tbb::parallel_for(0, 2000, [&](int) {
      probe.enter();
      spin_work(40000);
      probe.leave();
    });
  });

  require(probe.peak.load(std::memory_order_relaxed) <= fg,
          "FG cap violated: observed concurrency exceeded fg_threads()");
}

inline void rejected_jobs_are_serial() {
  std::cout << "---> 2) Testing that the Serial branch works ";

  auto ex = std::make_shared<flowcontrol::Executor>(4, true);
  flowcontrol::Scheduler sched(ex, /*max_parallel_jobs=*/1,
                               /*small_span_threshold=*/1);

  std::atomic<int> serial_only_count{0};

  auto worker = [&] {
    ConcurrencyProbe probe;

    sched.pfor<int>(
        0, 800,
        [&](int) {
          probe.enter();
          spin_work(25000);
          probe.leave();
        },
        /*grainsize=*/0, /*force=*/true);

    if (probe.peak.load(std::memory_order_relaxed) == 1)
      serial_only_count.fetch_add(1, std::memory_order_relaxed);
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 10; ++i)
    threads.emplace_back(worker);
  for (auto &t : threads)
    t.join();

  std::cout << "Num serial: "
            << serial_only_count.load(std::memory_order_relaxed) << std::endl;
  require(serial_only_count.load(std::memory_order_relaxed) > 0,
          "Expected at least one rejected/serial job, but none observed");
}

inline void nested_parallelism_throttled() {
  std::cout << "---> 3) Nested Parallelism Check" << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(6, true);
  flowcontrol::Scheduler sched(ex, /*max_parallel_jobs=*/2,
                               /*small_span_threshold=*/1);

  ConcurrencyProbe probe;

  sched.pfor<int>(
      0, 300,
      [&](int) {
        probe.enter();

        // Nested region attempt
        sched.pfor<int>(
            0, 200, [&](int) { spin_work(15000); }, 0, true);

        spin_work(20000);
        probe.leave();
      },
      0, true);

  require(probe.peak.load(std::memory_order_relaxed) <= ex->fg_threads(),
          "Nested parallelism exceeded fg_threads()");
}

inline void with_concurrency_limits_parallel_sort() {
  std::cout << "---> 4) With concurrency test" << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(8, true);
  flowcontrol::Scheduler sched(ex, /*max_parallel_jobs=*/4,
                               /*small_span_threshold=*/1);

  std::vector<int> v(150000);
  std::iota(v.begin(), v.end(), 0);
  std::reverse(v.begin(), v.end());

  ConcurrencyProbe probe;

  auto cmp = [&](int a, int b) {
    probe.enter();
    spin_work(4000);
    probe.leave();
    return a < b;
  };

  const int k = 2;
  sched.with_concurrency(k,
                         [&] { tbb::parallel_sort(v.begin(), v.end(), cmp); });

  // In practice peak should be <= k (if comparator work is heavy enough).
  require(probe.peak.load(std::memory_order_relaxed) <= k,
          "with_concurrency(k) did not limit parallel_sort concurrency");
}

inline void bg_shutdown_drain_and_reject() {
  std::cout << "---> 5) Testing Shutdown Sequence" << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(4, true);
  flowcontrol::Scheduler sched(ex, /*max_parallel_jobs=*/2,
                               /*small_span_threshold=*/1);

  std::atomic<int> done{0};

  for (int i = 0; i < 200; ++i) {
    sched.bg([&] {
      spin_work(15000);
      done.fetch_add(1, std::memory_order_relaxed);
    });
  }

  // Drain and stop BG
  ex->shutdown();

  const auto st = sched.bg_stats();
  const size_t accepted = st.total - st.dropped;

  require(done.load(std::memory_order_relaxed) == static_cast<int>(accepted),
          "BG drain mismatch: done != accepted");

  // After shutdown, enqueue should be rejected/dropped
  sched.bg([&] { done.fetch_add(1000, std::memory_order_relaxed); });
  const auto st2 = sched.bg_stats();
  require(st2.dropped > st.dropped,
          "Expected bg task to be dropped after shutdown, but drop count did "
          "not increase");
}

inline void small_span_runs_serial() {
  std::cout << "---> 6) Testing scenario when small span >= serial"
            << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(6, true);
  flowcontrol::Scheduler sched(ex, /*max_parallel_jobs=*/3,
                               /*small_span_threshold=*/2048);

  ConcurrencyProbe probe;
  sched.pfor<int>(0, 100, [&](int) { // < threshold
    probe.enter();
    spin_work(20000);
    probe.leave();
  });

  require(probe.peak.load(std::memory_order_relaxed) == 1,
          "Expected small span to run serial, but saw parallel execution");
}

inline void run_ret_propagates_exceptions() {
  std::cout << "---> 7) Testing exception propagation" << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(4, true);
  bool caught = false;
  try {
    (void)ex->run_ret([&]() -> int { throw std::runtime_error("boom"); });
  } catch (const std::runtime_error &) {
    caught = true;
  }
  require(caught, "Executor::run_ret did not propagate exception");
}

inline void with_concurrency_propagates_exceptions() {
  std::cout << "---> 8) Testing exception propagation v2" << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(6, true);
  flowcontrol::Scheduler sched(ex, 2, 1);

  bool caught = false;
  try {
    sched.with_concurrency(2, [&] { throw std::runtime_error("boom"); });
  } catch (const std::runtime_error &) {
    caught = true;
  }
  require(caught, "with_concurrency did not propagate exception");
}

inline void stress_mixed_operations() {
  std::cout << "---> 9) Testing mixed operations. [Stress test]" << std::endl;
  auto ex = std::make_shared<flowcontrol::Executor>(8, true);
  flowcontrol::Scheduler sched(ex, 4, 1);

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;

  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        sched.pfor<int>(
            0, 2000, [&](int) { spin_work(2000); }, 0, false);
        sched.pinvoke(
            5000, [&] { spin_work(5000); }, [&] { spin_work(5000); });
        (void)sched.preduce<int, int>(
            0, 500, 0,
            [&](int i) {
              spin_work(500);
              return i;
            },
            [&](int a, int b) { return a + b; }, 0, false);

        sched.bg([&] { spin_work(5000); });
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true, std::memory_order_relaxed);
  for (auto &th : threads)
    th.join();

  ex->shutdown(); // should not deadlock
  require(true, "stress test completed");
}

inline bool run_all() {
  std::cout << "[FlowControl] Running all tests...." << std::endl;
  fg_cap_respected();
  rejected_jobs_are_serial();
  nested_parallelism_throttled();
  with_concurrency_limits_parallel_sort();
  bg_shutdown_drain_and_reject();
  small_span_runs_serial();
  run_ret_propagates_exceptions();
  with_concurrency_propagates_exceptions();
  stress_mixed_operations();

  std::cout << "[FlowControl] Tests Completed\n" << std::endl;
  return true;
}
} // namespace test

#endif
