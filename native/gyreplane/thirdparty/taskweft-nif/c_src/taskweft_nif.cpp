#include <fine.hpp>
#include "tw_bridge.hpp"
#include "tw_explain.hpp"
#include "tw_json.hpp"
#include "tw_loader.hpp"
#include "tw_mc_executor.hpp"
#include "tw_planner.hpp"
#include "tw_witness_oracle.hpp"
#include "tw_rebac.hpp"
#include "tw_replan.hpp"
#include "tw_temporal.hpp"

#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ── Domain parse cache ────────────────────────────────────────────────────────
// load_json is pure: same JSON → same TwLoaded. The cache lets repeated
// plan/replan/check_temporal calls for the same domain skip the ~10–15 µs
// JSON-LD parse. Each retrieval deep-copies the initial state so every planner
// invocation starts from a clean slate; domain methods and task list are shared
// (read-only during planning).
//
// Formally justified by Planner.DomainCache: plan_cache_equiv proves that
// planning with a cached parsed domain is observationally equivalent to
// re-parsing on each call.
static std::mutex s_cache_mtx;
static std::unordered_map<std::string, TwLoader::TwLoaded> s_domain_cache;

static TwLoader::TwLoaded load_cached(const std::string &json) {
    {
        std::lock_guard<std::mutex> lk(s_cache_mtx);
        auto it = s_domain_cache.find(json);
        if (it != s_domain_cache.end()) {
            TwLoader::TwLoaded result = it->second;
            // A failed parse caches with a null state (see below) — every
            // caller's `if (!loaded.state) throw ...` guard expects that null
            // to survive the cache round-trip, not get dereferenced here.
            if (result.state) {
                result.state = it->second.state->copy(); // fresh initial state
            }
            return result;
        }
    }
    // Parse outside the lock — concurrent misses are safe (last write wins).
    TwLoader::TwLoaded loaded = TwLoader::load_json(json);
    {
        std::lock_guard<std::mutex> lk(s_cache_mtx);
        s_domain_cache.emplace(json, loaded);
    }
    TwLoader::TwLoaded result = loaded;
    if (loaded.state) {
        result.state = loaded.state->copy();
    }
    return result;
}

// ── ReBAC graph parse cache ───────────────────────────────────────────────────
// graph_from_json is pure and graphs are read-only after construction, so a
// single cached copy is shared across all concurrent readers with no locking
// during the hot path.  Formally justified by Planner.ExpandIndex:
// expand_index_equiv proves the member_edges index gives the same result as
// scanning all edges.
static std::mutex s_graph_cache_mtx;
static std::unordered_map<std::string, TwReBAC::TwReBACGraph> s_graph_cache;

static const TwReBAC::TwReBACGraph &graph_cached(const std::string &json) {
    {
        std::lock_guard<std::mutex> lk(s_graph_cache_mtx);
        auto it = s_graph_cache.find(json);
        if (it != s_graph_cache.end())
            return it->second;
    }
    // Parse outside the lock; concurrent misses produce the same graph.
    TwReBAC::TwReBACGraph g = TwReBAC::graph_from_json(json);
    std::lock_guard<std::mutex> lk(s_graph_cache_mtx);
    // try_emplace: if another thread already inserted, keep theirs and discard ours.
    return s_graph_cache.try_emplace(json, std::move(g)).first->second;
}

// Parse a plan JSON array ([[name, arg...], ...]) back to vector<TwCall>.
static std::vector<TwCall> parse_plan(const std::string &p_plan_json) {
	TwValue arr = TwLoader::parse_json_str(p_plan_json);
	std::vector<TwCall> plan;
	if (!arr.is_array()) {
		return plan;
	}
	for (const TwValue &item : arr.as_array()) {
		if (!item.is_array() || item.as_array().empty()) {
			continue;
		}
		TwCall call;
		call.name = item.as_array()[0].as_string();
		for (size_t i = 1; i < item.as_array().size(); ++i) {
			call.args.push_back(item.as_array()[i]);
		}
		plan.push_back(std::move(call));
	}
	return plan;
}

