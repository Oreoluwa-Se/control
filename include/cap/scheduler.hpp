#ifndef FLOWCONTROL_SCHEDULER_HPP
#define FLOWCONTROL_SCHEDULER_HPP

#include "executor.hpp"

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>

#include <cassert>
#include <iostream>
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

namespace flowcontrol {
/* ---------------------------------------------------------------------------
 * Role: Uses the Executor to control subscription flow:
 * Goal is to avoid oversubscribing to threads and control overall information
 * flow using the hard cap as a limiting factor.
 * -------------------------------------------------------------------------- */

struct BgStats {
  size_t total{0};
  size_t dropped{0};
};

class Scheduler {
  // TLS state: “are we inside with_job?”, and if so, are we admitted?
  inline static thread_local bool tls_active_ = false;
  inline static thread_local bool tls_allow_ = true;
  std::atomic<int> active_scopes_{0};

  struct TlsFrame {
    bool prev_active{false};
    bool prev_allow{true};
    bool engaged{false};

    TlsFrame() = default;
    explicit TlsFrame(bool allow) { engage(allow); }

    TlsFrame(const TlsFrame &) = delete;
    TlsFrame &operator=(const TlsFrame &) = delete;
    TlsFrame(TlsFrame &&) = delete;
    TlsFrame &operator=(TlsFrame &&) = delete;

    void engage(bool allow) {
      assert(!engaged && "TlsFrame::engage called twice");
      engaged = true;
      prev_active = tls_active_;
      prev_allow = tls_allow_;
      tls_active_ = true;
      tls_allow_ = allow;
    }

    ~TlsFrame() {
      if (engaged) {
        tls_active_ = prev_active;
        tls_allow_ = prev_allow;
      }
    }
  };

public:
  using Ptr = std::shared_ptr<Scheduler>;

  explicit Scheduler(Executor::Ptr ex, int max_parallel_jobs = -1,
                     int small_span_threshold = 2048);

  // Shared counters block (kept alive by tokens/guards).
  struct Counters {
    std::atomic<int> jobs{0};    // admitted top-level jobs
    std::atomic<int> regions{0}; // active inner parallel regions
  };

  // ---- Job admission (soft gating) ---- //
  struct JobToken {
    std::shared_ptr<Counters> ctr{};
    bool owned{false};

    JobToken() = default;
    JobToken(std::shared_ptr<Counters> c, bool ok)
        : ctr(std::move(c)), owned(ok) {}

    JobToken(const JobToken &) = delete;
    JobToken &operator=(const JobToken &) = delete;

    JobToken(JobToken &&o) noexcept : ctr(std::move(o.ctr)), owned(o.owned) {
      o.owned = false;
    }

    JobToken &operator=(JobToken &&o) noexcept {
      if (this != &o) {
        if (owned && ctr)
          ctr->jobs.fetch_sub(1, std::memory_order_acq_rel);

        ctr = std::move(o.ctr);
        owned = o.owned;
        o.owned = false;
      }

      return *this;
    }

    ~JobToken() {
      if (owned && ctr)
        ctr->jobs.fetch_sub(1, std::memory_order_acq_rel);
    }

    explicit operator bool() const noexcept { return owned; }
  };

  struct ParallelRegionGuard {
    std::shared_ptr<Counters> ctr{};
    explicit ParallelRegionGuard(const std::shared_ptr<Counters> &c) : ctr(c) {
      if (ctr)
        ctr->regions.fetch_add(1, std::memory_order_relaxed);
    }

    ParallelRegionGuard(const ParallelRegionGuard &) = delete;
    ParallelRegionGuard &operator=(const ParallelRegionGuard &) = delete;

    ParallelRegionGuard(ParallelRegionGuard &&o) noexcept
        : ctr(std::move(o.ctr)) {
      o.ctr.reset();
    }

    ~ParallelRegionGuard() {
      if (ctr)
        ctr->regions.fetch_sub(1, std::memory_order_relaxed);
    }
  };

  inline JobToken enter_job() { return enter_job_(); }

  inline bool allow_parallel(size_t span_hint) const noexcept {
    return allow_parallel_(span_hint);
  }

  int fg_threads() const noexcept { return ex_ ? ex_->fg_threads() : 1; }

  int int_est_thread_per() const noexcept;

