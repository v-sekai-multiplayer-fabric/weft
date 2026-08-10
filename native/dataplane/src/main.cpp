// Throughput smoke for the weft data-plane ring. It pins one thread per core, each with
// its own ring (share-nothing), writes many snapshots, and reports the aggregate rate.
// This mirrors bench/ring_native.c and builds on both Windows and Linux. C++ over
// iceoryx2, no Rust.
//
//   weft-dataplane [iterations_per_thread] [threads]

#include "ring.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
static bool pin_to_core(unsigned index) {
  DWORD_PTR mask = static_cast<DWORD_PTR>(1) << index;
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
}
#else
#include <pthread.h>
#include <sched.h>
static bool pin_to_core(unsigned index) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(index, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}
#endif

static void writer(unsigned core, std::uint64_t iterations) {
  pin_to_core(core);
  Ring ring(8);
  std::vector<std::int32_t> coords(24);
  for (int i = 0; i < 24; ++i) {
    coords[i] = i;
  }
  for (std::uint64_t tick = 0; tick < iterations; ++tick) {
    coords[0] = static_cast<std::int32_t>(tick);
    ring.write(tick, coords.data(), coords.size());
  }
}

int main(int argc, char** argv) {
  std::uint64_t iterations =
      (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 50000000ULL;
  unsigned threads = (argc > 2) ? static_cast<unsigned>(std::atoi(argv[2])) : 1u;
  if (threads < 1) {
    threads = 1;
  }

  auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back(writer, t, iterations);
  }
  for (auto& th : pool) {
    th.join();
  }
  auto end = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(end - start).count();
  double total = static_cast<double>(iterations) * threads;
  double rate = total / secs / 1e6;
  double per_core = rate / threads;
  std::printf("threads=%u, aggregate=%.1f M snapshots/sec, per-core=%.1f M/sec\n",
              threads, rate, per_core);
  return 0;
}