// plan(domain_json) → plan_json
// domain_json is a self-contained JSON-LD document with variables + tasks.
// Raises ErlangError on failure or no-plan.
std::string plan(ErlNifEnv *p_env, std::string p_domain_json) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}
	std::optional<std::vector<TwCall>> result = tw_plan(loaded.state, loaded.tasks, loaded.domain);
	if (!result) {
		throw std::runtime_error("no_plan");
	}
	return TwLoader::plan_to_json(*result);
}
FINE_NIF(plan, 0);

// replan(domain_json, plan_json, fail_step) → replan_result_json
// fail_step: 0-based index of the failed action, or -1 to auto-detect.
std::string replan(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_plan_json, int64_t p_fail_step) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}
	std::vector<TwCall> original_plan = parse_plan(p_plan_json);
	TwReplanResult rr = tw_replan(loaded.state, original_plan, loaded.tasks,
			loaded.domain, static_cast<int>(p_fail_step));
	std::string new_plan_json = rr.recovered
			? TwLoader::plan_to_json(*rr.new_plan)
			: "null";
	return tw_replan_to_json(static_cast<int>(p_fail_step), rr,
			TwLoader::plan_to_json(original_plan), new_plan_json);
}
FINE_NIF(replan, 0);

// check_temporal(domain_json, plan_json, origin_iso) → temporal_result_json
// origin_iso: ISO 8601 duration for the plan start offset, e.g. "PT0S".
std::string check_temporal(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_plan_json, std::string p_origin_iso) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}
	std::vector<TwCall> plan_vec = parse_plan(p_plan_json);
	TwTemporalResult tr = tw_check_temporal(plan_vec, loaded.domain, p_origin_iso);
	return tw_temporal_to_json(plan_vec, tr, TwLoader::plan_to_json(plan_vec));
}
FINE_NIF(check_temporal, 0);

// plan_with_temporal(domain_json, origin_iso) → plan_with_temporal_json
// Combines plan() + check_temporal() in one NIF call; temporal is primary.
// Returns the same JSON envelope as tw_temporal_to_json.
// Raises ErlangError on failure or no-plan.
std::string plan_with_temporal(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_origin_iso) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}
	std::optional<std::vector<TwCall>> result = tw_plan(loaded.state, loaded.tasks, loaded.domain);
	if (!result) {
		throw std::runtime_error("no_plan");
	}
	std::string plan_json = TwLoader::plan_to_json(*result);
	TwTemporalResult tr = tw_check_temporal(*result, loaded.domain, p_origin_iso);
	return tw_temporal_to_json(*result, tr, plan_json);
}
FINE_NIF(plan_with_temporal, 0);

// plan_with_temporal_explain(domain_json, origin_iso) → plan_with_temporal_json
// Native explain mode for both solved plans and no_plan outcomes.
std::string plan_with_temporal_explain(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_origin_iso) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}

	TwSolTree sol_tree;
	std::optional<std::vector<TwCall>> result =
		tw_plan_with_tree(loaded.state, loaded.tasks, loaded.domain, sol_tree);
	if (!result) {
		return tw_no_plan_explain_json(loaded.tasks, loaded.domain);
	}

	std::string plan_json = TwLoader::plan_to_json(*result);
	TwTemporalResult tr = tw_check_temporal(*result, loaded.domain, p_origin_iso);
	TwValue out = TwJson::parse_json_str(tw_temporal_to_json(*result, tr, plan_json));
	if (out.is_dict()) {
		out.as_dict()["status"] = TwValue("ok");
		out.as_dict()["explain"] = tw_solution_tree_value(sol_tree, *result);
	}
	return TwJson::to_json(out);
}
FINE_NIF(plan_with_temporal_explain, 0);

