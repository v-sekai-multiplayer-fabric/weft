// Taskweft planning state — pure C++20, no Godot dependency.
// Uses tsl::ordered_map to preserve JSON insertion order for deterministic
// key iteration (matches Python dict ordering).
#pragma once
#include "tw_rebac.hpp"
#include "tw_value.hpp"
#include "thirdparty/tsl_ordered_map.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct TwState {
    tsl::ordered_map<std::string, TwValue> vars;
    // ReBAC graph for goal satisfaction and action/method guards (hasCapability,
    // team membership, delegation, ...). Immutable once built at domain-load
    // time and shared (not deep-copied) across every state fork — a shared_ptr
    // so `copy()` below can propagate it via ordinary default copy semantics
    // instead of an explicit field-by-field allow-list that's easy to forget
    // to update (a `TwReBACGraph` value member here previously was, silently,
    // for every field added after `vars`).
    std::shared_ptr<const TwReBAC::TwReBACGraph> rebac_graph;
    int rebac_fuel = 8;

    // Memoized canonical signature hash. Invalidated on any state mutation.
    mutable bool     sig_hash_valid = false;
    mutable uint64_t sig_hash_cache = 0;

    void set_var(const std::string &key, TwValue val) {
        vars[key] = std::move(val);
        sig_hash_valid = false;
    }

    TwValue get_var(const std::string &key) const {
        auto it = vars.find(key);
        return it != vars.end() ? it->second : TwValue{};
    }

    bool has_var(const std::string &key) const { return vars.count(key) > 0; }

    // set_nested always creates a new Dict entry so copies share nothing.
    void set_nested(const std::string &var, const TwValue &key, TwValue val) {
        TwValue::Dict dict;
        if (vars.count(var) && vars.at(var).is_dict())
            dict = vars.at(var).as_dict();
        dict[key.to_string()] = std::move(val);
        vars[var] = TwValue(std::move(dict));
        sig_hash_valid = false;
    }

    TwValue get_nested(const std::string &var, const TwValue &key) const {
        auto it = vars.find(var);
        if (it == vars.end() || !it->second.is_dict()) return TwValue{};
        auto &d = it->second.as_dict();
        auto kit = d.find(key.to_string());
        return kit != d.end() ? kit->second : TwValue{};
    }

    bool has_nested(const std::string &var, const TwValue &key) const {
        auto it = vars.find(var);
        if (it == vars.end() || !it->second.is_dict()) return false;
        return it->second.as_dict().count(key.to_string()) > 0;
    }

    // Full member-wise copy (TwValue's copy constructor deep-copies `vars`;
    // `rebac_graph` is a shared_ptr, so this is a cheap refcount bump, not a
    // graph deep-copy). Deliberately *not* a field-by-field allow-list — the
    // previous version only copied `vars`, silently dropping `rebac_graph`
    // (and leaving every state after the first fork unable to satisfy a ReBAC
    // goal binding) because nobody remembered to add the new field here.
    // `sig_hash_valid`/`sig_hash_cache` copying along is correct too, not
    // just harmless: the hash is still valid for an unmutated copy, and
    // set_var/set_nested already invalidate it the moment the copy diverges.
    std::shared_ptr<TwState> copy() const {
        return std::make_shared<TwState>(*this);
    }

    // Deterministic string fingerprint for cycle detection.
    std::string signature() const {
        std::string s;
        std::vector<std::string> keys;
        keys.reserve(vars.size());
        for (auto &[k, _] : vars) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (auto &k : keys) {
            s += k; s += '='; s += vars.at(k).to_string(); s += ';';
        }
        return s;
    }

    // Fast deterministic hash for planner memoization keys.
    // Canonicalizes by sorted top-level variable name so equivalent states
    // reached through different insertion orders still collide intentionally.
    uint64_t signature_hash() const {
        if (sig_hash_valid) return sig_hash_cache;

        uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
        auto mix_str = [&h](const std::string &s) {
            for (unsigned char c : s) {
                h ^= static_cast<uint64_t>(c);
                h *= 1099511628211ull;
            }
            h ^= 0x9e3779b97f4a7c15ull;
            h *= 1099511628211ull;
        };

        std::vector<std::string> keys;
        keys.reserve(vars.size());
        for (const auto &[k, _] : vars) keys.push_back(k);
        std::sort(keys.begin(), keys.end());

        for (const std::string &k : keys) {
            mix_str(k);
            auto it = vars.find(k);
            h ^= it->second.stable_hash();
            h *= 1099511628211ull;
        }
        sig_hash_cache = h;
        sig_hash_valid = true;
        return sig_hash_cache;
    }
};
