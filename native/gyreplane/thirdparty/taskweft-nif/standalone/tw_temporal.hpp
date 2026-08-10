// RECTGTN 'T' — Temporal: ISO 8601 duration parsing, STN consistency, plan timing.
// Mirrors Python ipyhop/temporal/stn.py and temporal_metadata.py.
//
// tw_parse_duration_ms: ISO 8601-1:2019 §5.5.2.4 compliant; mirrors
// lean/Planner/Iso8601Duration.lean.  Canonical order Y→Mo→D→H→Mi→S,
// fractions on lowest-order unit only, integer millisecond arithmetic.
#pragma once
#include "tw_domain.hpp"
#include "thirdparty/date/date.h"
#include <cstdint>
#include <optional>
#include <sstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// Unit milliseconds (Timex conventions: 1Y=365d, 1Mo=30d, 1W=7d).
namespace tw_duration_detail {
static constexpr int64_t MS_Y  = 365LL * 86400 * 1000;
static constexpr int64_t MS_MO =  30LL * 86400 * 1000;
static constexpr int64_t MS_W  =   7LL * 86400 * 1000;
static constexpr int64_t MS_D  =        86400LL * 1000;
static constexpr int64_t MS_H  =         3600LL * 1000;
static constexpr int64_t MS_MI =           60LL * 1000;
static constexpr int64_t MS_S  =                  1000;
} // namespace tw_duration_detail

// Parse ISO 8601 duration → total milliseconds.
// Handles P[nY][nM][nD][T[nH][nM][nS]] and PnW.
// "P" alone returns 0.  Returns -1 on any parse error.
inline int64_t tw_parse_duration_ms(const std::string &dur) {
    using namespace tw_duration_detail;
    if (dur.empty() || dur[0] != 'P') return -1;
    if (dur.size() == 1) return 0; // "P" = zero duration

    size_t  i          = 1;
    int64_t total_ms   = 0;
    bool    in_time    = false;
    bool    saw_t      = false;
    bool    frac_seen  = false;
    bool    saw_w      = false;
    bool    saw_any    = false;
    int     last_rank  = -1;

    while (i < dur.size()) {
        if (frac_seen) return -1; // fractionNotOnLast

        if (dur[i] == 'T') {
            if (saw_t)         return -1; // duplicateT
            if (i + 1 >= dur.size()) return -1; // unexpectedEnd
            saw_t   = true;
            in_time = true;
            // lastRank carried through (H rank=3 > D rank=2 naturally)
            ++i;
            continue;
        }

        if (!std::isdigit((unsigned char)dur[i])) return -1; // unexpectedToken

        // Integer part
        int64_t whole = 0;
        while (i < dur.size() && std::isdigit((unsigned char)dur[i]))
            whole = whole * 10 + (dur[i++] - '0');

        // Optional fraction (max 3 digits; remainder truncated)
        int64_t frac_milli = 0;
        bool    has_frac   = false;
        if (i < dur.size() && dur[i] == '.') {
            ++i;
            if (i >= dur.size() || !std::isdigit((unsigned char)dur[i]))
                return -1; // invalidNumber: trailing dot
            has_frac = true;
            int64_t acc = 0, n = 0;
            while (i < dur.size() && std::isdigit((unsigned char)dur[i]) && n < 3) {
                acc = acc * 10 + (dur[i++] - '0');
                ++n;
            }
            while (i < dur.size() && std::isdigit((unsigned char)dur[i])) ++i; // excess
            for (int64_t k = n; k < 3; ++k) acc *= 10;
            frac_milli = acc;
        }

        if (i >= dur.size()) return -1; // number with no unit
        char unit_c = dur[i++];

        // Classify unit → (rank, unit_ms)
        int     rank;
        int64_t unit_ms;
        if (!in_time) {
            if      (unit_c == 'Y') { rank = 0;  unit_ms = MS_Y;  }
            else if (unit_c == 'M') { rank = 1;  unit_ms = MS_MO; }
            else if (unit_c == 'W') { rank = 99; unit_ms = MS_W;  }
            else if (unit_c == 'D') { rank = 2;  unit_ms = MS_D;  }
            else return -1; // wrong-side or unknown unit
        } else {
            if      (unit_c == 'H') { rank = 3; unit_ms = MS_H;  }
            else if (unit_c == 'M') { rank = 4; unit_ms = MS_MI; }
            else if (unit_c == 'S') { rank = 5; unit_ms = MS_S;  }
            else return -1;
        }

        // W must stand alone (mixedBasicExtended)
        if (unit_c == 'W' && (saw_any || in_time)) return -1;
        if (saw_w) return -1;
        if (unit_c == 'W') saw_w = true;

        // Canonical order (nonCanonicalOrder)
        if (rank != 99 && rank <= last_rank) return -1;
        last_rank = (rank == 99) ? last_rank : rank;

        total_ms += whole * unit_ms;
        if (has_frac) {
            total_ms += frac_milli * (unit_ms / 1000);
            frac_seen = true;
        }
        saw_any = true;
    }

    return total_ms;
}

