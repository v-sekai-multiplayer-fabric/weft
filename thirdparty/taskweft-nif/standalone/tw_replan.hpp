// RECTGTN 'R' — Replan: simulate plan execution and recover from action failure.
// Mirrors Python plan_jsonld.py _do_simulate() and _do_replan(), and
// IPyHOP planner.blacklist_command() for command-vs-action distinction.
#pragma once
#include "tw_domain.hpp"
#include "tw_planner.hpp"
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Result of simulating a plan step-by-step.
struct TwSimulateResult {
    int completed_steps;             // number of actions successfully applied
    int fail_step;                   // index of the failed action, or -1 if all succeeded
    std::string fail_action;         // name of the failed action, or ""
    std::shared_ptr<TwState> state;  // state after completed_steps actions
};

// Apply plan actions one by one, stopping at the first failure.
// Mirrors _do_simulate() in plan_jsonld.py.
inline TwSimulateResult tw_simulate(
        std::shared_ptr<TwState> init_state,
        const std::vector<TwCall> &plan,
        const TwDomain &domain) {
    TwSimulateResult r;
    r.completed_steps = 0;
    r.fail_step       = -1;

    std::shared_ptr<TwState> cur = init_state->copy();
    for (int i = 0; i < (int)plan.size(); ++i) {
        std::unordered_map<std::string, TwActionFn>::const_iterator it =
            domain.actions.find(plan[i].name);
        if (it == domain.actions.end()) {
            r.fail_step   = i;
            r.fail_action = plan[i].name;
            r.state       = cur;
            return r;
        }
        std::shared_ptr<TwState> next = it->second(cur, plan[i].args);
        if (!next) {
            r.fail_step   = i;
            r.fail_action = plan[i].name;
            r.state       = cur;
            return r;
        }
        cur = next;
        r.completed_steps = i + 1;
    }
    r.state = cur;
    return r;
}

// Result of a replan operation.
struct TwReplanResult {
    TwSimulateResult simulate;        // how far the original plan ran
    std::optional<std::vector<TwCall>> new_plan;  // recovered plan, or nullopt
    bool recovered;                   // true if new_plan was found
    TwBlacklist blacklist;            // commands blacklisted for this replan
};

// Simulate original_plan up to fail_step (or until first failure if fail_step < 0).
// Then replan from the state at failure using the original task list.
//
// The failed command is blacklisted (mirrors IPyHOP blacklist_command): the planner
// will not re-select the exact same (action, args) instance that failed at runtime,
// forcing it to find an alternative.  This is the key command-vs-action distinction:
//   action  = the function definition (TwActionFn)
//   command = a specific instantiation with concrete args (TwCall) that can be
//             individually blacklisted when it fails at execution time.
inline TwReplanResult tw_replan(
        std::shared_ptr<TwState> init_state,
        const std::vector<TwCall> &original_plan,
        const std::vector<TwTask> &original_tasks,
        const TwDomain &domain,
        int fail_step = -1) {
    TwReplanResult r;

    // Simulate to determine state at failure.
    if (fail_step < 0 || fail_step >= (int)original_plan.size()) {
        r.simulate = tw_simulate(init_state, original_plan, domain);
    } else {
        // Simulate only up to the specified fail_step to get pre-failure state.
        std::vector<TwCall> prefix(original_plan.begin(),
                                   original_plan.begin() + fail_step);
        TwSimulateResult partial = tw_simulate(init_state, prefix, domain);
        r.simulate.completed_steps = fail_step;
        r.simulate.fail_step       = fail_step;
        r.simulate.fail_action     = original_plan[fail_step].name;
        r.simulate.state           = partial.state;
    }

    std::shared_ptr<TwState> replan_state =
        r.simulate.state ? r.simulate.state : init_state;

    // Blacklist the specific command that failed at runtime so the replanner
    // is forced to find an alternative path (not just retry the same step).
    if (r.simulate.fail_step >= 0 && r.simulate.fail_step < (int)original_plan.size())
        r.blacklist.insert(tw_call_key(original_plan[r.simulate.fail_step]));

    r.new_plan  = tw_plan(replan_state, original_tasks, domain, &r.blacklist);
    r.recovered = r.new_plan.has_value();
    return r;
}

