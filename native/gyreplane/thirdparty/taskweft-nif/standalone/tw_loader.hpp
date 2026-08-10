// Taskweft JSON-LD domain loader — pure C++20, no Godot dependency.
// Includes a minimal recursive-descent JSON parser and the domain builder.
#pragma once
#include "tw_domain.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>

// M_PI and M_E are not defined on Windows with MSVC/clang without _USE_MATH_DEFINES.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.71828182845904523536
#endif
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TwLoader {

// ---- JSON parser — canonical implementation is in tw_json.hpp ---------------
// tw_json.hpp is included transitively:
//   tw_loader.hpp → tw_domain.hpp → tw_state.hpp → tw_rebac.hpp → tw_json.hpp
// Using-declarations re-export into TwLoader so existing callers are unchanged.
using TwJson::skip_ws;
using TwJson::parse_json_string;
using TwJson::parse_json_number;
using TwJson::parse_json;
using TwJson::parse_json_str;

// ---- Expression evaluators -------------------------------------------------
// KHR_interactivity §02 node types — tinygrad-style flat dispatch registry.
// References:
//   @software{tinygrad2020,
//     author = {Hoeger, George and tinygrad contributors},
//     title  = {tinygrad},
//     url    = {https://github.com/tinygrad/tinygrad},
//     year   = {2020},
//     note   = {Op-registry dispatch pattern: flat unordered_map from type
//               string to callable, avoiding class hierarchies.}
//   }

using Params = std::unordered_map<std::string, TwValue>;

// "{name}"        → params[name] (type preserved).
// "{a}_{b}", "x{a}", etc. → string interpolation of every "{var}" against
//   params, result is a TwValue::STRING. Unknown vars are left literal.
// Anything else returned as-is.
inline TwValue resolve_param(const TwValue &val, const Params &params) {
    if (!val.is_string()) return val;
    const auto &s = val.as_string();

    // Fast path: the entire string is a single "{name}" — preserve the
    // bound value's type so non-string params (ints, floats) flow through.
    if (s.size() >= 3 && s.front() == '{' && s.back() == '}' &&
            s.find('{', 1) == std::string::npos) {
        std::string name = s.substr(1, s.size() - 2);
        auto it = params.find(name);
        if (it != params.end()) return it->second;
        return val;
    }

    // No braces at all → no substitution.
    if (s.find('{') == std::string::npos) return val;

    // Multi-substitution: scan and stringify every "{var}".
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t end = s.find('}', i + 1);
            if (end == std::string::npos) {
                out.append(s, i, std::string::npos);
                break;
            }
            std::string name = s.substr(i + 1, end - i - 1);
            auto it = params.find(name);
            if (it != params.end()) out += it->second.to_string();
            else out.append(s, i, end - i + 1);
            i = end + 1;
        } else {
            out += s[i++];
        }
    }
    return TwValue(out);
}

// RFC 6901 §3 — escape a string so it represents one reference token.
// Order matters: '~' must be encoded before '/'.
inline std::string escape_rfc6901(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

// RFC 6901 §3 — a reference token is well-formed iff every '~' is followed
// by '0' or '1'. Anything else is an "error condition" per §7.
inline bool is_valid_rfc6901_token(const std::string &t) {
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] != '~') continue;
        if (i + 1 >= t.size()) return false;
        char nxt = t[i + 1];
        if (nxt != '0' && nxt != '1') return false;
        ++i;
    }
    return true;
}

// RFC 6901 §4 — decode a single reference token. Per spec: replace every
// '~1' first, then every '~0'. Order is significant: doing '~0' first would
// turn '~01' into '~1' which would then become '/' (wrong; must be '~1').
// Caller is responsible for validating the token first.
inline std::string unescape_rfc6901(const std::string &t) {
    std::string mid;
    mid.reserve(t.size());
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '~' && i + 1 < t.size() && t[i + 1] == '1') {
            mid += '/';
            ++i;
        } else {
            mid += t[i];
        }
    }
    std::string out;
    out.reserve(mid.size());
    for (size_t i = 0; i < mid.size(); i++) {
        if (mid[i] == '~' && i + 1 < mid.size() && mid[i + 1] == '0') {
            out += '~';
            ++i;
        } else {
            out += mid[i];
        }
    }
    return out;
}

// Substitute '{name}' templates in a raw pointer string against `params`,
// escaping each substituted value per RFC 6901 §3 so that values containing
// '/' or '~' map to a single reference token rather than splitting the path.
// Unknown vars are left literal. Multiple templates per segment are allowed.
inline std::string substitute_pointer(const std::string &raw, const Params &params) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '{') {
            size_t end = raw.find('}', i + 1);
            if (end == std::string::npos) {
                out.append(raw, i, std::string::npos);
                break;
            }
            std::string name = raw.substr(i + 1, end - i - 1);
            auto it = params.find(name);
            if (it != params.end()) out += escape_rfc6901(it->second.to_string());
            else out.append(raw, i, end - i + 1);
            i = end + 1;
        } else {
            out += raw[i++];
        }
    }
    return out;
}

// RFC 6901 — parse a JSON Pointer into decoded reference tokens.
// Empty string → {} (whole-document reference).
// Returns {} for any error condition: missing leading '/' or a token with a
// '~' not followed by '0' or '1' (per §3 ABNF).
inline std::vector<std::string> parse_rfc6901(const std::string &ptr) {
    if (ptr.empty()) return {};
    if (ptr[0] != '/') return {};
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&](std::vector<std::string> &acc) -> bool {
        if (!is_valid_rfc6901_token(cur)) return false;
        acc.push_back(unescape_rfc6901(cur));
        cur.clear();
        return true;
    };
    for (size_t i = 1; i < ptr.size(); i++) {
        if (ptr[i] == '/') {
            if (!flush(tokens)) return {};
        } else {
            cur += ptr[i];
        }
    }
    if (!flush(tokens)) return {};
    return tokens;
}

// Resolve a templated pointer into Taskweft's 2-segment (var, key) shape.
// Substitutes '{var}' (auto-escaping per RFC 6901), then parses per RFC 6901.
// Returns {"", nil} unless the pointer decodes to exactly two tokens.
inline std::pair<std::string, TwValue> parse_pointer(
        const std::string &ptr, const Params &params) {
    auto tokens = parse_rfc6901(substitute_pointer(ptr, params));
    if (tokens.size() != 2) return {"", TwValue{}};
    return {std::move(tokens[0]), TwValue(std::move(tokens[1]))};
}

// Forward declaration.
inline TwValue eval_expr(const TwValue &expr, const Params &params,
        const TwState &state, const TwValue::Dict &enums);

// Resolve the KHR_interactivity node type from "type" key.
// Accepts fully qualified "math/add" or short "add".
inline std::string node_type(const TwValue::Dict &expr) {
    auto it = expr.find("type");
    if (it == expr.end() || !it->second.is_string()) return "";
    const std::string &s = it->second.as_string();
    auto slash = s.rfind('/');
    return slash != std::string::npos ? s.substr(slash + 1) : s;
}

// ---- KHR_interactivity node type registry (tinygrad-style flat table) ------
//
// Each entry: short-name → fn(a, b, c, d) → TwValue.
// a = get("a"), b = get("b"), c = get("c"), d = get("d") — pre-evaluated.
// Structural nodes (get, select, clamp, mix, switch, random) handled before
// dispatch because they require access to the full expression dict.
//
// Sources: KHR_interactivity 02_node_types.md (all scalar math/* and type/*).

using NodeFn = std::function<TwValue(TwValue, TwValue, TwValue, TwValue)>;

// ---- Matrix helpers (floatNxN, flat row-major, N in {2,3,4}) ---------------
// Generic NxN via Laplace expansion / adjugate — correct for any N and
// simpler to get right than three hand-specialized 2x2/3x3/4x4 formulas.
// Not performance-critical (interactivity graphs evaluate at authoring/tick
// rate, not per-vertex).

inline std::vector<double> mat_to_doubles(const TwValue &m) {
    std::vector<double> out;
    if (!m.is_array()) return out;
    out.reserve(m.as_array().size());
    for (const auto &v : m.as_array()) out.push_back(v.as_number());
    return out;
}

// Row-major flat length -> matrix dimension; 0 if not a recognized floatNxN.
inline int mat_dim(size_t n) {
    if (n == 4) return 2;
    if (n == 9) return 3;
    if (n == 16) return 4;
    return 0;
}

inline std::vector<double> mat_minor(const std::vector<double> &m, int n, int r, int c) {
    std::vector<double> out; out.reserve((size_t)(n - 1) * (n - 1));
    for (int i = 0; i < n; ++i) {
        if (i == r) continue;
        for (int j = 0; j < n; ++j) {
            if (j == c) continue;
            out.push_back(m[(size_t)i * n + j]);
        }
    }
    return out;
}