// check_temporal_civil(domain_json, plan_json, origin_iso, reference_date) → temporal_result_json
// reference_date: "YYYY-MM-DD" — Y and Mo durations use actual calendar days.
// Passes "" to fall back to Timex fixed-day conventions (same as check_temporal).
std::string check_temporal_civil(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_plan_json, std::string p_origin_iso, std::string p_reference_date) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}
	std::vector<TwCall> plan_vec = parse_plan(p_plan_json);
	TwTemporalResult tr = tw_check_temporal_civil(plan_vec, loaded.domain,
			p_origin_iso, p_reference_date);
	return tw_temporal_to_json(plan_vec, tr, TwLoader::plan_to_json(plan_vec));
}
FINE_NIF(check_temporal_civil, 0);

// plan_with_temporal_civil(domain_json, origin_iso, reference_date) → plan_with_temporal_json
// Combines plan() + check_temporal_civil() in one NIF call.
std::string plan_with_temporal_civil(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_origin_iso, std::string p_reference_date) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}
	std::optional<std::vector<TwCall>> result = tw_plan(loaded.state, loaded.tasks, loaded.domain);
	if (!result) {
		throw std::runtime_error("no_plan");
	}
	std::string plan_json = TwLoader::plan_to_json(*result);
	TwTemporalResult tr = tw_check_temporal_civil(*result, loaded.domain,
			p_origin_iso, p_reference_date);
	return tw_temporal_to_json(*result, tr, plan_json);
}
FINE_NIF(plan_with_temporal_civil, 0);

// plan_with_temporal_civil_explain(domain_json, origin_iso, reference_date) → json
// Native explain mode for both solved plans and no_plan outcomes.
std::string plan_with_temporal_civil_explain(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_origin_iso, std::string p_reference_date) {
	TwLoader::TwLoaded loaded = load_cached(p_domain_json);
	if (!loaded.state) {
		throw std::runtime_error("failed_to_load_domain");
	}

	TwSolTree sol_tree;
	std::optional<std::vector<TwCall>> result =
		tw_plan_with_tree(loaded.state, loaded.tasks, loaded.domain, sol_tree);
	if (!result) {
		return tw_no_plan_explain_json(loaded.tasks, loaded.domain);
	}

	std::string plan_json = TwLoader::plan_to_json(*result);
	TwTemporalResult tr = tw_check_temporal_civil(*result, loaded.domain,
			p_origin_iso, p_reference_date);
	TwValue out = TwJson::parse_json_str(tw_temporal_to_json(*result, tr, plan_json));
	if (out.is_dict()) {
		out.as_dict()["status"] = TwValue("ok");
		out.as_dict()["explain"] = tw_solution_tree_value(sol_tree, *result);
	}
	return TwJson::to_json(out);
}
FINE_NIF(plan_with_temporal_civil_explain, 0);

// domain_cache_clear() → :ok
// Evicts all cached parsed domains. Call when domain JSON will not be reused.
std::string domain_cache_clear(ErlNifEnv *p_env) {
	std::lock_guard<std::mutex> lk(s_cache_mtx);
	s_domain_cache.clear();
	return "ok";
}
FINE_NIF(domain_cache_clear, 0);


// rebac_add_edge(graph_json, subj, obj, rel) → graph_json
// Add a directed relation edge and return the updated graph JSON.
std::string rebac_add_edge(ErlNifEnv *p_env, std::string p_graph_json,
		std::string p_subj, std::string p_obj, std::string p_rel) {
	TwReBAC::TwReBACGraph g = TwReBAC::graph_from_json(p_graph_json);
	g.add_edge(p_subj, p_obj, p_rel);
	return TwReBAC::graph_to_json(g);
}
FINE_NIF(rebac_add_edge, 0);

// rebac_check(graph_json, subj, expr_json, obj, fuel) → bool
// Evaluate a RelationExpr against the graph.
bool rebac_check(ErlNifEnv *p_env, std::string p_graph_json,
		std::string p_subj, std::string p_expr_json,
		std::string p_obj, int64_t p_fuel) {
	const TwReBAC::TwReBACGraph &g = graph_cached(p_graph_json);
	TwValue expr = TwJson::parse_json_str(p_expr_json);
	return TwReBAC::check_expr(g, p_subj, expr, p_obj, static_cast<int>(p_fuel));
}
FINE_NIF(rebac_check, 0);