// Legacy shim: parse ISO 8601 duration → seconds (double).
// Kept for callers that still use the old signature.  New code should
// prefer tw_parse_duration_ms and work in milliseconds.
inline double tw_parse_duration(const std::string &dur) {
    int64_t ms = tw_parse_duration_ms(dur);
    return ms < 0 ? -1.0 : static_cast<double>(ms) / 1000.0;
}

// Format milliseconds → ISO 8601 duration string (integer arithmetic only).
inline std::string tw_format_duration_ms(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t total_s = ms / 1000;
    int64_t milli   = ms % 1000;
    int64_t h = total_s / 3600; total_s -= h * 3600;
    int64_t m = total_s / 60;
    int64_t s = total_s % 60;

    std::string out = "PT";
    if (h > 0) out += std::to_string(h) + "H";
    if (m > 0) out += std::to_string(m) + "M";
    if (s > 0 || milli > 0 || (h == 0 && m == 0)) {
        out += std::to_string(s);
        if (milli > 0) {
            char buf[8];
            int  len = std::snprintf(buf, sizeof(buf), ".%03lld", (long long)milli);
            while (len > 1 && buf[len - 1] == '0') --len;
            buf[len] = '\0';
            out += buf;
        }
        out += "S";
    }
    return out;
}

// Legacy shim: seconds → ISO 8601 string.
inline std::string tw_format_duration(double seconds) {
    return tw_format_duration_ms(static_cast<int64_t>(seconds * 1000.0 + 0.5));
}

// STN is an implementation detail — hidden from callers.
// Public interface: TwTemporalStep, TwTemporalResult, tw_check_temporal().
namespace tw_detail {
struct STN {
    static constexpr double INF = std::numeric_limits<double>::infinity();

    std::vector<std::string>                points;
    std::unordered_map<std::string, size_t> idx;
    std::vector<std::vector<double>>        dist;

    void add_point(const std::string &p) {
        if (idx.count(p)) return;
        size_t n = points.size();
        idx[p] = n;
        points.push_back(p);
        for (auto &row : dist) row.push_back(INF);
        dist.push_back(std::vector<double>(n + 1, INF));
        dist[n][n] = 0.0;
    }

    void add_constraint(const std::string &from, const std::string &to,
                        double lo, double hi) {
        add_point(from); add_point(to);
        size_t fi = idx.at(from), ti = idx.at(to);
        if (hi  < dist[fi][ti]) dist[fi][ti] = hi;
        if (-lo < dist[ti][fi]) dist[ti][fi] = -lo;
    }

    bool consistent() const {
        size_t n = points.size();
        if (n == 0) return true;
        std::vector<std::vector<double>> d = dist;
        for (size_t k = 0; k < n; ++k)
            for (size_t i = 0; i < n; ++i) {
                if (d[i][k] == INF) continue;
                for (size_t j = 0; j < n; ++j) {
                    if (d[k][j] == INF) continue;
                    double via = d[i][k] + d[k][j];
                    if (via < d[i][j]) d[i][j] = via;
                }
            }
        for (size_t i = 0; i < n; ++i)
            if (d[i][i] < 0.0) return false;
        return true;
    }
};
} // namespace tw_detail