inline double mat_det(const std::vector<double> &m, int n) {
    if (n == 1) return m[0];
    if (n == 2) return m[0] * m[3] - m[1] * m[2];
    double det = 0.0;
    for (int j = 0; j < n; ++j) {
        double cof = ((j % 2 == 0) ? 1.0 : -1.0) * m[(size_t)j];
        det += cof * mat_det(mat_minor(m, n, 0, j), n - 1);
    }
    return det;
}

inline std::vector<double> mat_transpose_v(const std::vector<double> &m, int n) {
    std::vector<double> out((size_t)n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            out[(size_t)j * n + i] = m[(size_t)i * n + j];
    return out;
}

inline std::vector<double> mat_mul_v(const std::vector<double> &a, const std::vector<double> &b, int n) {
    std::vector<double> out((size_t)n * n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            double sum = 0.0;
            for (int k = 0; k < n; ++k) sum += a[(size_t)i * n + k] * b[(size_t)k * n + j];
            out[(size_t)i * n + j] = sum;
        }
    return out;
}

// Adjugate / determinant. Per spec: non-finite or zero determinant -> not
// invertible, output matrix is all positive zeros.
inline std::pair<bool, std::vector<double>> mat_inverse_v(const std::vector<double> &m, int n) {
    double det = mat_det(m, n);
    if (det == 0.0 || std::isnan(det) || std::isinf(det))
        return {false, std::vector<double>((size_t)n * n, 0.0)};
    std::vector<double> cof((size_t)n * n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            double sign = ((i + j) % 2 == 0) ? 1.0 : -1.0;
            cof[(size_t)i * n + j] = sign * mat_det(mat_minor(m, n, i, j), n - 1);
        }
    std::vector<double> adj = mat_transpose_v(cof, n);  // adjugate = transpose(cofactor)
    std::vector<double> inv((size_t)n * n);
    for (size_t i = 0; i < inv.size(); ++i) inv[i] = adj[i] / det;
    return {true, inv};
}

inline TwValue mat_to_value(const std::vector<double> &m) {
    TwValue::Array out; out.reserve(m.size());
    for (double d : m) out.push_back(TwValue(d));
    return TwValue(std::move(out));
}

inline const std::unordered_map<std::string, NodeFn> &kNodeTypes() {
    static const std::unordered_map<std::string, NodeFn> tbl = {
        // ---- Arithmetic (float/int polymorphic) ----------------------------
        {"add",  [](auto a, auto b, auto, auto) -> TwValue {
            if (a.is_int() && b.is_int()) return TwValue(a.as_int() + b.as_int());
            return TwValue(a.as_number() + b.as_number()); }},
        {"sub",  [](auto a, auto b, auto, auto) -> TwValue {
            if (a.is_int() && b.is_int()) return TwValue(a.as_int() - b.as_int());
            return TwValue(a.as_number() - b.as_number()); }},
        {"mul",  [](auto a, auto b, auto, auto) -> TwValue {
            if (a.is_int() && b.is_int()) return TwValue(a.as_int() * b.as_int());
            return TwValue(a.as_number() * b.as_number()); }},
        {"div",  [](auto a, auto b, auto, auto) -> TwValue {
            if (a.is_int() && b.is_int()) { int64_t bi = b.as_int(); return bi ? TwValue(a.as_int()/bi) : TwValue{}; }
            double bd = b.as_number(); return bd != 0.0 ? TwValue(a.as_number()/bd) : TwValue{}; }},
        // math/rem — truncated remainder (spec §Remainder; ECMAScript %)
        {"rem",  [](auto a, auto b, auto, auto) -> TwValue {
            if (a.is_int() && b.is_int()) { int64_t bi = b.as_int(); return bi ? TwValue(a.as_int()%bi) : TwValue(int64_t(0)); }
            double bd = b.as_number(); return TwValue(bd != 0.0 ? std::fmod(a.as_number(), bd) : 0.0); }},
        // math/fract — fractional part: a - floor(a)
        {"fract",[](auto a, auto, auto, auto) -> TwValue {
            double v = a.as_number(); return TwValue(v - std::floor(v)); }},
        {"neg",  [](auto a, auto, auto, auto) -> TwValue {
            return a.is_int() ? TwValue(-a.as_int()) : TwValue(-a.as_number()); }},
        {"abs",  [](auto a, auto, auto, auto) -> TwValue {
            return a.is_int() ? TwValue(std::abs(a.as_int())) : TwValue(std::abs(a.as_number())); }},
        {"min",  [](auto a, auto b, auto, auto) -> TwValue { return a < b ? a : b; }},
        {"max",  [](auto a, auto b, auto, auto) -> TwValue { return a > b ? a : b; }},
        {"saturate", [](auto a, auto, auto, auto) -> TwValue {
            double v = a.as_number(); return TwValue(v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v); }},
        // math/smoothStep(a,b,c) — Hermite interpolation, defined in terms of
        // math/min + math/saturate (spec §Smooth Step). See lean/KHRTier1Witness.lean
        // for the witness-certified reference model this mirrors.
        {"smoothStep", [](auto a, auto b, auto c, auto) -> TwValue {
            double av = a.as_number(), bv = b.as_number(), cv = c.as_number();
            double mn = std::min(av, bv);
            double raw = (cv - mn) / std::abs(bv - av);
            double t = raw < 0.0 ? 0.0 : raw > 1.0 ? 1.0 : raw;
            return TwValue(t * t * (3.0 - 2.0 * t)); }},

        // ---- Sign/rounding -------------------------------------------------
        {"sign", [](auto a, auto, auto, auto) -> TwValue {
            if (a.is_int()) { int64_t v = a.as_int(); return TwValue(v > 0 ? int64_t(1) : v < 0 ? int64_t(-1) : int64_t(0)); }
            double v = a.as_number(); return TwValue(v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0); }},
        {"trunc",[](auto a, auto, auto, auto) -> TwValue {
            return a.is_int() ? a : TwValue(std::trunc(a.as_number())); }},
        {"floor",[](auto a, auto, auto, auto) -> TwValue { return TwValue(std::floor(a.as_number())); }},
        {"ceil", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::ceil(a.as_number())); }},
        {"round",[](auto a, auto, auto, auto) -> TwValue {
            double v = a.as_number(); return TwValue(v < 0.0 ? -std::round(-v) : std::round(v)); }},

        // ---- Exponential / logarithmic ------------------------------------
        {"sqrt", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::sqrt(a.as_number())); }},
        {"cbrt", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::cbrt(a.as_number())); }},
        {"exp",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::exp(a.as_number())); }},
        {"log",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::log(a.as_number())); }},
        {"log2", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::log2(a.as_number())); }},
        {"log10",[](auto a, auto, auto, auto) -> TwValue { return TwValue(std::log10(a.as_number())); }},
        {"pow",  [](auto a, auto b, auto, auto) -> TwValue { return TwValue(std::pow(a.as_number(), b.as_number())); }},

        // ---- Comparison ---------------------------------------------------
        {"eq",  [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a == b); }},
        {"neq", [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a != b); }},
        {"lt",  [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a <  b); }},
        {"le",  [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a <= b); }},
        {"gt",  [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a >  b); }},
        {"ge",  [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a >= b); }},

        // ---- Boolean (math/and, math/or, math/not, math/xor) --------------
        {"and", [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a.as_bool() && b.as_bool()); }},
        {"or",  [](auto a, auto b, auto, auto) -> TwValue { return TwValue(a.as_bool() || b.as_bool()); }},
        {"not", [](auto a, auto, auto, auto)   -> TwValue {
            if (a.is_int()) return TwValue((int64_t)~(int32_t)a.as_int());  // bitwise NOT for int
            return TwValue(!a.as_bool()); }},
        {"xor", [](auto a, auto b, auto, auto) -> TwValue {
            if (a.is_int() && b.is_int()) return TwValue((int64_t)((int32_t)a.as_int() ^ (int32_t)b.as_int()));
            return TwValue(a.as_bool() != b.as_bool()); }},

        // ---- Special float checks -----------------------------------------
        {"isNaN", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::isnan(a.as_number())); }},
        {"isInf", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::isinf(a.as_number())); }},

        // ---- Trigonometry --------------------------------------------------
        {"sin",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::sin(a.as_number())); }},
        {"cos",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::cos(a.as_number())); }},
        {"tan",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::tan(a.as_number())); }},
        {"asin", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::asin(a.as_number())); }},
        {"acos", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::acos(a.as_number())); }},
        {"atan", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::atan(a.as_number())); }},
        {"atan2",[](auto a, auto b, auto, auto) -> TwValue { return TwValue(std::atan2(a.as_number(), b.as_number())); }},
        {"sinh", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::sinh(a.as_number())); }},
        {"cosh", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::cosh(a.as_number())); }},
        {"tanh", [](auto a, auto, auto, auto) -> TwValue { return TwValue(std::tanh(a.as_number())); }},
        {"asinh",[](auto a, auto, auto, auto) -> TwValue { return TwValue(std::asinh(a.as_number())); }},
        {"acosh",[](auto a, auto, auto, auto) -> TwValue { return TwValue(std::acosh(a.as_number())); }},
        {"atanh",[](auto a, auto, auto, auto) -> TwValue { return TwValue(std::atanh(a.as_number())); }},
        {"deg",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(a.as_number() * (180.0 / M_PI)); }},
        {"rad",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(a.as_number() * (M_PI / 180.0)); }},

        // ---- Constants ----------------------------------------------------
        {"E",   [](auto, auto, auto, auto) -> TwValue { return TwValue(M_E); }},
        {"Pi",  [](auto, auto, auto, auto) -> TwValue { return TwValue(M_PI); }},
        {"Tau", [](auto, auto, auto, auto) -> TwValue { return TwValue(2.0 * M_PI); }},
        {"Inf", [](auto, auto, auto, auto) -> TwValue { return TwValue(std::numeric_limits<double>::infinity()); }},
        {"NaN", [](auto, auto, auto, auto) -> TwValue { return TwValue(std::numeric_limits<double>::quiet_NaN()); }},

        // ---- Integer bitwise / shift --------------------------------------
        // math/asr — arithmetic shift right (sign-extending, 5-bit shift count)
        {"asr",    [](auto a, auto b, auto, auto) -> TwValue {
            return TwValue((int64_t)((int32_t)a.as_int() >> (b.as_int() & 31))); }},
        // math/lsl — logical shift left (truncated to 32 bits)
        {"lsl",    [](auto a, auto b, auto, auto) -> TwValue {
            return TwValue((int64_t)(int32_t)((uint32_t)a.as_int() << (b.as_int() & 31))); }},
        // math/clz — count leading zeros (32-bit; 0→32)
        {"clz",    [](auto a, auto, auto, auto) -> TwValue {
            uint32_t v = (uint32_t)a.as_int();
            return TwValue((int64_t)(v ? std::countl_zero(v) : 32)); }},
        // math/ctz — count trailing zeros (32-bit; 0→32)
        {"ctz",    [](auto a, auto, auto, auto) -> TwValue {
            uint32_t v = (uint32_t)a.as_int();
            return TwValue((int64_t)(v ? std::countr_zero(v) : 32)); }},
        // math/popcnt — count set bits (32-bit)
        {"popcnt", [](auto a, auto, auto, auto) -> TwValue {
            return TwValue((int64_t)std::popcount((uint32_t)a.as_int())); }},

        // ---- Type conversions (type/*) ------------------------------------
        {"boolToInt",   [](auto a, auto, auto, auto) -> TwValue { return TwValue((int64_t)(a.as_bool() ? 1 : 0)); }},
        {"boolToFloat", [](auto a, auto, auto, auto) -> TwValue { return TwValue(a.as_bool() ? 1.0 : 0.0); }},
        {"intToBool",   [](auto a, auto, auto, auto) -> TwValue { return TwValue(a.as_int() != 0); }},
        {"intToFloat",  [](auto a, auto, auto, auto) -> TwValue { return TwValue(a.as_number()); }},
        {"floatToInt",  [](auto a, auto, auto, auto) -> TwValue { return TwValue((int64_t)a.as_number()); }},
        {"floatToBool", [](auto a, auto, auto, auto) -> TwValue { return TwValue(a.as_number() != 0.0); }},

        // ---- Vector swizzle: combine / extract ----------------------------
        // math/combine2/3/4 — pack scalars into an array
        {"combine2", [](auto a, auto b, auto, auto) -> TwValue {
            return TwValue(TwValue::Array{a, b}); }},
        {"combine3", [](auto a, auto b, auto c, auto) -> TwValue {
            return TwValue(TwValue::Array{a, b, c}); }},
        {"combine4", [](auto a, auto b, auto c, auto d) -> TwValue {
            return TwValue(TwValue::Array{a, b, c, d}); }},
        // math/extract2/3/4 — index into an array (0-based)
        {"extract2", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            size_t i = (size_t)b.as_int();
            return i < a.as_array().size() ? a.as_array()[i] : TwValue{}; }},
        {"extract3", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            size_t i = (size_t)b.as_int();
            return i < a.as_array().size() ? a.as_array()[i] : TwValue{}; }},
        {"extract4", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            size_t i = (size_t)b.as_int();
            return i < a.as_array().size() ? a.as_array()[i] : TwValue{}; }},
        // math/extract2x2/3x3/4x4 — matrices are flat row-major arrays, so
        // this is index-for-index identical to extract2/3/4.
        {"extract2x2", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            size_t i = (size_t)b.as_int();
            return i < a.as_array().size() ? a.as_array()[i] : TwValue{}; }},
        {"extract3x3", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            size_t i = (size_t)b.as_int();
            return i < a.as_array().size() ? a.as_array()[i] : TwValue{}; }},
        {"extract4x4", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            size_t i = (size_t)b.as_int();
            return i < a.as_array().size() ? a.as_array()[i] : TwValue{}; }},
        // math/combine2x2 — pack 4 floats (row-major) into a 2x2 matrix.
        // Fits the existing 4-arg (a,b,c,d) convention, unlike combine3x3/4x4
        // which need more than 4 named inputs and are handled as structural
        // nodes in eval_node() instead.
        {"combine2x2", [](auto a, auto b, auto c, auto d) -> TwValue {
            return TwValue(TwValue::Array{a, b, c, d}); }},
        // math/rotate2D(a=float2, angle) — 2D rotation. See
        // lean/KHRTier1Witness.lean for the length-preservation witness this
        // mirrors.
        {"rotate2D", [](auto a, auto angle, auto, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 2) return TwValue{};
            double ax = a.as_array()[0].as_number(), ay = a.as_array()[1].as_number();
            double t = angle.as_number();
            double s = std::sin(t), cc = std::cos(t);
            return TwValue(TwValue::Array{
                TwValue(ax * cc - ay * s), TwValue(ax * s + ay * cc)}); }},

        // ---- Vector math --------------------------------------------------
        // math/length — Euclidean length (hypot-style: inf beats NaN)
        {"length", [](auto a, auto, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue(std::abs(a.as_number()));
            double sum = 0.0;
            for (const auto &comp : a.as_array()) sum += comp.as_number() * comp.as_number();
            return TwValue(std::sqrt(sum)); }},
        // math/dot — dot product
        {"dot", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array() || !b.is_array()) return TwValue(a.as_number() * b.as_number());
            double sum = 0.0;
            size_t n = std::min(a.as_array().size(), b.as_array().size());
            for (size_t i = 0; i < n; ++i) sum += a.as_array()[i].as_number() * b.as_array()[i].as_number();
            return TwValue(sum); }},
        // math/normalize — unit vector
        {"normalize", [](auto a, auto, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue(1.0);
            double len = 0.0;
            for (const auto &c : a.as_array()) len += c.as_number() * c.as_number();
            len = std::sqrt(len);
            if (len == 0.0 || std::isnan(len) || std::isinf(len)) {
                TwValue::Array zeros(a.as_array().size(), TwValue(0.0));
                return TwValue(std::move(zeros));
            }
            TwValue::Array out; out.reserve(a.as_array().size());
            for (const auto &c : a.as_array()) out.push_back(TwValue(c.as_number() / len));
            return TwValue(std::move(out)); }},
        // math/cross — cross product (float3 only)
        {"cross", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 3 ||
                !b.is_array() || b.as_array().size() < 3) return TwValue{};
            double ax = a.as_array()[0].as_number(), ay = a.as_array()[1].as_number(), az = a.as_array()[2].as_number();
            double bx = b.as_array()[0].as_number(), by = b.as_array()[1].as_number(), bz = b.as_array()[2].as_number();
            return TwValue(TwValue::Array{TwValue(ay*bz-az*by), TwValue(az*bx-ax*bz), TwValue(ax*by-ay*bx)}); }},

        // ---- Quaternion ---------------------------------------------------
        // math/quatMul — Hamilton product
        {"quatMul", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 4 ||
                !b.is_array() || b.as_array().size() < 4) return TwValue{};
            double ax=a.as_array()[0].as_number(), ay=a.as_array()[1].as_number(),
                   az=a.as_array()[2].as_number(), aw=a.as_array()[3].as_number();
            double bx=b.as_array()[0].as_number(), by=b.as_array()[1].as_number(),
                   bz=b.as_array()[2].as_number(), bw=b.as_array()[3].as_number();
            return TwValue(TwValue::Array{
                TwValue(aw*bx + ax*bw + ay*bz - az*by),
                TwValue(aw*by + ay*bw + az*bx - ax*bz),
                TwValue(aw*bz + az*bw + ax*by - ay*bx),
                TwValue(aw*bw - ax*bx - ay*by - az*bz)}); }},
        // math/quatConjugate — negate xyz, keep w
        {"quatConjugate", [](auto a, auto, auto, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 4) return TwValue{};
            return TwValue(TwValue::Array{
                TwValue(-a.as_array()[0].as_number()), TwValue(-a.as_array()[1].as_number()),
                TwValue(-a.as_array()[2].as_number()), a.as_array()[3]}); }},
        // math/quatAngleBetween — 2*acos(dot(a,b)) for unit quaternions
        {"quatAngleBetween", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 4 ||
                !b.is_array() || b.as_array().size() < 4) return TwValue{};
            double d = 0.0;
            for (size_t i = 0; i < 4; ++i) d += a.as_array()[i].as_number() * b.as_array()[i].as_number();
            return TwValue(2.0 * std::acos(std::clamp(d, -1.0, 1.0))); }},
        // math/quatFromAxisAngle — axis (float3) + angle (float) → float4
        {"quatFromAxisAngle", [](auto axis, auto angle, auto, auto) -> TwValue {
            if (!axis.is_array() || axis.as_array().size() < 3) return TwValue{};
            double s = std::sin(0.5 * angle.as_number()), w = std::cos(0.5 * angle.as_number());
            return TwValue(TwValue::Array{
                TwValue(axis.as_array()[0].as_number()*s), TwValue(axis.as_array()[1].as_number()*s),
                TwValue(axis.as_array()[2].as_number()*s), TwValue(w)}); }},
        // math/quatSlerp — slerp(a, b, c)
        {"quatSlerp", [](auto a, auto b, auto c, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 4 ||
                !b.is_array() || b.as_array().size() < 4) return TwValue{};
            double t = c.as_number();
            double ax=a.as_array()[0].as_number(), ay=a.as_array()[1].as_number(),
                   az=a.as_array()[2].as_number(), aw=a.as_array()[3].as_number();
            double bx=b.as_array()[0].as_number(), by=b.as_array()[1].as_number(),
                   bz=b.as_array()[2].as_number(), bw=b.as_array()[3].as_number();
            double d = ax*bx + ay*by + az*bz + aw*bw;
            if (d < 0.0) { d=-d; bx=-bx; by=-by; bz=-bz; bw=-bw; }
            double ka, kb;
            if (d > 0.9995) { ka=1.0-t; kb=t; }
            else { double om=std::acos(d), so=std::sin(om); ka=std::sin((1.0-t)*om)/so; kb=std::sin(t*om)/so; }
            return TwValue(TwValue::Array{TwValue(ax*ka+bx*kb), TwValue(ay*ka+by*kb),
                                          TwValue(az*ka+bz*kb), TwValue(aw*ka+bw*kb)}); }},

        // ---- Milestone 3: vector/matrix algebra ----------------------------
        // math/rotate3D(a=float3, rotation=float4) — assumes unit quaternion.
        // value = a + 2*(r x (r x a) + rw*(r x a))
        {"rotate3D", [](auto a, auto rot, auto, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 3 ||
                !rot.is_array() || rot.as_array().size() < 4) return TwValue{};
            double ax=a.as_array()[0].as_number(), ay=a.as_array()[1].as_number(), az=a.as_array()[2].as_number();
            double rx=rot.as_array()[0].as_number(), ry=rot.as_array()[1].as_number(),
                   rz=rot.as_array()[2].as_number(), rw=rot.as_array()[3].as_number();
            double tx = ry*az - rz*ay, ty = rz*ax - rx*az, tz = rx*ay - ry*ax;
            double ux = ry*tz - rz*ty, uy = rz*tx - rx*tz, uz = rx*ty - ry*tx;
            return TwValue(TwValue::Array{
                TwValue(ax + 2.0*(ux + rw*tx)),
                TwValue(ay + 2.0*(uy + rw*ty)),
                TwValue(az + 2.0*(uz + rw*tz))}); }},
        // math/transform(a=floatN, b=floatNxN) — row-major matrix * vector.
        {"transform", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array() || !b.is_array()) return TwValue{};
            const auto &v = a.as_array();
            auto m = mat_to_doubles(b);
            int n = mat_dim(m.size());
            if (n == 0 || (size_t)n != v.size()) return TwValue{};
            TwValue::Array out; out.reserve((size_t)n);
            for (int i = 0; i < n; ++i) {
                double sum = 0.0;
                for (int j = 0; j < n; ++j) sum += m[(size_t)i*n+j] * v[(size_t)j].as_number();
                out.push_back(TwValue(sum));
            }
            return TwValue(std::move(out)); }},
        // math/slerp(a=float2|float3, b=same, c=t) — vector slerp (distinct
        // from math/quatSlerp above). Falls back to plain lerp near-zero
        // length or near-parallel vectors, per spec §Vector Slerp.
        {"slerp", [](auto a, auto b, auto c, auto) -> TwValue {
            if (!a.is_array() || !b.is_array()) return TwValue{};
            const auto &av = a.as_array(); const auto &bv = b.as_array();
            size_t n = av.size();
            if (n != bv.size() || (n != 2 && n != 3)) return TwValue{};
            double t = c.as_number();
            constexpr double kEps = 1e-8;
            auto lerp_out = [&]() -> TwValue {
                TwValue::Array out; out.reserve(n);
                for (size_t i = 0; i < n; ++i)
                    out.push_back(TwValue((1.0 - t)*av[i].as_number() + t*bv[i].as_number()));
                return TwValue(std::move(out));
            };
            double la = 0.0, lb = 0.0;
            for (size_t i = 0; i < n; ++i) {
                la += av[i].as_number()*av[i].as_number();
                lb += bv[i].as_number()*bv[i].as_number();
            }
            la = std::sqrt(la); lb = std::sqrt(lb);
            if (la < kEps || lb < kEps) return lerp_out();
            std::vector<double> ah(n), bh(n);
            for (size_t i = 0; i < n; ++i) { ah[i] = av[i].as_number()/la; bh[i] = bv[i].as_number()/lb; }
            double d = 0.0; for (size_t i = 0; i < n; ++i) d += ah[i]*bh[i];
            d = std::clamp(d, -1.0, 1.0);
            double L = (1.0 - t)*la + t*lb;
            if (n == 2) {
                double theta = std::acos(d);
                if (ah[0]*bh[1] - ah[1]*bh[0] < 0.0) theta = -theta;
                double ang = t * theta;
                double s = std::sin(ang), cc = std::cos(ang);
                return TwValue(TwValue::Array{
                    TwValue((ah[0]*cc - ah[1]*s) * L),
                    TwValue((ah[0]*s + ah[1]*cc) * L)});
            }
            // n == 3
            if (d > 1.0 - kEps) return lerp_out();
            double rx, ry, rz;
            if (d < -1.0 + kEps) {
                // Any unit vector perpendicular to ah.
                if (std::abs(ah[0]) < 0.9) { rx = 1.0; ry = 0.0; rz = 0.0; }
                else { rx = 0.0; ry = 1.0; rz = 0.0; }
                double dp = rx*ah[0] + ry*ah[1] + rz*ah[2];
                rx -= dp*ah[0]; ry -= dp*ah[1]; rz -= dp*ah[2];
                double rl = std::sqrt(rx*rx+ry*ry+rz*rz);
                rx/=rl; ry/=rl; rz/=rl;
            } else {
                rx = ah[1]*bh[2]-ah[2]*bh[1]; ry = ah[2]*bh[0]-ah[0]*bh[2]; rz = ah[0]*bh[1]-ah[1]*bh[0];
                double rl = std::sqrt(rx*rx+ry*ry+rz*rz);
                rx/=rl; ry/=rl; rz/=rl;
            }
            double ang = t * std::acos(d);
            double qs = std::sin(0.5*ang), qw = std::cos(0.5*ang);
            double qx = rx*qs, qy = ry*qs, qz = rz*qs;
            // rotate3D(ah, Q)
            double tx = qy*ah[2] - qz*ah[1], ty = qz*ah[0] - qx*ah[2], tz = qx*ah[1] - qy*ah[0];
            double ux = qy*tz - qz*ty, uy = qz*tx - qx*tz, uz = qx*ty - qy*tx;
            double ox = ah[0] + 2.0*(ux + qw*tx);
            double oy = ah[1] + 2.0*(uy + qw*ty);
            double oz = ah[2] + 2.0*(uz + qw*tz);
            return TwValue(TwValue::Array{TwValue(ox*L), TwValue(oy*L), TwValue(oz*L)}); }},
        // math/transpose(a=floatNxN)
        {"transpose", [](auto a, auto, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            auto m = mat_to_doubles(a);
            int n = mat_dim(m.size());
            if (n == 0) return TwValue{};
            return mat_to_value(mat_transpose_v(m, n)); }},
        // math/determinant(a=floatNxN)
        {"determinant", [](auto a, auto, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            auto m = mat_to_doubles(a);
            int n = mat_dim(m.size());
            if (n == 0) return TwValue{};
            return TwValue(mat_det(m, n)); }},
        // math/inverse(a=floatNxN, b=output selector) — b=0 (default) ->
        // inverse matrix, b=1 -> isValid bool. Same b-as-selector convention
        // extract2/3/4 already use for their index argument.
        {"inverse", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array()) return TwValue{};
            auto m = mat_to_doubles(a);
            int n = mat_dim(m.size());
            if (n == 0) return TwValue{};
            auto [ok, inv] = mat_inverse_v(m, n);
            if (b.as_int() == 1) return TwValue(ok);
            return mat_to_value(inv); }},
        // math/matMul(a=floatNxN, b=floatNxN, same N)
        {"matMul", [](auto a, auto b, auto, auto) -> TwValue {
            if (!a.is_array() || !b.is_array()) return TwValue{};
            auto ma = mat_to_doubles(a), mb = mat_to_doubles(b);
            int n = mat_dim(ma.size());
            if (n == 0 || mb.size() != ma.size()) return TwValue{};
            return mat_to_value(mat_mul_v(ma, mb, n)); }},
        // math/matCompose(translation=float3 a, rotation=float4 b, scale=float3 c)
        // -> float4x4 (row-major). Assumes unit rotation quaternion.
        {"matCompose", [](auto a, auto b, auto c, auto) -> TwValue {
            if (!a.is_array() || a.as_array().size() < 3 ||
                !b.is_array() || b.as_array().size() < 4 ||
                !c.is_array() || c.as_array().size() < 3) return TwValue{};
            double tx=a.as_array()[0].as_number(), ty=a.as_array()[1].as_number(), tz=a.as_array()[2].as_number();
            double rx=b.as_array()[0].as_number(), ry=b.as_array()[1].as_number(),
                   rz=b.as_array()[2].as_number(), rw=b.as_array()[3].as_number();
            double sx=c.as_array()[0].as_number(), sy=c.as_array()[1].as_number(), sz=c.as_array()[2].as_number();
            return TwValue(TwValue::Array{
                TwValue(sx*(1-2*(ry*ry+rz*rz))), TwValue(sy*2*(rx*ry-rz*rw)),     TwValue(sz*2*(rx*rz+ry*rw)),     TwValue(tx),
                TwValue(sx*2*(rx*ry+rz*rw)),     TwValue(sy*(1-2*(rx*rx+rz*rz))), TwValue(sz*2*(ry*rz-rx*rw)),     TwValue(ty),
                TwValue(sx*2*(rx*rz-ry*rw)),     TwValue(sy*2*(ry*rz+rx*rw)),     TwValue(sz*(1-2*(rx*rx+ry*ry))), TwValue(tz),
                TwValue(0.0), TwValue(0.0), TwValue(0.0), TwValue(1.0)}); }},
    };
    return tbl;
}