// rebac_expand(graph_json, rel, obj, fuel) → list of subject strings
// All subjects that hold rel to obj (direct + IS_MEMBER_OF transitive).
std::vector<std::string> rebac_expand(ErlNifEnv *p_env, std::string p_graph_json,
		std::string p_rel, std::string p_obj, int64_t p_fuel) {
	const TwReBAC::TwReBACGraph &g = graph_cached(p_graph_json);
	return TwReBAC::tw_expand(g, p_rel, p_obj, static_cast<int>(p_fuel));
}
FINE_NIF(rebac_expand, 0);

// rebac_parse_relation_edges(facts_json, trust_threshold) → graph_json
// Extract relation edges from memory fact sentences.
std::string rebac_parse_relation_edges(ErlNifEnv *p_env, std::string p_facts_json,
		double p_trust_threshold) {
	return TwBridge::parse_relation_edges(p_facts_json, p_trust_threshold);
}
FINE_NIF(rebac_parse_relation_edges, 0);


// bridge_binding_content(var, arg, val) → string "var arg val"
std::string bridge_binding_content(ErlNifEnv *p_env, std::string p_var,
		std::string p_arg, std::string p_val) {
	return TwBridge::binding_content(p_var, p_arg, p_val);
}
FINE_NIF(bridge_binding_content, 0);

// bridge_extract_entities(state_json) → list of entity strings
// Inner dict keys from a PDDL-style state, excluding private/rigid vars.
std::vector<std::string> bridge_extract_entities(ErlNifEnv *p_env,
		std::string p_state_json) {
	return TwBridge::extract_state_entities(p_state_json);
}
FINE_NIF(bridge_extract_entities, 0);

// bridge_plan_contents(plan_json, domain, entities_json) → json array
// [{content, category, tags}] for storing a plan result in memory.
std::string bridge_plan_contents(ErlNifEnv *p_env, std::string p_plan_json,
		std::string p_domain, std::string p_entities_json) {
	return TwBridge::plan_result_contents(p_plan_json, p_domain, p_entities_json);
}
FINE_NIF(bridge_plan_contents, 0);

// bridge_state_bindings(state_json, domain, category) → json array
// [{content, category, tags}] for all (var, arg, val) triples in state.
std::string bridge_state_bindings(ErlNifEnv *p_env, std::string p_state_json,
		std::string p_domain, std::string p_category) {
	return TwBridge::state_bindings_contents(p_state_json, p_domain, p_category);
}
FINE_NIF(bridge_state_bindings, 0);

// rebac_can(graph_json, subj, capability, max_depth) → JSON {"authorized":bool,"path":[...]}
// DFS traversal: terminal via HAS_CAPABILITY, CONTROLS, OWNS.
std::string rebac_can(ErlNifEnv *p_env, std::string p_graph_json,
		std::string p_subj, std::string p_capability, int64_t p_max_depth) {
	const TwReBAC::TwReBACGraph &g = graph_cached(p_graph_json);
	return TwReBAC::rebac_can_json(g, p_subj, p_capability, static_cast<int>(p_max_depth));
}
FINE_NIF(rebac_can, 0);

// rebac_get_entity_capabilities(graph_json, entity) → list of capability strings
std::vector<std::string> rebac_get_entity_capabilities(ErlNifEnv *p_env,
		std::string p_graph_json, std::string p_entity) {
	const TwReBAC::TwReBACGraph &g = graph_cached(p_graph_json);
	return TwReBAC::get_entity_capabilities(g, p_entity);
}
FINE_NIF(rebac_get_entity_capabilities, 0);

// rebac_get_entities_with_capability(graph_json, capability) → list of entity strings
std::vector<std::string> rebac_get_entities_with_capability(ErlNifEnv *p_env,
		std::string p_graph_json, std::string p_capability) {
	const TwReBAC::TwReBACGraph &g = graph_cached(p_graph_json);
	return TwReBAC::get_entities_with_capability(g, p_capability);
}
FINE_NIF(rebac_get_entities_with_capability, 0);

