// Witness oracle: goal-reachability pre-check using Plausible-inspired
// property-based testing with iterative deepening (QuickCheck-style).
//
// This provides a sound generalization over fixed-depth DFS: instead of
// bounding a generic walk, it formulates each witness query as a universal
// property (∀ w ∈ Fin N, ¬candidate_is_witness w) and searches for a
// counterexample. The iterative deepening ladder starts cheap (small Fin
// window, few instances) and escalates only when necessary.
//
// For TOHTN domains the witness predicate is: "does the first goal-method
// make measurable progress on the unsatisfied binding?"  When the oracle
// reports provably-none, the planner skips the branch — sound because all
// goal methods are tried eventually.

#pragma once
#include "tw_domain.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Plausible {

// ── PCG64 fast PRNG ──────────────────────────────────────────────────────────────
//
// Fast, good-quality PRNG for the inner loop. Not crypto — but matches what
// Lean's StdGen does after seeding: generate locally without OS calls.
//
// State update: state = state * multiplier + increment
// Output: PCG-XSL-RR-Variant (good statistical quality)

class Rng {
    uint64_t state;
    uint64_t inc;

    // PCG64 state transition — fast, no external calls
    void step() {
        state = state * 6364136223846793005ULL + inc;
    }

    // PCG-XSL-RR-Variant output — good 64-bit uniform
    uint64_t output() {
        uint64_t old = state;
        step();
        uint64_t xorshifted = ((old >> 18) ^ old) >> 27;
        uint64_t rot = old >> 59;
        return (xorshifted >> rot) | (xorshifted << (64 - rot));
    }

public:
    explicit Rng(std::optional<uint64_t> seed = std::nullopt) {
        if (seed) {
            init_fixed(*seed);
            return;
        }
        init_csprng();
    }

    // Fixed seed — zero CSPRNG calls (benchmarks)
    void init_fixed(uint64_t s) {
        state = s * 6364136223846793005ULL + 1442695040888963407ULL;
        inc = s + 1;
    }

    // Seed from OS CSPRNG once — matches Lean's IO.random → StdGen
    void init_csprng() {
        std::random_device rd;
        state = rd();
        inc = state + 1;
    }

    uint64_t rand(uint64_t n) {
        if (n <= 1) return 0;
        uint64_t max_val = UINT64_MAX - UINT64_MAX % n;
        while (true) {
            uint64_t r = output();
            if (r < max_val) return r % n;
        }
    }
};

// ── TestResult ───────────────────────────────────────────────────────────────────

template<typename P>
struct TestResult {
    enum Kind { success, failure, gave_up } kind;

    struct Success {};
    struct Failure {
        std::vector<std::string> trace;
    };
    struct GaveUp {
        uint64_t n;
    };

    std::variant<Success, Failure, GaveUp> data;

    bool is_failure() const {
        return std::holds_alternative<Failure>(data);
    }
};

// ── Testable ────────────────────────────────────────────────────────────────────

template<typename P>
struct Testable {
    virtual ~Testable() = default;
    virtual TestResult<P> run(Rng &rng, uint64_t num_inst) const = 0;
};

// ── ForallFin<N> — universal quantifier over Fin N candidates ──────────────────

template<uint64_t N>
struct ForallFin : Testable<bool> {
    using Predicate = std::function<bool(uint64_t)>;
    Predicate pred;

    TestResult<bool> run(Rng &rng, uint64_t num_inst) const override {
        TestResult<bool> result;
        result.kind = TestResult<bool>::gave_up;
        result.data = TestResult<bool>::GaveUp{num_inst};

        for (uint64_t i = 0; i < num_inst; ++i) {
            uint64_t w = rng.rand(N);
            bool holds = pred(w);
            if (!holds) {
                result.kind = TestResult<bool>::failure;
                result.data = TestResult<bool>::Failure{
                    {std::string("w=") + std::to_string(w)}};
                return result;
            }
        }

        result.kind = TestResult<bool>::success;
        result.data = TestResult<bool>::Success{};
        return result;
    }
};

} // namespace Plausible

// ── PlausibleWitnessDag ──────────────────────────────────────────────────────────