inline TwValue eval_node(const TwValue::Dict &expr, const Params &params,
        const TwState &state, const TwValue::Dict &enums) {

    const std::string type = node_type(expr);
    if (type.empty()) return TwValue{};

    auto get = [&](const char *k) -> TwValue {
        auto it = expr.find(k);
        return it != expr.end() ? eval_expr(it->second, params, state, enums) : TwValue{};
    };

    // ---- Structural nodes (need expr dict access) -------------------------

    // pointer/get: {"type":"pointer/get","pointer":"/var/key"}
    if (type == "get") {
        auto pit = expr.find("pointer");
        if (pit == expr.end()) return TwValue{};
        auto [var, key] = parse_pointer(pit->second.as_string(), params);
        return var.empty() ? TwValue{} : state.get_nested(var, key);
    }

    // rebac/check: {"type":"rebac/check","rel":<string-or-relation-expr>,
    //               "subject":<eval-expr>,"object":<eval-expr>}
    // "rel" is not itself evaluated — it's a relation-expression tree (or a
    // bare capability-name string, sugar for {"type":"base","rel":<string>})
    // consumed directly by TwReBAC::check_expr against the domain's graph
    // (TwState::rebac_graph, populated once at load time — see
    // tw_loader.hpp's `load_domain`). "subject"/"object" are ordinary
    // sub-expressions (typically a "{param}" string, resolved the same way
    // any other action/method param reference is), so this composes with
    // whatever the rest of the eval language already supports rather than
    // inventing a second templating mechanism. The action-guard mechanism
    // this replaces (a bespoke TwActionFn-wrapping closure) is gone —
    // capabilities.actions requirements now compile into an ordinary
    // {"eval": {"type": "rebac/check", ...}} body step, ANDed with whatever
    // other guards/effects an action already has.
    if ((type == "check") && (expr.count("rel"))) {
        if (!state.rebac_graph) return TwValue(false);
        TwValue rel_expr = expr.at("rel");
        if (rel_expr.is_string()) {
            TwValue::Dict base_expr;
            base_expr["type"] = TwValue(std::string("base"));
            base_expr["rel"]  = rel_expr;
            rel_expr = TwValue(std::move(base_expr));
        }
        std::string subject = get("subject").as_string();
        std::string object  = get("object").as_string();
        return TwValue(TwReBAC::check_expr(*state.rebac_graph, subject, rel_expr,
                object, state.rebac_fuel));
    }

    // math/combine3x3 — pack 9 named floats (row-major) into a 3x3 matrix.
    // Structural: needs >4 named inputs, beyond the (a,b,c,d) table convention.
    if (type == "combine3x3") {
        static const char *keys[9] = {"a","b","c","d","e","f","g","h","i"};
        TwValue::Array out; out.reserve(9);
        for (auto *k : keys) out.push_back(get(k));
        return TwValue(std::move(out));
    }

    // math/combine4x4 — pack 16 named floats (row-major) into a 4x4 matrix.
    if (type == "combine4x4") {
        static const char *keys[16] = {"a","b","c","d","e","f","g","h",
                                        "i","j","k","l","m","n","o","p"};
        TwValue::Array out; out.reserve(16);
        for (auto *k : keys) out.push_back(get(k));
        return TwValue(std::move(out));
    }

    // math/quatFromAngles: {"type":"math/quatFromAngles","configuration":{"order":"yxz"},"x":..,"y":..,"z":..}
    // Structural: "order" is a configuration string, not a value socket.
    if (type == "quatFromAngles") {
        std::string order = "yxz";
        auto cfg_it = expr.find("configuration");
        if (cfg_it != expr.end() && cfg_it->second.is_dict()) {
            auto ord_it = cfg_it->second.as_dict().find("order");
            if (ord_it != cfg_it->second.as_dict().end() && ord_it->second.is_string()) {
                const std::string &o = ord_it->second.as_string();
                static const char *valid[6] = {"xyz","xzy","yxz","yzx","zxy","zyx"};
                for (auto *v : valid) if (o == v) { order = o; break; }
            }
        }
        double x = get("x").as_number(), y = get("y").as_number(), z = get("z").as_number();
        // Per-axis quaternions (half-angle), intrinsic Tait-Bryan composition:
        // apply axes left-to-right in `order` via Hamilton product q = q0 * q1 * q2.
        auto axis_quat = [](char axis, double a) -> TwValue::Array {
            double s = std::sin(0.5 * a), c = std::cos(0.5 * a);
            switch (axis) {
                case 'x': return {TwValue(s), TwValue(0.0), TwValue(0.0), TwValue(c)};
                case 'y': return {TwValue(0.0), TwValue(s), TwValue(0.0), TwValue(c)};
                default:  return {TwValue(0.0), TwValue(0.0), TwValue(s), TwValue(c)};
            }
        };
        auto qmul = [](const TwValue::Array &a, const TwValue::Array &b) -> TwValue::Array {
            double ax=a[0].as_number(), ay=a[1].as_number(), az=a[2].as_number(), aw=a[3].as_number();
            double bx=b[0].as_number(), by=b[1].as_number(), bz=b[2].as_number(), bw=b[3].as_number();
            return {TwValue(aw*bx + ax*bw + ay*bz - az*by),
                    TwValue(aw*by + ay*bw + az*bx - ax*bz),
                    TwValue(aw*bz + az*bw + ax*by - ay*bx),
                    TwValue(aw*bw - ax*bx - ay*by - az*bz)};
        };
        double angles[3] = {x, y, z};
        std::unordered_map<char, double> byAxis = {{'x', x}, {'y', y}, {'z', z}};
        TwValue::Array q0 = axis_quat(order[0], byAxis[order[0]]);
        TwValue::Array q1 = axis_quat(order[1], byAxis[order[1]]);
        TwValue::Array q2 = axis_quat(order[2], byAxis[order[2]]);
        (void)angles;
        return TwValue(qmul(qmul(q0, q1), q2));
    }

    // math/select: {"type":"math/select","condition":expr,"a":if_true,"b":if_false}
    if (type == "select") {
        auto cit = expr.find("condition");
        TwValue cond = cit != expr.end()
            ? eval_expr(cit->second, params, state, enums) : TwValue{};
        return cond.as_bool() ? get("a") : get("b");
    }

    // math/switch: {"type":"math/switch","selection":expr,"cases":[...],"default":expr,"<n>":expr}
    if (type == "switch") {
        auto sel_it = expr.find("selection");
        int64_t sel = sel_it != expr.end()
            ? eval_expr(sel_it->second, params, state, enums).as_int() : 0;
        std::string sel_str = std::to_string(sel);
        auto case_it = expr.find(sel_str);
        if (case_it != expr.end())
            return eval_expr(case_it->second, params, state, enums);
        auto def_it = expr.find("default");
        return def_it != expr.end()
            ? eval_expr(def_it->second, params, state, enums) : TwValue{};
    }

    // math/clamp: {"type":"math/clamp","a":val,"b":lo,"c":hi}
    if (type == "clamp") {
        TwValue a = get("a"), lo = get("b"), hi = get("c");
        if (a < lo) return lo; if (a > hi) return hi; return a;
    }

    // math/mix (lerp): {"type":"math/mix","a":x,"b":y,"c":t}  (spec uses a,b,c)
    if (type == "mix") {
        TwValue a = get("a"), b = get("b");
        auto c_it = expr.find("c");
        auto t_it = expr.find("t");  // legacy alias
        double t = c_it != expr.end() ? eval_expr(c_it->second, params, state, enums).as_number()
                 : t_it != expr.end() ? eval_expr(t_it->second, params, state, enums).as_number()
                 : 0.0;
        return TwValue((1.0 - t) * a.as_number() + t * b.as_number());
    }

    // math/random — seeded per-activation (simplification: use thread_local RNG)
    if (type == "random") {
        thread_local std::mt19937_64 rng(std::random_device{}());
        thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
        return TwValue(dist(rng));
    }

    // ---- Registry dispatch for pure (a,b,c,d) nodes ----------------------
    TwValue a = get("a"), b = get("b"), c = get("c"), d = get("d");
    const auto &tbl = kNodeTypes();
    auto it = tbl.find(type);
    if (it != tbl.end()) return it->second(a, b, c, d);
    return TwValue{};
}