  template <class I, class Body>
  void pfor(I first, I last, Body &&body, I grainsize = I(0),
            bool force = false) {
    Scope::Job top(*this);

    if (tls_active_ && !tls_allow_) { // rejected => serial
      for (I idx = first; idx < last; ++idx)
        body(idx);
      return;
    }

    const I n = last - first;
    if (!ex_ || (!force && !allow_parallel_(static_cast<size_t>(n)))) {
      for (I idx = first; idx < last; ++idx)
        body(idx);
      return;
    }

    ParallelRegionGuard rg(counters_);
    const bool allow = tls_allow_; // capture current job allow-policy
    const int k = int_est_thread_per();
    const I gs = (grainsize <= I(0)) ? pick_grainsize_(n, k)
                                     : std::max<I>(I(1), grainsize);

    ex_->run([&] {
      tbb::parallel_for(tbb::blocked_range<I>(first, last, gs),
                        [&](const tbb::blocked_range<I> &r) {
                          // Propagate job TLS to worker
                          Scope::Worker ws(allow);
                          for (I idx = r.begin(); idx != r.end(); ++idx)
                            body(idx);
                        });
    });
  }

  template <class... Fns> void pinvoke(size_t span_hint, Fns &&...fns) {
    Scope::Job top(*this);

    if (tls_active_ && !tls_allow_) {
      (std::forward<Fns>(fns)(), ...);
      return;
    }

    if (!ex_ || !allow_parallel_(span_hint)) {
      (std::forward<Fns>(fns)(), ...);
      return;
    }

    ParallelRegionGuard rg(counters_);
    const bool allow = tls_allow_;

    ex_->run([&] {
      tbb::task_group tg;

      (tg.run([allow, sp = std::make_shared<std::decay_t<Fns>>(
                          std::forward<Fns>(fns))]() {
        Scope::Worker ws(allow);
        std::invoke(*sp);
      }),
       ...);

      tg.wait();
    });
  }

  template <class I, class T, class Map, class Reduce>
  T preduce(I first, I last, T init, Map &&map, Reduce &&reduce,
            I grainsize = I(0), bool force = false) {
    Scope::Job top(*this);
    const I n = last - first;

    if (tls_active_ && !tls_allow_) {
      T acc = std::move(init);
      for (I idx = first; idx < last; ++idx)
        acc = reduce(std::move(acc), map(idx));
      return acc;
    }

    if (!ex_ || (!force && !allow_parallel_(static_cast<size_t>(n)))) {
      T acc = std::move(init);
      for (I idx = first; idx < last; ++idx)
        acc = reduce(std::move(acc), map(idx));
      return acc;
    }

    ParallelRegionGuard rg(counters_);
    const bool allow = tls_allow_;
    const int k = int_est_thread_per();
    const I gs = (grainsize <= I(0)) ? pick_grainsize_(n, k)
                                     : std::max<I>(I(1), grainsize);

    return ex_->run_ret([&] {
      using Range = tbb::blocked_range<I>;
      return tbb::parallel_reduce(
          Range(first, last, gs), std::move(init),
          [&](const Range &r, T acc) -> T {
            Scope::Worker ws(allow);
            for (I i = r.begin(); i != r.end(); ++i)
              acc = reduce(std::move(acc), map(i));
            return acc;
          },
          [&](T a, T b) -> T {
            Scope::Worker ws(allow);
            return reduce(std::move(a), std::move(b));
          });
    });
  }

  template <class I, class T, class MapRange, class Reduce>
  T preduce_range(I first, I last, T init, MapRange &&map_range,
                  Reduce &&reduce, I grainsize = I(0), bool force = false) {
    Scope::Job top(*this);
    const I n = last - first;

    if (tls_active_ && !tls_allow_) {
      tbb::blocked_range<I> r(first, last, std::max<I>(I(1), grainsize));
      return std::invoke(map_range, r, std::move(init));
    }

    if (!ex_ || (!force && !allow_parallel_(static_cast<size_t>(n)))) {
      tbb::blocked_range<I> r(first, last, std::max<I>(I(1), grainsize));
      return std::invoke(map_range, r, std::move(init));
    }

    ParallelRegionGuard rg(counters_);
    const bool allow = tls_allow_;
    const int k = int_est_thread_per();
    const I gs = (grainsize <= I(0)) ? pick_grainsize_(n, k)
                                     : std::max<I>(I(1), grainsize);

    return ex_->run_ret([&] {
      using Range = tbb::blocked_range<I>;
      return tbb::parallel_reduce(
          Range(first, last, gs), std::move(init),
          [&](const Range &r, T acc) -> T {
            Scope::Worker ws(allow);
            return std::invoke(map_range, r, std::move(acc));
          },
          [&](T a, T b) -> T {
            Scope::Worker ws(allow);
            return std::invoke(reduce, std::move(a), std::move(b));
          });
    });
  }

