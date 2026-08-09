#pragma once
// weft data plane, native C++.
//
// Single-writer seqlock ring, the same layout and method as bench/ring_native.c and
// Weft.DataPlane.Ring. The sequence is atomic and gives the ordering. The data is plain,
// so the store loop vectorizes. One thread per core writes or reads. This is C++ over
// Eclipse iceoryx v1. The project does not use Rust. See docs/runtime-choice.md.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class Ring {
public:
  explicit Ring(std::size_t max_entities) : data_(max_entities * 3, 0) {}

  std::size_t slots() const { return data_.size(); }

  // Write one snapshot. The sequence is odd during the write and even after it. The
  // release on the even store publishes the data.
  void write(std::uint64_t tick, const std::int32_t* coords, std::size_t n) {
    std::uint64_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    tick_ = tick;
    const std::size_t m = n < data_.size() ? n : data_.size();
    std::int32_t* __restrict d = data_.data();
    const std::int32_t* __restrict c = coords;
    for (std::size_t i = 0; i < m; ++i) {
      d[i] = c[i];
    }
    seq_.store(s + 2, std::memory_order_release);
  }

  // Read the latest snapshot into out. Retries while a write is in progress or the
  // sequence changed. Returns the tick of the snapshot that was read.
  std::uint64_t read(std::int32_t* out, std::size_t n) const {
    for (;;) {
      std::uint64_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 & 1u) {
        continue;
      }
      std::uint64_t tick = tick_;
      const std::size_t m = n < data_.size() ? n : data_.size();
      const std::int32_t* __restrict d = data_.data();
      std::int32_t* __restrict o = out;
      for (std::size_t i = 0; i < m; ++i) {
        o[i] = d[i];
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) == s1) {
        return tick;
      }
    }
  }

private:
  std::atomic<std::uint64_t> seq_{0};
  std::uint64_t tick_{0};
  std::vector<std::int32_t> data_;
};