inline TwValue eval_expr(const TwValue &expr, const Params &params,
        const TwState &state, const TwValue::Dict &enums) {
    if (expr.is_dict()) {
        const auto &d = expr.as_dict();
        if (d.count("type")) return eval_node(d, params, state, enums);
    }
    return resolve_param(expr, params);
}

// ---- Domain building helpers -----------------------------------------------

inline Params build_params(const TwValue::Array &names, const std::vector<TwValue> &args) {
    Params p;
    for (size_t i = 0; i < names.size() && i < args.size(); ++i)
        p[names[i].as_string()] = args[i];
    return p;
}

inline void run_binds(const TwValue::Array &binds, Params &params, const TwState &state) {
    for (auto &bind : binds) {
        if (!bind.is_dict()) continue;
        const auto &bd = bind.as_dict();
        auto name_it = bd.find("name");
        auto ptr_it  = bd.find("pointer");
        if (name_it == bd.end() || ptr_it == bd.end()) continue;
        auto [var, key] = parse_pointer(ptr_it->second.as_string(), params);
        if (!var.empty())
            params[name_it->second.as_string()] = state.get_nested(var, key);
    }
}

inline bool run_checks(const TwValue::Array &checks, const Params &params,
        const TwState &state, const TwValue::Dict &enums) {
    // Method-alternative and goal-method check clauses are glTF Interactivity
    // node evaluations. Each clause is `{"eval": <node>}`; a clause whose
    // result is anything other than boolean true fails the alternative.
    // Issue #50 phase 3 — legacy `{"pointer": "/x", "<op>": v}` shape removed.
    for (auto &step : checks) {
        if (!step.is_dict()) return false;
        const auto &cs = step.as_dict();
        auto eval_it = cs.find("eval");
        if (eval_it == cs.end()) return false;
        TwValue result = eval_expr(eval_it->second, params, state, enums);
        if (!result.is_bool() || !result.as_bool()) return false;
    }
    return true;
}