// Per-step temporal annotation — all time values are ISO 8601 strings.
struct TwTemporalStep {
    std::string action_name;
    std::string duration_iso;   // ISO 8601 duration (e.g. "PT10M"), empty if none
    std::string start_iso;      // ISO 8601 duration from origin (e.g. "PT0S")
    std::string end_iso;        // ISO 8601 duration from origin (e.g. "PT10M")
};

// Result of temporal analysis on a plan — all time in ISO 8601.
struct TwTemporalResult {
    bool        consistent;    // STN consistency check passed
    std::string total_iso;     // total plan duration as ISO 8601 (e.g. "PT20M")
    std::string origin_iso;    // origin offset supplied by caller (default "PT0S")
    std::vector<TwTemporalStep> steps;
};

// Build a sequential STN from a plan and return temporal metadata.
// Sequential assumption: each action starts exactly when the previous ends.
// Actions with no duration entry are treated as PT0S.
// origin_iso: ISO 8601 duration string for the plan start offset (default "PT0S").
inline TwTemporalResult tw_check_temporal(
        const std::vector<TwCall> &plan,
        const TwDomain            &domain,
        const std::string         &origin_iso = "PT0S") {
    TwTemporalResult r;
    r.consistent = true;
    r.origin_iso = origin_iso;

    int64_t origin_ms = tw_parse_duration_ms(origin_iso);
    if (origin_ms < 0) origin_ms = 0;

    if (plan.empty()) {
        r.total_iso = "PT0S";
        return r;
    }

    tw_detail::STN stn;
    stn.add_point("t0");
    std::string prev_end  = "t0";
    int64_t     current   = origin_ms;
    int64_t     total_ms  = 0;

    for (size_t i = 0; i < plan.size(); ++i) {
        const std::string &name = plan[i].name;

        int64_t     dur_ms  = 0;
        std::string dur_iso;
        auto dit = domain.action_durations.find(name);
        if (dit != domain.action_durations.end()) {
            dur_iso       = dit->second;
            int64_t parsed = tw_parse_duration_ms(dur_iso);
            if (parsed >= 0) dur_ms = parsed;
        }

        TwTemporalStep step;
        step.action_name  = name;
        step.duration_iso = dur_iso.empty() ? "PT0S" : dur_iso;
        step.start_iso    = tw_format_duration_ms(current);
        step.end_iso      = tw_format_duration_ms(current + dur_ms);
        r.steps.push_back(std::move(step));

        current  += dur_ms;
        total_ms += dur_ms;

        double dur_s    = static_cast<double>(dur_ms) / 1000.0;
        std::string a_s = "a" + std::to_string(i) + "_start";
        std::string a_e = "a" + std::to_string(i) + "_end";
        stn.add_constraint(prev_end, a_s, 0.0, 0.0);
        stn.add_constraint(a_s,      a_e, dur_s, dur_s);
        prev_end = a_e;
    }

    r.consistent = stn.consistent();
    r.total_iso  = tw_format_duration_ms(total_ms);
    return r;
}

// ── Civil-time layer (uses Hinnant date.h) ────────────────────────────────────
//
// tw_parse_duration_ms uses Timex fixed-day conventions (1Y=365d, 1Mo=30d).
// The civil-time layer replaces Y and Mo with actual calendar arithmetic so
// P1Y from 2024-01-01 = 366 d (leap year) and P1M from 2024-01-31 = 29 d
// (February 2024).  W, D, H, Mi, S retain their fixed values.

// Parse "YYYY-MM-DD" (or "YYYY-MM-DDT..." — time part ignored).
// Returns nullopt on any format or range error.
inline std::optional<date::year_month_day> tw_parse_date(const std::string &s) {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    int y = 0, m = 0, d = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::isdigit((unsigned char)s[i])) return std::nullopt;
        y = y * 10 + (s[i] - '0');
    }
    for (int i = 5; i < 7; ++i) {
        if (!std::isdigit((unsigned char)s[i])) return std::nullopt;
        m = m * 10 + (s[i] - '0');
    }
    for (int i = 8; i < 10; ++i) {
        if (!std::isdigit((unsigned char)s[i])) return std::nullopt;
        d = d * 10 + (s[i] - '0');
    }
    date::year_month_day ymd =
        date::year{y} / date::month{(unsigned)m} / date::day{(unsigned)d};
    if (!ymd.ok()) return std::nullopt;
    return ymd;
}