  template <class F> decltype(auto) with_concurrency(int k, F &&f) {
    Scope::Job top(*this); // establish implicit “top job” scope

    const int fg = fg_threads();
    int kk = std::clamp(k, 1, std::max(1, fg));

    // If rejected, force serial.
    if (tls_active_ && !tls_allow_)
      kk = 1;

    // Honor contention logic (jobs/regions) even though span_hint is unknown
    // here.
    if (kk > 1 && !allow_parallel_(static_cast<size_t>(small_cutoff_)))
      kk = 1;

    if (!ex_)
      return std::invoke(std::forward<F>(f));

    // Count as one region only if we actually intend to run parallel.
    std::optional<ParallelRegionGuard> rg;
    if (kk > 1)
      rg.emplace(counters_);

    const bool allow = tls_allow_;

    using Fn = std::decay_t<F>;
    auto sp = std::make_shared<Fn>(std::forward<F>(f));

    return ex_->run_ret([&, kk, sp, allow]() -> std::invoke_result_t<Fn &> {
      using R = std::invoke_result_t<Fn &>;
      tbb::task_arena limited(kk);

      if constexpr (std::is_void_v<R>) {
        limited.execute([&] {
          Scope::Worker ws(allow);
          std::invoke(*sp);
        });
        return;
      } else if constexpr (std::is_reference_v<R>) {
        using NR = std::remove_reference_t<R>;
        NR *ptr = nullptr;

        limited.execute([&] {
          Scope::Worker ws(allow);
          ptr = std::addressof(std::invoke(*sp));
        });

        return *ptr;
      } else {
        std::optional<R> out;

        limited.execute([&] {
          Scope::Worker ws(allow);
          out.emplace(std::invoke(*sp));
        });

        return std::move(*out);
      }
    });
  }

  void bg(std::function<void()> f) {
    if (!f)
      return;

    bgrd_total.fetch_add(1, std::memory_order_relaxed);
    if (!ex_) {
      f();
      return;
    }

    const bool ok = ex_->enqueue_bg(std::move(f));
    if (!ok)
      bgrd_dropped.fetch_add(1, std::memory_order_relaxed);
  }

  static Ptr make_flow_scheduler(int total_threads,
                                 int small_span_threshold = 2000,
                                 bool reserve_bg = true) {
    auto exec = std::make_shared<Executor>(total_threads, reserve_bg);
    return std::make_shared<Scheduler>(exec, -1, small_span_threshold);
  }

  int admitted_jobs() const noexcept {
    return counters_->jobs.load(std::memory_order_relaxed);
  }

  int active_regions() const noexcept {
    return counters_->regions.load(std::memory_order_relaxed);
  }

  int active_scopes() const noexcept {
    return active_scopes_.load(std::memory_order_relaxed);
  }

  BgStats bg_stats() const noexcept {
    return BgStats{bgrd_total.load(std::memory_order_relaxed),
                   bgrd_dropped.load(std::memory_order_relaxed)};
  }

private:
  // ---- TLS job-scope gating ----
  struct Scope {
    struct Job {
      Scheduler *s{nullptr};
      JobToken tok{};
      TlsFrame frame{};
      bool counted{false};

      explicit Job(Scheduler &sched) : s(&sched) {
        if (!tls_active_) {
          tok = s->enter_job_();
          frame.engage(static_cast<bool>(tok)); // rejected => tls_allow_=false
          s->active_scopes_.fetch_add(1, std::memory_order_relaxed);
          counted = true;
        }
      }

      Job(const Job &) = delete;
      Job &operator=(const Job &) = delete;
      Job(Job &&) = delete;
      Job &operator=(Job &&) = delete;

      ~Job() {
        if (counted) {
          s->active_scopes_.fetch_sub(1, std::memory_order_relaxed);
        }
      }
    };

    struct Worker {
      TlsFrame frame{};
      explicit Worker(bool allow) {
        if (!tls_active_)
          frame.engage(allow);
      }

      Worker(const Worker &) = delete;
      Worker &operator=(const Worker &) = delete;
      Worker(Worker &&) = delete;
      Worker &operator=(Worker &&) = delete;
    };
  };

  template <class I> static I pick_grainsize_(I n, int k_threads_per_job) {
    const int blocks = std::max(1, k_threads_per_job * 4);
    const long long nn = static_cast<long long>(n);
    long long gs_ll = (nn + blocks - 1) / blocks;
    I gs = static_cast<I>(gs_ll);

    const I kMin = static_cast<I>(64);
    const I kMax = static_cast<I>(2048);

    gs = std::max(gs, kMin);
    gs = std::min(gs, kMax);
    if (gs > n)
      gs = std::max<I>(I(1), n);

    return std::max<I>(I(1), gs);
  }

  JobToken enter_job_();
  bool allow_parallel_(size_t span_hint) const noexcept;

private:
  Executor::Ptr ex_{nullptr};
  int max_jobs_{1};
  int small_cutoff_{2048};
  std::shared_ptr<Counters> counters_{std::make_shared<Counters>()};
  std::atomic<size_t> bgrd_dropped{0};
  std::atomic<size_t> bgrd_total{0};
};
} // namespace flowcontrol
#endif