inline std::vector<TwTask> expand_subtasks(const TwValue::Array &defs, const Params &params) {
    std::vector<TwTask> tasks;
    for (auto &def : defs) {
        if (!def.is_array() || def.as_array().empty()) continue;
        const auto &arr = def.as_array();
        TwCall call;
        call.name = resolve_param(arr[0], params).as_string();
        for (size_t i = 1; i < arr.size(); ++i)
            call.args.push_back(resolve_param(arr[i], params));
        tasks.push_back(std::move(call));
    }
    return tasks;
}

// ---- Callable builders -----------------------------------------------------

inline TwActionFn build_action(const TwValue::Dict &def, const TwValue::Dict &enums) {
    TwValue::Array param_names, bind_defs, body;
    auto get_arr = [&](const char *k) -> TwValue::Array {
        auto it = def.find(k);
        return (it != def.end() && it->second.is_array()) ? it->second.as_array() : TwValue::Array{};
    };
    param_names = get_arr("params");
    bind_defs   = get_arr("bind");
    body        = get_arr("body");

    return [param_names, bind_defs, body, enums](
            std::shared_ptr<TwState> state, std::vector<TwValue> args)
            -> std::shared_ptr<TwState> {
        Params params = build_params(param_names, args);
        run_binds(bind_defs, params, *state);

        auto new_state = state->copy();

        for (auto &step : body) {
            if (!step.is_dict()) return nullptr;
            const auto &s = step.as_dict();

            // RECTGTN exposes exactly two action body primitives:
            //   - eval:        evaluate a glTF Interactivity node; action
            //                  fails if the result is false
            //   - pointer/set: write state at a JSON-pointer path
            // Anything else is a malformed step and aborts the action.
            auto eval_it = s.find("eval");
            auto set_it  = s.find("pointer/set");

            if (eval_it != s.end()) {
                TwValue result = eval_expr(eval_it->second, params, *new_state, enums);
                if (!result.is_bool() || !result.as_bool()) return nullptr;
            } else if (set_it != s.end()) {
                auto [var, key] = parse_pointer(set_it->second.as_string(), params);
                if (var.empty()) return nullptr;
                auto val_it = s.find("value");
                if (val_it == s.end()) return nullptr;
                TwValue value = eval_expr(val_it->second, params, *new_state, enums);
                new_state->set_nested(var, key, std::move(value));
            } else {
                return nullptr;
            }
        }
        return new_state;
    };
}