namespace PlausibleWitnessDag {

struct Level {
    uint64_t idx       = 0;
    uint64_t walkSteps = 0;
    uint64_t finBound  = 0;
    uint64_t numInst   = 0;
};

inline std::vector<Level> ladder() {
    return {
        {0, 64, 256, 200},
        {1, 512, 1024, 800},
        {2, 4000, 4096, 2000},
    };
}

struct Readback {
    uint64_t value     = 0;
    bool     found     = false;
    uint64_t witnessIdx = 0;
    bool     budgetHit = false;
};

struct Outcome {
    enum Kind { found, provably_none, budgetHit } kind;
    uint64_t witness_idx = 0;
};

struct TraceEntry {
    std::string query;
    uint64_t    level;
    Outcome     outcome;
};

using ReadbackFn = std::function<Readback(uint64_t)>;

inline std::tuple<uint64_t, uint64_t, TraceEntry>
resolve(const std::string &query,
        std::function<bool(uint64_t)> candidate_is_witness,
        ReadbackFn readback,
        std::vector<Level> levels = ladder()) {

    uint64_t chosen = 0;
    uint64_t lvl_idx = 0;
    Outcome outcome = {Outcome::provably_none, 0};
    bool resolved = false;

    for (const Level &lvl : levels) {
        if (!resolved) {
            Plausible::Rng cert_rng(std::nullopt);

            if (lvl.finBound == 256) {
                Plausible::ForallFin<256> test;
                test.pred = [&](uint64_t w) { return !candidate_is_witness(w); };
                Plausible::TestResult<bool> tr = test.run(cert_rng, lvl.numInst);

                if (tr.is_failure()) {
                    outcome.kind = Outcome::found;
                    outcome.witness_idx = 0;
                    resolved = true;
                } else {
                    Readback rb = readback(lvl.walkSteps);
                    lvl_idx = lvl.idx;
                    chosen = rb.value;

                    if (rb.found) {
                        outcome.kind = Outcome::found;
                        outcome.witness_idx = rb.witnessIdx;
                        resolved = true;
                    } else if (!rb.budgetHit) {
                        outcome.kind = Outcome::provably_none;
                        resolved = true;
                    } else {
                        outcome.kind = Outcome::budgetHit;
                    }
                }
            } else if (lvl.finBound == 1024) {
                Plausible::ForallFin<1024> test;
                test.pred = [&](uint64_t w) { return !candidate_is_witness(w); };
                Plausible::TestResult<bool> tr = test.run(cert_rng, lvl.numInst);

                if (tr.is_failure()) {
                    outcome.kind = Outcome::found;
                    outcome.witness_idx = 0;
                    resolved = true;
                } else {
                    Readback rb = readback(lvl.walkSteps);
                    lvl_idx = lvl.idx;
                    chosen = rb.value;

                    if (rb.found) {
                        outcome.kind = Outcome::found;
                        outcome.witness_idx = rb.witnessIdx;
                        resolved = true;
                    } else if (!rb.budgetHit) {
                        outcome.kind = Outcome::provably_none;
                        resolved = true;
                    } else {
                        outcome.kind = Outcome::budgetHit;
                    }
                }
            } else {
                Plausible::ForallFin<4096> test;
                test.pred = [&](uint64_t w) { return !candidate_is_witness(w); };
                Plausible::TestResult<bool> tr = test.run(cert_rng, lvl.numInst);

                if (tr.is_failure()) {
                    outcome.kind = Outcome::found;
                    outcome.witness_idx = 0;
                    resolved = true;
                } else {
                    Readback rb = readback(lvl.walkSteps);
                    lvl_idx = lvl.idx;
                    chosen = rb.value;

                    if (rb.found) {
                        outcome.kind = Outcome::found;
                        outcome.witness_idx = rb.witnessIdx;
                        resolved = true;
                    } else if (!rb.budgetHit) {
                        outcome.kind = Outcome::provably_none;
                        resolved = true;
                    } else {
                        outcome.kind = Outcome::budgetHit;
                    }
                }
            }
        }
    }

    TraceEntry te;
    te.query = query;
    te.level = lvl_idx;
    te.outcome = outcome;

    return {chosen, lvl_idx, te};
}

} // namespace PlausibleWitnessDag