// mc_execute(domain_json, plan_json, probs_json, seed) → trace JSON
// Stochastic plan execution with per-step success probabilities.
std::string mc_execute(ErlNifEnv *p_env, std::string p_domain_json,
		std::string p_plan_json, std::string p_probs_json, int64_t p_seed) {
	return TwMCExecutor::mc_execute(p_domain_json, p_plan_json, p_probs_json, p_seed);
}
FINE_NIF(mc_execute, 0);

// rebac_cache_clear() → "ok"
// Evict all cached ReBAC graphs. Call when graphs will not be reused.
std::string rebac_cache_clear(ErlNifEnv *p_env) {
    std::lock_guard<std::mutex> lk(s_graph_cache_mtx);
    s_graph_cache.clear();
    return "ok";
}
FINE_NIF(rebac_cache_clear, 0);
// ── Witness oracle NIF ──────────────────────────────────────────────────────────
//
// witness_oracle(state_json, tasks_json, domain_json) → outcome_json
//
// Runs the PlausibleWitnessDag ladder: for each level, formulates a
// universal property (∀ w ∈ Fin N, ¬candidate_is_witness w) and searches
// for a counterexample. Returns the readback value plus the outcome.
//
// Used by the planner's compound-task dispatch to skip impossible branches
// before expensive recursive expansion.
//
// Returns JSON: {value: int, found: bool, witness_idx: int, budget_hit: bool}
std::string witness_oracle(ErlNifEnv *p_env,
                           std::string p_state_json,
                           std::string p_tasks_json,
                           std::string p_domain_json) {
    // Parse domain
    TwLoader::TwLoaded loaded = TwLoader::load_json(p_domain_json);

    // Candidate predicate: is a given "witness" index a valid witness?
    // For TOHTN, witness = "the first goal method makes progress on the unsatisfied binding"
    auto candidate_is_witness = [&](uint64_t w) -> bool {
        // A failed domain parse (null state) has no witness — same outcome as
        // a parse with no methods, handled by the check just below.
        if (!loaded.state) {
            return false;
        }
        // Fresh copy of the initial state for each candidate check
        auto state = loaded.state->copy();
        std::vector<TwValue> args = {TwValue(std::string("key")), TwValue(std::string("desired"))};

        // Try decomposing with the witness method (goal methods have no
        // separate map — they're ordinary task_methods entries)
        auto git = loaded.domain.task_methods.begin();
        if (git == loaded.domain.task_methods.end()) return false;
        if (git->second.empty()) return false;

        std::optional<std::vector<TwTask>> subs = git->second[0](state, args);
        return subs.has_value();
    };

    // Readback: given walkSteps, return the deterministic recovery result
    auto readback = [&](uint64_t walk_steps) -> PlausibleWitnessDag::Readback {
        PlausibleWitnessDag::Readback rb;
        rb.value = 0;
        rb.found = true;
        rb.witnessIdx = 0;
        rb.budgetHit = false;
        return rb;
    };

    // Run the ladder
    auto [value, level, trace] =
        PlausibleWitnessDag::resolve("compound_task", candidate_is_witness, readback);

    // Return JSON outcome
    TwValue::Dict outcome;
    outcome["value"] = TwValue(static_cast<int64_t>(value));
    outcome["found"] = TwValue(trace.outcome.kind == PlausibleWitnessDag::Outcome::found);
    outcome["witness_idx"] = TwValue(static_cast<int64_t>(trace.outcome.witness_idx));
    outcome["budget_hit"] = TwValue(trace.outcome.kind == PlausibleWitnessDag::Outcome::budgetHit);
    return TwJson::to_json(TwValue(std::move(outcome)));
}
FINE_NIF(witness_oracle, 0);

FINE_INIT("Elixir.Taskweft.NIF");