// Also used to build goal-method alternatives: TwGoalMethodFn IS TwMethodFn
// (tw_domain.hpp) — a goal method invoked as (state, [key, desired]) is the
// same calling convention as an ordinary method whose params are
// [key_param, desired_param]. There's no distinct wire shape for a goal
// method either: it's an ordinary "methods" entry named after the state
// var it targets.
inline TwMethodFn build_method_alt(const TwValue::Array &param_names,
        const TwValue::Dict &alt, const TwValue::Dict &enums) {
    auto get_arr = [&](const char *k) -> TwValue::Array {
        auto it = alt.find(k);
        return (it != alt.end() && it->second.is_array()) ? it->second.as_array() : TwValue::Array{};
    };
    TwValue::Array bind_defs   = get_arr("bind");
    TwValue::Array check_defs  = get_arr("check");
    TwValue::Array subtask_defs = get_arr("subtasks");

    return [param_names, bind_defs, check_defs, subtask_defs, enums](
            std::shared_ptr<TwState> state, std::vector<TwValue> args)
            -> std::optional<std::vector<TwTask>> {
        Params params = build_params(param_names, args);
        run_binds(bind_defs, params, *state);
        if (!run_checks(check_defs, params, *state, enums)) return std::nullopt;
        return expand_subtasks(subtask_defs, params);
    };
}