// Incremental replan using a solution tree from a previous tw_plan_with_tree call.
//
// Instead of restarting the full search, this locates the nearest T/G ancestor
// of the failed action in the tree, simulates only the plan prefix before that
// ancestor's subtree, and replans from there — skipping the method choice that
// produced the failed plan.  Equivalent to IPyHOP's _post_failure_modify +
// _backtrack + resume, without requiring a persistent solution-tree object.
//
// Returns a full plan (prefix + recovered suffix).
inline TwReplanResult tw_replan_incremental(
        std::shared_ptr<TwState>   init_state,
        const std::vector<TwCall>  &original_plan,
        const std::vector<TwTask>  &original_tasks,
        const TwDomain             &domain,
        const TwSolTree            &sol_tree,
        int                        fail_step = -1) {
    TwReplanResult r;

    // Determine fail_step if not given.
    if (fail_step < 0 || fail_step >= (int)original_plan.size()) {
        r.simulate = tw_simulate(init_state, original_plan, domain);
    } else {
        std::vector<TwCall> pre(original_plan.begin(),
                                original_plan.begin() + fail_step);
        TwSimulateResult partial = tw_simulate(init_state, pre, domain);
        r.simulate.completed_steps = fail_step;
        r.simulate.fail_step       = fail_step;
        r.simulate.fail_action     = original_plan[fail_step].name;
        r.simulate.state           = partial.state;
    }

    if (r.simulate.fail_step < 0) {
        // No failure: nothing to do.
        r.new_plan  = original_plan;
        r.recovered = true;
        return r;
    }

    r.blacklist.insert(tw_call_key(original_plan[r.simulate.fail_step]));

    // Find the nearest T/G ancestor with an untried method alternative.
    int ancestor = -1;
    if (r.simulate.fail_step < (int)sol_tree.action_nodes.size())
        ancestor = sol_tree.nearest_retryable_ancestor(
                       sol_tree.action_nodes[r.simulate.fail_step], domain);

    if (ancestor < 0) {
        // No retryable ancestor — fall back to full replan from failure state.
        std::shared_ptr<TwState> rs = r.simulate.state ? r.simulate.state : init_state;
        r.new_plan  = tw_plan(rs, original_tasks, domain, &r.blacklist);
        r.recovered = r.new_plan.has_value();
        return r;
    }

    // Simulate the prefix that precedes the ancestor's subtree.
    int prefix_len = sol_tree.prefix_length(ancestor);
    std::shared_ptr<TwState> replan_state = init_state;
    if (prefix_len > 0) {
        std::vector<TwCall> pre(original_plan.begin(),
                                original_plan.begin() + prefix_len);
        TwSimulateResult ps = tw_simulate(init_state, pre, domain);
        if (ps.state) replan_state = ps.state;
    }

    // Skip the method that was used at the ancestor.
    TwMethodSkip skip;
    {
        TwCall ac;
        ac.name = sol_tree.nodes[ancestor].name;
        ac.args = sol_tree.nodes[ancestor].args;
        skip[tw_call_key(ac)].insert(sol_tree.nodes[ancestor].method_idx);
    }

    // Replan from the ancestor's entry state with the method skip + command blacklist.
    // tw_plan_with_tree builds a fresh tree for the recovered plan.
    TwSolTree new_tree;
    std::optional<std::vector<TwCall>> suffix =
        tw_plan_with_tree(replan_state, original_tasks, domain,
                          new_tree, &r.blacklist, &skip);

    if (!suffix) {
        r.recovered = false;
        return r;
    }

    // Full plan = already-executed prefix + recovered suffix.
    std::vector<TwCall> full_plan(original_plan.begin(),
                                  original_plan.begin() + prefix_len);
    full_plan.insert(full_plan.end(), suffix->begin(), suffix->end());
    r.new_plan  = std::move(full_plan);
    r.recovered = true;
    return r;
}

// Serialise a TwSimulateResult as a JSON object.
inline std::string tw_simulate_to_json(const std::vector<TwCall> &plan,
                                        const TwSimulateResult    &sr,
                                        const std::string         &plan_json) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"plan\": " << plan_json << ",\n";
    o << "  \"completed_steps\": " << sr.completed_steps << ",\n";
    o << "  \"fail_step\": " << sr.fail_step << ",\n";
    o << "  \"success\": " << (sr.fail_step < 0 ? "true" : "false") << "\n";
    o << "}";
    return o.str();
}

// Serialise a TwReplanResult as a JSON object.
// plan_json / new_plan_json: pre-serialised plan arrays from TwLoader::plan_to_json.
inline std::string tw_replan_to_json(int               fail_step,
                                      const TwReplanResult &rr,
                                      const std::string &original_plan_json,
                                      const std::string &new_plan_json) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"original_plan\": " << original_plan_json << ",\n";
    o << "  \"fail_step\": " << fail_step << ",\n";
    o << "  \"completed_steps\": " << rr.simulate.completed_steps << ",\n";
    o << "  \"recovered\": " << (rr.recovered ? "true" : "false") << ",\n";
    o << "  \"new_plan\": " << new_plan_json << "\n";
    o << "}";
    return o.str();
}