// Civil-time-aware duration → milliseconds.
// `cursor` is the current civil date; it is advanced in-place for Y and Mo
// components so that successive calls accumulate correctly through a plan.
//
// Fractions on Y/Mo fall back to fixed unit_ms (calendar fractions are
// ambiguous; this matches the Timex convention for sub-unit precision).
// Returns -1 on any parse error (same contract as tw_parse_duration_ms).
inline int64_t tw_civil_duration_ms(const std::string &dur,
                                     date::year_month_day &cursor) {
    using namespace tw_duration_detail;
    using namespace date;
    if (dur.empty() || dur[0] != 'P') return -1;
    if (dur.size() == 1) return 0;

    size_t  i         = 1;
    int64_t total_ms  = 0;
    bool    in_time   = false;
    bool    saw_t     = false;
    bool    frac_seen = false;
    bool    saw_w     = false;
    bool    saw_any   = false;
    int     last_rank = -1;

    while (i < dur.size()) {
        if (frac_seen) return -1;

        if (dur[i] == 'T') {
            if (saw_t) return -1;
            if (i + 1 >= dur.size()) return -1;
            saw_t = true; in_time = true; ++i; continue;
        }

        if (!std::isdigit((unsigned char)dur[i])) return -1;

        int64_t whole = 0;
        while (i < dur.size() && std::isdigit((unsigned char)dur[i]))
            whole = whole * 10 + (dur[i++] - '0');

        int64_t frac_milli = 0;
        bool    has_frac   = false;
        if (i < dur.size() && dur[i] == '.') {
            ++i;
            if (i >= dur.size() || !std::isdigit((unsigned char)dur[i])) return -1;
            has_frac = true;
            int64_t acc = 0, n = 0;
            while (i < dur.size() && std::isdigit((unsigned char)dur[i]) && n < 3)
                acc = acc * 10 + (dur[i++] - '0'), ++n;
            while (i < dur.size() && std::isdigit((unsigned char)dur[i])) ++i;
            for (int64_t k = n; k < 3; ++k) acc *= 10;
            frac_milli = acc;
        }

        if (i >= dur.size()) return -1;
        char unit_c = dur[i++];

        int     rank;
        int64_t unit_ms_fixed;
        if (!in_time) {
            if      (unit_c == 'Y') { rank = 0;  unit_ms_fixed = MS_Y;  }
            else if (unit_c == 'M') { rank = 1;  unit_ms_fixed = MS_MO; }
            else if (unit_c == 'W') { rank = 99; unit_ms_fixed = MS_W;  }
            else if (unit_c == 'D') { rank = 2;  unit_ms_fixed = MS_D;  }
            else return -1;
        } else {
            if      (unit_c == 'H') { rank = 3; unit_ms_fixed = MS_H;  }
            else if (unit_c == 'M') { rank = 4; unit_ms_fixed = MS_MI; }
            else if (unit_c == 'S') { rank = 5; unit_ms_fixed = MS_S;  }
            else return -1;
        }

        if (unit_c == 'W' && (saw_any || in_time)) return -1;
        if (saw_w) return -1;
        if (unit_c == 'W') saw_w = true;
        if (rank != 99 && rank <= last_rank) return -1;
        last_rank = (rank == 99) ? last_rank : rank;

        // Y and Mo: calendar arithmetic via date.h
        if (!in_time && (unit_c == 'Y' || unit_c == 'M') && cursor.ok()) {
            auto from_sys = sys_days{cursor};
            if (unit_c == 'Y') {
                cursor = year_month_day{
                    cursor.year() + years{(int)whole}, cursor.month(), cursor.day()};
            } else {
                cursor = year_month_day{
                    cursor.year(), cursor.month() + months{(int)whole}, cursor.day()};
            }
            // Clamp day to last valid day of the resulting month (e.g. Jan 31 + 1Mo → Feb 28/29)
            if (!cursor.ok())
                cursor = cursor.year() / cursor.month() / last;
            int64_t days = (sys_days{cursor} - from_sys).count();
            total_ms += days * 86400LL * 1000;
            // Fractions on Y/Mo: fixed fallback (calendar fractions are ambiguous)
            if (has_frac) total_ms += frac_milli * (unit_ms_fixed / 1000);
        } else {
            total_ms += whole * unit_ms_fixed;
            if (has_frac) total_ms += frac_milli * (unit_ms_fixed / 1000);
        }

        if (has_frac) frac_seen = true;
        saw_any = true;
    }

    return total_ms;
}