// Scan method: iterate over all keys of state[over], try each branch with
// {_key} bound to that key; on first match return branch subtasks + recurse
// call; if no key matches any branch return done_subtasks.
inline TwMethodFn build_scan_method(const TwValue::Dict &scan_def,
        const TwValue::Dict &enums) {
    // "over" — state variable name to iterate
    std::string over_var;
    {
        TwValue::Dict::const_iterator it = scan_def.find("over");
        if (it != scan_def.end()) over_var = it->second.as_string();
    }

    // "recurse" — task name to append when a branch matches
    std::string recurse_name;
    {
        TwValue::Dict::const_iterator it = scan_def.find("recurse");
        if (it != scan_def.end()) recurse_name = it->second.as_string();
    }

    // "branches" — array of alt-style dicts each with bind/check/subtasks
    struct Branch {
        TwValue::Array bind_defs;
        TwValue::Array check_defs;
        TwValue::Array subtask_defs;
    };
    std::vector<Branch> branches;
    {
        TwValue::Dict::const_iterator it = scan_def.find("branches");
        if (it != scan_def.end() && it->second.is_array()) {
            for (const TwValue &br : it->second.as_array()) {
                if (!br.is_dict()) continue;
                auto get = [&](const char *k) -> TwValue::Array {
                    TwValue::Dict::const_iterator jt = br.as_dict().find(k);
                    return (jt != br.as_dict().end() && jt->second.is_array())
                        ? jt->second.as_array() : TwValue::Array{};
                };
                branches.push_back({get("bind"), get("check"), get("subtasks")});
            }
        }
    }

    // "done" — optional check run when all branches × keys exhausted; fail if not met
    TwValue::Array done_check;
    {
        TwValue::Dict::const_iterator it = scan_def.find("done");
        if (it != scan_def.end() && it->second.is_array())
            done_check = it->second.as_array();
    }

    // "done_subtasks" — returned when no key matches any branch (and done check passes)
    TwValue::Array done_subtasks;
    {
        TwValue::Dict::const_iterator it = scan_def.find("done_subtasks");
        if (it != scan_def.end() && it->second.is_array())
            done_subtasks = it->second.as_array();
    }

    return [over_var, recurse_name, branches, done_check, done_subtasks, enums](
            std::shared_ptr<TwState> state, std::vector<TwValue> /*args*/)
            -> std::optional<std::vector<TwTask>> {
        // Collect current keys of the scanned variable.
        std::vector<std::string> keys;
        {
            tsl::ordered_map<std::string, TwValue>::const_iterator it = state->vars.find(over_var);
            if (it != state->vars.end() && it->second.is_dict())
                for (const std::pair<const std::string, TwValue> &kv : it->second.as_dict())
                    keys.push_back(kv.first);
        }

        // Branch-priority ordering: for each branch, scan ALL keys before
        // trying the next branch. Matches Python gltf_domain_interpreter.py.
        for (const Branch &br : branches) {
            for (const std::string &key : keys) {
                Params params;
                params["_key"] = TwValue(key);
                run_binds(br.bind_defs, params, *state);
                if (!run_checks(br.check_defs, params, *state, enums)) continue;
                std::vector<TwTask> subtasks = expand_subtasks(br.subtask_defs, params);
                if (!recurse_name.empty())
                    subtasks.push_back(TwCall{recurse_name, {}});
                return subtasks;
            }
        }
        // All branches x keys exhausted — run optional done check.
        Params empty;
        if (!done_check.empty() && !run_checks(done_check, empty, *state, enums))
            return std::nullopt;
        return expand_subtasks(done_subtasks, empty);
    };
}

// ---- Main loader -----------------------------------------------------------

struct TwLoaded {
    TwDomain                 domain;
    std::shared_ptr<TwState> state;
    std::vector<TwTask>      tasks;
    TwValue::Dict            enums;
};

