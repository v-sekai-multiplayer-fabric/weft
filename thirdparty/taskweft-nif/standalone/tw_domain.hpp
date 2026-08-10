// Taskweft domain, goal, and task types — pure C++20, no Godot dependency.
#pragma once
#include "tw_state.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Primitive or compound task call: [name, arg1, arg2, ...]
struct TwCall {
    std::string          name;
    std::vector<TwValue> args;
};

// One binding in a conjunctive goal: (var, key, desired).
// Maps to IPyHOP unigoal ('var', 'key', desired_val).
//
// Satisfaction strategy (in priority order):
//  1. ReBAC check — if state.rebac_graph is set and non-empty, AND desired
//     is a string (a ReBAC object is always an entity name, never a bool/
//     int/float/array/dict; this is what actually distinguishes a relation
//     check from an ordinary state binding, not merely "a graph exists
//     somewhere in this domain" -- see the bug note below):
//     • var is a JSON object  → parsed as a full RelationExpr (union, intersection,
//       difference, tuple_to_userset, …) and evaluated via check_expr.
//     • var is a plain string → auto-wrapped as {"type":"base","rel":var}
//       covering all relation types with IS_MEMBER_OF inheritance.
//     key = subject entity, desired = object entity.
//  2. Plain equality fallback — state[var][key] == desired (legacy behaviour).
//
// Real bug fixed here: this used to gate step 1 on "a graph exists" alone,
// with no check on desired's type at all. Any domain that declares
// "capabilities" (building a real, non-empty rebac_graph) silently routed
// every ordinary TwGoal/TwMultiGoal binding, even a plain boolean check
// like {var: "delivered", key: "package_1", desired: true}, into
// check_expr instead of state equality. desired.as_string() on a bool
// TwValue returns the unset default "" (TwValue::as_string() just reads
// the internal string field, never populated for a bool), so check_expr
// could never match a real edge, and the binding failed unconditionally
// -- not a rare edge case, but every ordinary goal binding in any domain
// that also happens to use "capabilities", confirmed live: a two-step
// warehouse-delivery domain with real "capabilities" entities returned
// no_plan for its own goal-based todo_list while the identical plan
// built from direct action-sequence calls (no goal binding involved)
// succeeded. Gating on desired.is_string() fixes it with no per-domain
// configuration: a real ReBAC object is always a string entity name, so
// this is a sound, general signal already present in the data, not a
// heuristic tuned to any one domain's variable names.
struct TwGoalBinding {
    std::string var;
    std::string key;
    TwValue     desired;

    bool satisfied(const TwState &state) const {
        if ((state.rebac_graph) && (!state.rebac_graph->edges.empty()) &&
                desired.is_string()) {
            TwValue expr;
            if (!var.empty() && var.front() == '{') {
                expr = TwJson::parse_json_str(var);
                // Guard: malformed JSON yields NIL, not a dict — fail safely.
                if (!expr.is_dict()) return false;
            } else {
                TwValue::Dict m;
                m["type"] = TwValue(std::string("base"));
                m["rel"]  = TwValue(var);
                expr = TwValue(std::move(m));
            }
            return TwReBAC::check_expr(*state.rebac_graph, key, expr,
                                       desired.as_string(), state.rebac_fuel);
        }
        return state.get_nested(var, TwValue(key)) == desired;
    }
};

// Conjunctive goal: a list of (var, key, desired) bindings.
// The planner keeps it in the task list until every binding is satisfied,
// trying each unsatisfied binding as a subtask (with backtracking over ordering).
struct TwGoal {
    std::vector<TwGoalBinding> bindings;

    bool is_satisfied(const TwState &state) const {
        for (auto &b : bindings)
            if (!b.satisfied(state)) return false;
        return true;
    }

    std::vector<TwGoalBinding> unsatisfied(const TwState &state) const {
        std::vector<TwGoalBinding> unmet;
        for (auto &b : bindings)
            if (!b.satisfied(state)) unmet.push_back(b);
        return unmet;
    }
};

// Multigoal: same binding structure as TwGoal but decomposed by the planner
// with backtracking over which unsatisfied binding to satisfy first
// (IPyHOP MultiGoal / RECTGTN 'N'). Each unsatisfied binding becomes a
// single-binding TwGoal subtask; the MultiGoal is re-queued until done.
struct TwMultiGoal {
    std::vector<TwGoalBinding> bindings;

    bool is_satisfied(const TwState &state) const {
        for (const TwGoalBinding &b : bindings)
            if (!b.satisfied(state)) return false;
        return true;
    }

    std::vector<TwGoalBinding> unsatisfied(const TwState &state) const {
        std::vector<TwGoalBinding> unmet;
        for (const TwGoalBinding &b : bindings)
            if (!b.satisfied(state)) unmet.push_back(b);
        return unmet;
    }
};

// A task list item is either a task call, a conjunctive goal, or a multigoal.
using TwTask = std::variant<TwCall, TwGoal, TwMultiGoal>;

// Action: (state_copy, args) → new_state | nullptr
using TwActionFn =
    std::function<std::shared_ptr<TwState>(std::shared_ptr<TwState>, std::vector<TwValue>)>;

// Task method: (state, args) → subtask_list | nullopt
using TwMethodFn =
    std::function<std::optional<std::vector<TwTask>>(std::shared_ptr<TwState>, std::vector<TwValue>)>;

// A goal method is called with args=[key, desired] to try to satisfy a
// TwGoalBinding on the var it's registered under — mechanically identical
// to an ordinary method call, just with that specific two-arg convention.
// There is no separate storage for it: it's registered under task_methods
// like any other method (by the state var name it targets), so TwGoal/
// TwMultiGoal binding resolution looks it up there — see tw_planner.hpp's
// unmet-binding dispatch.
using TwGoalMethodFn = TwMethodFn;

struct TwDomain {
    std::unordered_map<std::string, TwActionFn>              actions;
    std::unordered_map<std::string, std::vector<TwMethodFn>> task_methods;
    // ISO 8601 duration strings per action (RECTGTN 'T' temporal metadata).
    std::unordered_map<std::string, std::string>             action_durations;

    bool has_action(const std::string &n) const { return actions.count(n) > 0; }
    bool has_task(const std::string &n)   const { return task_methods.count(n) > 0; }
};
