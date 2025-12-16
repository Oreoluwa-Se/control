#include "cap/executor.hpp"
#include "cap/scheduler.hpp"

namespace {
constexpr int MIN_THREADS_COUNT = 2;
}

using namespace flowcontrol;

Executor::Executor(int total_threads, bool reserve_bg)
    : total_(normalize_(total_threads)),
      reserve_bg_(reserve_bg && total_ >= MIN_THREADS_COUNT),
      ctrl_(tbb::global_control::max_allowed_parallelism,
            static_cast<size_t>(reserve_bg_ ? (total_ - 1) : total_)),
      fg_(reserve_bg_ ? (total_ - 1) : total_) {
  if (reserve_bg_) {

    bg_thread_ = std::thread([this] {
      tbb::task_arena bg_limit(1);

      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lk(bg_mu_);
          bg_cv_.wait(lk, [&] { return bg_stop_ || !bgq_.empty(); });

          if (bg_stop_ && bgq_.empty())
            break;

          task = std::move(bgq_.front());
          bgq_.pop_front();
        }

        try {
          bg_limit.execute(
              [&] { tbb::this_task_arena::isolate([&] { task(); }); });
        } catch (...) { /* swallowed*/
        }

        inflight_.fetch_sub(1, std::memory_order_acq_rel);
        bg_cv_.notify_all();
      }
    });
  }
}

Executor::~Executor() noexcept { shutdown(); }

int Executor::fg_threads() const noexcept {
  return reserve_bg_ ? (total_ - 1) : total_;
}

int Executor::bg_threads() const noexcept { return reserve_bg_ ? 1 : 0; }

void Executor::run_many(std::vector<std::function<void()>> tasks) {
  if (tasks.empty())
    return;

  run([&] {
    tbb::task_group tg;
    for (auto &fn : tasks)
      tg.run(std::move(fn));
    tg.wait();
  });
}

bool Executor::enqueue_bg(std::function<void()> f) {
  if (!f)
    return true;

  if (!reserve_bg_) {
    std::invoke(f);
    return true;
  }

  // Reject if shutdown started
  if (shutting_.load(std::memory_order_acquire))
    return false;

  inflight_.fetch_add(1, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lk(bg_mu_);

    // Re-check under lock to avoid racing shutdown + queueing.
    if (shutting_.load(std::memory_order_acquire)) {
      inflight_.fetch_sub(1, std::memory_order_acq_rel);
      bg_cv_.notify_all();
      return false;
    }

    bgq_.push_back(std::move(f));
  }

  bg_cv_.notify_one();
  return true;
}

void Executor::wait_bg_idle() {
  if (!reserve_bg_)
    return;

  std::unique_lock<std::mutex> lk(bg_mu_);
  bg_cv_.wait(lk, [&] {
    return inflight_.load(std::memory_order_acquire) == 0 && bgq_.empty();
  });
}

void Executor::shutdown() noexcept {

  shutting_.store(true, std::memory_order_release);
  wait_bg_idle();

  // handle closing background thread
  if (reserve_bg_ && bg_thread_.joinable()) {
    // Ensure thread exits
    {
      std::lock_guard<std::mutex> lk(bg_mu_);
      bg_stop_ = true;
    }
    bg_cv_.notify_all();
    bg_thread_.join();
  }
}

int Executor::normalize_(int n) noexcept {
  if (n > 0)
    return n;

  std::cout << "[Executor::normalize]: No thread count given; "
               "defaulting to system thread count\n";

  const unsigned hc = std::thread::hardware_concurrency();
  int def = (hc > 0) ? static_cast<int>(hc) : MIN_THREADS_COUNT;

  if (def < MIN_THREADS_COUNT)
    def = MIN_THREADS_COUNT;

  return def;
}

/* ---------- Scheduler Class ---------- */
Scheduler::Scheduler(Executor::Ptr ex, int max_parallel_jobs,
                     int small_span_threshold)
    : ex_(std::move(ex)), max_jobs_([&] {
        if (!ex_)
          return 1;
        const int fg = ex_->fg_threads();

        if (max_parallel_jobs > 0)
          return std::min(max_parallel_jobs, fg);

        return std::max(1, fg / 2);
      }()),
      small_cutoff_(small_span_threshold) {}

int Scheduler::int_est_thread_per() const noexcept {
  if (!ex_)
    return 1;

  const int fg = ex_->fg_threads();
  const int admitted = counters_->jobs.load(std::memory_order_relaxed);
  const int active = active_scopes_.load(std::memory_order_relaxed);

  const int denom = std::max(1, std::max(admitted, active));

  int k = fg / denom;
  return std::clamp(k, 1, fg);
}

/* -------------- Helper Functions ---------------- */
Scheduler::JobToken Scheduler::enter_job_() {
  int curr = counters_->jobs.load(std::memory_order_relaxed);
  while (curr < max_jobs_) {
    if (counters_->jobs.compare_exchange_weak(curr, curr + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
      // Job has been admitted and we incremented the counter
      return JobToken{counters_, true};
    }
  }

  // job not admitted no slots available essentially
  return JobToken{counters_, false};
}

bool Scheduler::allow_parallel_(size_t span_hint) const noexcept {
  if (!ex_ || ex_->fg_threads() <= 1)
    return false;

  if (span_hint < static_cast<size_t>(small_cutoff_))
    return false;

  const int fg = ex_->fg_threads();
  const int regions = counters_->regions.load(std::memory_order_relaxed);

  const int admitted_jobs =
      counters_->jobs.load(std::memory_order_relaxed); // admitted only

  const int active_scopes = active_scopes_.load(
      std::memory_order_relaxed); // admitted + rejected top scopes

  // With Scope::Job at helper entrypoints this should be rare.
  if (admitted_jobs <= 0 && active_scopes <= 0)
    return regions < fg;

  // Only admitted jobs can start parallel regions.
  if (admitted_jobs <= 0)
    return false;

  // Account for rejected-but-active scopes that are still consuming CPU
  // serially.
  const int effective_jobs =
      std::max(1, std::max(admitted_jobs, active_scopes));

  const int k = fg / effective_jobs;
  if (k < 2)
    return false;

  // Region budget tied to admitted jobs
  const int region_budget = std::min(admitted_jobs, fg);
  return regions < region_budget;
}