inline TwLoaded load_domain(const TwValue &data) {
    TwLoaded result;
    // A non-object top-level document (array, string, number, null) used to
    // fall through with `state` already allocated, so every downstream
    // caller's `if (!loaded.state) throw "failed_to_load_domain"` guard
    // never fired — a malformed document silently planned as an empty
    // domain (trivial "ok" success) instead of failing to load. `state`
    // stays null here specifically so that guard catches this case too.
    if (!data.is_dict()) {
        return result;
    }
    const auto &d = data.as_dict();

    // Whitelist: any key outside this set fails the load. A stale/misspelled
    // key (e.g. the old "tasks" before the todo_list rename) used to be
    // silently ignored — no error, no effect, just quietly dropped data —
    // which is worse than a strict failure for a caller that mistyped or is
    // still on an old shape. No free-form "notes"/"metadata" catch-all key
    // is whitelisted, deliberately: JSON itself dropped comment syntax for
    // exactly this reason (Crockford: comments got used to hold parsing
    // directives, which destroys interoperability) — a permitted-but-ignored
    // bucket inside the parsed document is the same hazard in a different
    // shape.
    static const std::unordered_set<std::string> kKnownKeys = {
        "@context", "@type", "name", "description", "version", "source",
        "enums", "variables", "actions", "methods", "todo_list",
        "capabilities",
    };
    for (const std::pair<const std::string, TwValue> &kv : d) {
        if (!kKnownKeys.count(kv.first)) {
            return result;
        }
    }

    result.state = std::make_shared<TwState>();

    // Enums
    if (auto it = d.find("enums"); it != d.end() && it->second.is_dict())
        result.enums = it->second.as_dict();
    const auto &enums = result.enums;

    // Variables
    if (auto it = d.find("variables"); it != d.end() && it->second.is_array()) {
        for (auto &var_def : it->second.as_array()) {
            if (!var_def.is_dict()) continue;
            const auto &vd = var_def.as_dict();
            auto name_it = vd.find("name");
            auto init_it = vd.find("init");
            if (name_it == vd.end() || init_it == vd.end()) continue;
            const std::string &var_name = name_it->second.as_string();
            const auto &init = init_it->second;
            if (init.is_dict()) {
                for (auto &[key, val] : init.as_dict())
                    result.state->set_nested(var_name, TwValue(key), val);
            } else {
                result.state->set_var(var_name, init);
            }
        }
    }

    // Relationships (ADR 0004): a top-level "capabilities" object — a
    // dedicated key, not a variable, matching glTF Interactivity's own
    // convention that structured/relational extension data (e.g.
    // KHR_lights_punctual's "/extensions/KHR_lights_punctual/lights") gets
    // its own namespaced slot rather than sharing the generic scalar/vector
    // "variables" array. Its "entities" list compiles into HAS_CAPABILITY
    // edges on a TwReBAC::TwReBACGraph (the same graph engine Taskweft.ReBAC
    // uses standalone), plus an optional explicit "graph" key (identical
    // wire format to Taskweft.ReBAC's graph_json: {"edges":[...],
    // "definitions":{}}) for richer relationships (team membership,
    // delegation, ...). _cap_<cap> state vars are still populated for
    // introspection/backward compatibility, but action guards are evaluated
    // against the graph via TwReBAC::check_expr, not read back from those
    // vars — so a domain can express a capability requirement as an
    // arbitrary relation expression, not just a direct HAS_CAPABILITY edge.
    //
    // There is no compiled sugar for action requirements: a domain author
    // writes the {"eval": {"type": "rebac/check", ...}} guard step directly
    // into the action's own body, the same mechanism every other action
    // precondition already uses (build_action's "eval" step handling,
    // below — "action fails if the result is false").
    auto rebac_graph = std::make_shared<TwReBAC::TwReBACGraph>();
    if (auto cap_it = d.find("capabilities"); cap_it != d.end() && cap_it->second.is_dict()) {
        const TwValue::Dict &caps = cap_it->second.as_dict();

        // entities: {entity: [cap, ...]} → HAS_CAPABILITY edge + _cap_<cap>[entity] (compat)
        TwValue::Dict::const_iterator ent_it = caps.find("entities");
        if (ent_it != caps.end() && ent_it->second.is_dict()) {
            for (const std::pair<const std::string, TwValue> &ep : ent_it->second.as_dict()) {
                if (!ep.second.is_array()) continue;
                for (const TwValue &cv : ep.second.as_array()) {
                    const std::string &cap = cv.as_string();
                    rebac_graph->add_edge(ep.first, cap, "HAS_CAPABILITY");
                    std::string cap_var = "_cap_" + cap;
                    result.state->set_nested(cap_var, TwValue(ep.first), TwValue(true));
                }
            }
        }

        // Optional explicit relationship graph, merged into the same graph
        // the "entities" capability edges above already populated.
        TwValue::Dict::const_iterator graph_it = caps.find("graph");
        if (graph_it != caps.end() && graph_it->second.is_dict()) {
            const TwValue::Dict &gm = graph_it->second.as_dict();
            TwValue::Dict::const_iterator ge_it = gm.find("edges");
            if (ge_it != gm.end() && ge_it->second.is_array()) {
                for (const TwValue &ev : ge_it->second.as_array()) {
                    if (!ev.is_dict()) continue;
                    const TwValue::Dict &em = ev.as_dict();
                    TwValue::Dict::const_iterator s_it = em.find("subject");
                    TwValue::Dict::const_iterator o_it = em.find("object");
                    TwValue::Dict::const_iterator r_it = em.find("rel");
                    if (s_it == em.end() || o_it == em.end() || r_it == em.end()) continue;
                    rebac_graph->add_edge(s_it->second.as_string(), o_it->second.as_string(),
                            r_it->second.as_string());
                }
            }
            TwValue::Dict::const_iterator gd_it = gm.find("definitions");
            if (gd_it != gm.end() && gd_it->second.is_dict()) {
                for (const std::pair<const std::string, TwValue> &dp : gd_it->second.as_dict())
                    rebac_graph->define(dp.first, dp.second);
            }
        }
    }

    // Assign the domain's ReBAC graph onto the initial state once — the
    // single source of truth both goal-binding satisfaction
    // (TwGoalBinding::satisfied, tw_domain.hpp) and the rebac/check eval
    // node below read from. TwState::copy() propagates it via ordinary
    // default member-wise copy (cheap: it's a shared_ptr), so it stays
    // available at every state reached during planning, not just the
    // initial one.
    result.state->rebac_graph = rebac_graph;

    // Actions
    if (auto it = d.find("actions"); it != d.end() && it->second.is_dict()) {
        for (const std::pair<const std::string, TwValue> &np : it->second.as_dict()) {
            if (!np.second.is_dict()) continue;
            // RECTGTN 'T': store ISO 8601 duration metadata for temporal analysis.
            TwValue::Dict adef = np.second.as_dict();
            TwValue::Dict::const_iterator dur_it = adef.find("duration");
            if (dur_it != adef.end() && dur_it->second.is_string())
                result.domain.action_durations[np.first] = dur_it->second.as_string();

            result.domain.actions[np.first] = build_action(adef, enums);
        }
    }

    // Task methods
    if (auto it = d.find("methods"); it != d.end() && it->second.is_dict()) {
        for (auto &[task_name, group] : it->second.as_dict()) {
            if (!group.is_dict()) continue;
            const auto &gd = group.as_dict();

            // Scan method: single fn that iterates over a state-variable's keys.
            if (auto sit = gd.find("scan"); sit != gd.end() && sit->second.is_dict()) {
                result.domain.task_methods[task_name] = {
                    build_scan_method(sit->second.as_dict(), enums)
                };
                continue;
            }

            TwValue::Array param_names;
            if (auto pit = gd.find("params"); pit != gd.end() && pit->second.is_array())
                param_names = pit->second.as_array();

            auto alts_it = gd.find("alternatives");
            if (alts_it == gd.end() || !alts_it->second.is_array()) continue;

            std::vector<TwMethodFn> fns;
            for (auto &alt : alts_it->second.as_array()) {
                if (!alt.is_dict()) continue;
                fns.push_back(build_method_alt(param_names, alt.as_dict(), enums));
            }
            result.domain.task_methods[task_name] = std::move(fns);
        }
    }

    // There is no separate "goals" key: a goal method IS an ordinary method
    // (TwGoalMethodFn is TwMethodFn) — TwGoal/TwMultiGoal binding resolution
    // looks up task_methods by the target state var's name directly (see
    // tw_planner.hpp), so a domain author defines a goal-satisfying method
    // under "methods" like any other, named after the variable it targets.
    // A separate top-level key here would just be a second, redundant way
    // to write the exact same {params, alternatives} shape into the exact
    // same map.

    // Initial todo list (GTPyHOP's term for this exact heterogeneous list —
    // find_plan(state, todo_list)). Array items are one of three shapes:
    //   [name, args...]                        — TwCall
    //   {"multigoal": {var: {key: desired}}}    — TwMultiGoal
    //   {"goal": [{"pointer", "eq"}, ...]}      — TwGoal (conjunctive bindings)
    // Any other item shape (an empty call array, an object matching none of
    // the three, a bare scalar) fails the whole load instead of silently
    // vanishing from the plan — a malformed item is caller error, not an
    // empty-but-valid todo list entry.
    if (auto it = d.find("todo_list"); it != d.end() && it->second.is_array()) {
        for (const TwValue &task_def : it->second.as_array()) {
            if (task_def.is_dict()) {
                const TwValue::Dict &td = task_def.as_dict();

                if (auto mg_it = td.find("multigoal"); mg_it != td.end() && mg_it->second.is_dict()) {
                    // {"multigoal": {var: {key: desired, ...}, ...}}
                    TwMultiGoal mg;
                    for (const std::pair<const std::string, TwValue> &vp : mg_it->second.as_dict()) {
                        if (!vp.second.is_dict()) {
                            continue;
                        }
                        for (const std::pair<const std::string, TwValue> &kp : vp.second.as_dict()) {
                            TwGoalBinding b;
                            b.var     = vp.first;
                            b.key     = kp.first;
                            b.desired = kp.second;
                            mg.bindings.push_back(std::move(b));
                        }
                    }
                    if (mg.bindings.empty()) {
                        result.state = nullptr;
                        return result;
                    }
                    result.tasks.push_back(std::move(mg));
                } else if (auto g_it = td.find("goal"); g_it != td.end() && g_it->second.is_array()) {
                    // {"goal": [{"pointer": "/var/key", "eq": desired}, ...]}
                    TwGoal goal;
                    for (auto &entry : g_it->second.as_array()) {
                        if (!entry.is_dict()) {
                            continue;
                        }
                        const auto &ed = entry.as_dict();
                        auto ptr_it = ed.find("pointer");
                        auto eq_it  = ed.find("eq");
                        if (ptr_it == ed.end() || eq_it == ed.end()) {
                            continue;
                        }
                        Params empty_params;
                        auto [var, key] = parse_pointer(ptr_it->second.as_string(), empty_params);
                        if (var.empty()) {
                            continue;
                        }
                        TwGoalBinding b;
                        b.var     = var;
                        b.key     = key.to_string();
                        b.desired = eq_it->second;
                        goal.bindings.push_back(std::move(b));
                    }
                    if (goal.bindings.empty()) {
                        result.state = nullptr;
                        return result;
                    }
                    result.tasks.push_back(std::move(goal));
                } else {
                    // Neither "multigoal" nor "goal" — an unrecognized object shape.
                    result.state = nullptr;
                    return result;
                }
            } else if (task_def.is_array() && !task_def.as_array().empty()) {
                const TwValue::Array &arr = task_def.as_array();
                TwCall call;
                call.name = arr[0].as_string();
                for (size_t i = 1; i < arr.size(); ++i) {
                    call.args.push_back(arr[i]);
                }
                result.tasks.push_back(std::move(call));
            } else {
                // An empty call array or a bare scalar — not any of the three
                // task kinds.
                result.state = nullptr;
                return result;
            }
        }
    }

    return result;
}

inline TwLoaded load_json(const std::string &json_str) {
    return load_domain(parse_json_str(json_str));
}

inline TwLoaded load_file(const std::string &path) {
    std::ifstream f(path);
    if (!f) return TwLoaded{};
    std::ostringstream oss;
    oss << f.rdbuf();
    return load_json(oss.str());
}

// Load domain and problem from separate files.
// The domain supplies actions/methods/goals; the problem supplies variables/tasks.
// State variables from the problem override those from the domain.
inline TwLoaded load_file_pair(const std::string &domain_path, const std::string &problem_path) {
    TwLoaded dom = load_file(domain_path);
    if (!dom.state) return TwLoaded{};
    TwLoaded prob = load_file(problem_path);
    if (!prob.state) return TwLoaded{};

    // Merge state: problem values override domain defaults.
    for (auto &[k, v] : prob.state->vars)
        dom.state->vars[k] = v;
    // Merge methods (goal methods included — no separate map): problem may
    // define extra or override domain methods.
    for (auto &[k, v] : prob.domain.task_methods)
        dom.domain.task_methods[k] = v;
    for (auto &[k, v] : prob.domain.actions)
        dom.domain.actions[k] = v;
    if (!prob.tasks.empty())
        dom.tasks = prob.tasks;

    return dom;
}

// Serialise a plan as a JSON array string.
inline std::string plan_to_json(const std::vector<TwCall> &plan) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < plan.size(); ++i) {
        if (i) oss << ", ";
        oss << "[\"" << plan[i].name << "\"";
        for (auto &arg : plan[i].args) oss << ", \"" << arg.to_string() << "\"";
        oss << "]";
    }
    oss << "]";
    return oss.str();
}

} // namespace TwLoader
