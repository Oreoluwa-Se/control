# flowcontrol

A small C++17 concurrency “flow control” layer built on **oneTBB** that helps prevent **oversubscription** and runaway nested parallelism by:

* **Hard-capping** total parallelism (process-wide) via `tbb::global_control`
* Running foreground work inside a bounded **task_arena**
* Providing a `Scheduler` that automatically throttles parallel regions across concurrent “top jobs”
* Supporting a simple background execution lane for maintenance work

This is intended for production workloads where multiple independent calls can trigger nested `tbb::parallel_*` algorithms and you want consistent CPU usage and predictable latency.

---

## Features

* **Hard cap** TBB parallelism (`tbb::global_control::max_allowed_parallelism`)
* Foreground execution via `Executor::run()` / `Executor::run_ret()`
* Background lane via `Executor::enqueue_bg()` (single dedicated worker thread)
* `Scheduler` helpers:

  * `pfor` (parallel-for with serial fallback)
  * `preduce` / `preduce_range` (parallel-reduce with serial fallback)
  * `pinvoke` (parallel invoke with serial fallback)
  * `with_concurrency(k, ...)` to run a block under a `k`-thread `task_arena`
* Automatic “top-job” accounting (TLS-based) to keep concurrency sane even when callers don’t explicitly “enter a job”

---

## Requirements

* CMake >= 3.25
* C++17 compiler (GCC/Clang/MSVC)
* oneTBB (system install or auto-fetched)

---

## Quick start (build + run CLI)

```bash
./run.sh -t Release -R
```

---

## Build and run tests

Tests live in a header (`include/test/check.hpp`) and are executed via `src/test_runner.cpp`.

```bash
./run.sh -t Debug --tests=ON
```

---

## Build as a library

Build only the library (no CLI):

```bash
./run.sh --app=OFF -t Release
```

Build shared library:

```bash
./run.sh --shared=ON --app=OFF -t Release
```

Or use CMake’s shared default behavior:

```bash
./run.sh --shared-default --app=OFF -t Release
```

---

## Install

Install to a prefix:

```bash
./run.sh --install=/usr/local
```

Install with sudo:

```bash
./run.sh --install=/usr/local --sudo
```

This installs:

* headers to `<prefix>/include`
* library to `<prefix>/lib`
* CMake package config to `<prefix>/lib/cmake/flowcontrol`

---

## Use from another CMake project

### Option A: `find_package` (after install)

```cmake
find_package(flowcontrol REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE flowcontrol::flowcontrol)
```

### Option B: `FetchContent` (direct from GitHub)

```cmake
include(FetchContent)

FetchContent_Declare(
  flowcontrol
  GIT_REPOSITORY https://github.com/<your-org>/flowcontrol.git
  GIT_TAG v0.1.0
)

FetchContent_MakeAvailable(flowcontrol)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE flowcontrol::flowcontrol)
```

Notes:

* When used as a **subproject**, flowcontrol defaults to **STATIC** unless the parent opts into shared.
* oneTBB will be fetched automatically if not found on the system.

---

## API sketch

### Executor

`Executor(total_threads, reserve_bg=true)`

* `total_threads` hard-caps total process parallelism (TBB)
* If `reserve_bg=true`, foreground uses `total_threads - 1` and background uses 1 thread

Foreground:

* `run(f)` executes `f` inside the FG arena
* `run_ret(f)` executes `f`, returns the result, and propagates exceptions

Background:

* `enqueue_bg(f)` queues work on a dedicated background thread (returns `false` if shutting down)
* `shutdown()` drains BG and stops the worker

### Scheduler

`Scheduler` is a “flow controller” layered on top of the Executor:

* Serial fallback when:

  * job admission is rejected (system saturated)
  * work span is below threshold
  * parallel budgets are exceeded (jobs/regions)
* Uses TLS so concurrent callers get safe throttling even without explicit job scopes

Example usage:

```cpp
#include <tbb/parallel_sort.h>
#include <tbb/parallel_for.h>
#include <memory>
#include <vector>

auto ex = std::make_shared<flowcontrol::Executor>(8, true);
flowcontrol::Scheduler sched(ex, /*max_parallel_jobs=*/4, /*small_span_threshold=*/2048);

// Parallel-for with throttling / serial fallback
sched.pfor<int>(0, N, [&](int i) {
  // work
});

// Parallel reduction with throttling / serial fallback
auto sum = sched.preduce<int, int>(
  0, N, 0,
  [&](int i){ return i; },
  [&](int a, int b){ return a + b; }
);

// Parallel invoke with throttling / serial fallback
sched.pinvoke(10000,
  [&]{ /* task A */ },
  [&]{ /* task B */ }
);

// Limit a block (including TBB algorithms) to k threads
sched.with_concurrency(2, [&] {
  tbb::parallel_sort(v.begin(), v.end());
});
```

---

## Design notes (high level)

* `tbb::global_control` sets an upper bound for the process.
* `tbb::task_arena` provides a local concurrency limit for foreground blocks.
* The `Scheduler` maintains lightweight state:

  * admitted jobs
  * active regions
  * active TLS scopes (auto top jobs)
* Parallel helpers fall back to serial when parallelism would be unsafe or counterproductive.

---

## Development

Enable sanitizers:

```bash
./run.sh -t Debug --sanitizers=ON
```

Verbose build:

```bash
./run.sh -v
```

Clean build directory:

```bash
./run.sh -C
```

---

## License

Apache-2.0

If you publish this, add a `LICENSE` file containing the Apache License 2.0 text and (optionally) add SPDX headers such as:

```cpp
// SPDX-License-Identifier: Apache-2.0
```