// Civil-time-aware temporal check.  Identical to tw_check_temporal except:
//   - reference_date "YYYY-MM-DD": Y and Mo durations use calendar arithmetic.
//   - reference_date "": falls back to tw_parse_duration_ms (Timex fixed days).
// The civil cursor accumulates through the plan so that each action's Y/Mo
// duration is measured from the civil date at which that action starts.
inline TwTemporalResult tw_check_temporal_civil(
        const std::vector<TwCall> &plan,
        const TwDomain            &domain,
        const std::string         &origin_iso     = "PT0S",
        const std::string         &reference_date = "") {
    TwTemporalResult r;
    r.consistent = true;
    r.origin_iso = origin_iso;

    std::optional<date::year_month_day> civil_cursor;
    if (!reference_date.empty())
        civil_cursor = tw_parse_date(reference_date);

    int64_t origin_ms = tw_parse_duration_ms(origin_iso);
    if (origin_ms < 0) origin_ms = 0;

    if (plan.empty()) { r.total_iso = "PT0S"; return r; }

    tw_detail::STN stn;
    stn.add_point("t0");
    std::string prev_end = "t0";
    int64_t current  = origin_ms;
    int64_t total_ms = 0;

    for (size_t i = 0; i < plan.size(); ++i) {
        const std::string &name = plan[i].name;

        int64_t     dur_ms  = 0;
        std::string dur_iso;
        auto dit = domain.action_durations.find(name);
        if (dit != domain.action_durations.end()) {
            dur_iso = dit->second;
            int64_t parsed = (civil_cursor && civil_cursor->ok())
                ? tw_civil_duration_ms(dur_iso, *civil_cursor)
                : tw_parse_duration_ms(dur_iso);
            if (parsed >= 0) dur_ms = parsed;
        }

        TwTemporalStep step;
        step.action_name  = name;
        step.duration_iso = dur_iso.empty() ? "PT0S" : dur_iso;
        step.start_iso    = tw_format_duration_ms(current);
        step.end_iso      = tw_format_duration_ms(current + dur_ms);
        r.steps.push_back(std::move(step));

        current  += dur_ms;
        total_ms += dur_ms;

        double dur_s    = static_cast<double>(dur_ms) / 1000.0;
        std::string a_s = "a" + std::to_string(i) + "_start";
        std::string a_e = "a" + std::to_string(i) + "_end";
        stn.add_constraint(prev_end, a_s, 0.0, 0.0);
        stn.add_constraint(a_s,      a_e, dur_s, dur_s);
        prev_end = a_e;
    }

    r.consistent = stn.consistent();
    r.total_iso  = tw_format_duration_ms(total_ms);
    return r;
}

// Serialise a plan + TwTemporalResult as a JSON object.
inline std::string tw_temporal_to_json(const std::vector<TwCall> &plan,
                                        const TwTemporalResult   &tr,
                                        const std::string        &plan_json) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"plan\": " << plan_json << ",\n";
    o << "  \"temporal\": {\n";
    o << "    \"consistent\": " << (tr.consistent ? "true" : "false") << ",\n";
    o << "    \"origin\": \"" << tr.origin_iso << "\",\n";
    o << "    \"total\": \"" << tr.total_iso << "\",\n";
    o << "    \"steps\": [";
    for (size_t i = 0; i < tr.steps.size(); ++i) {
        if (i) o << ", ";
        o << "{\"action\": \"" << tr.steps[i].action_name
          << "\", \"duration\": \"" << tr.steps[i].duration_iso
          << "\", \"start\": \"" << tr.steps[i].start_iso
          << "\", \"end\": \"" << tr.steps[i].end_iso << "\"}";
    }
    o << "]\n";
    o << "  }\n";
    o << "}";
    return o.str();
}
