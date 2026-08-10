// Native explain payloads for Taskweft planning outcomes.
// Builds JSON trees for both solved plans and no_plan outcomes.
#pragma once

#include "tw_domain.hpp"
#include "tw_json.hpp"
#include "tw_soltree.hpp"

#include <string>
#include <vector>

inline std::string tw_node_kind_name(TwSolNode::Kind kind) {
    switch (kind) {
        case TwSolNode::Kind::Root:
            return "root";
        case TwSolNode::Kind::Task:
            return "task";
        case TwSolNode::Kind::Action:
            return "action";
        case TwSolNode::Kind::Goal:
            return "goal";
        case TwSolNode::Kind::MultiGoal:
            return "multigoal";
    }
    return "unknown";
}

inline TwValue tw_call_to_value(const TwCall &call) {
    TwValue::Array out;
    out.reserve(call.args.size() + 1);
    out.emplace_back(call.name);
    for (const TwValue &arg : call.args) out.push_back(arg);
    return TwValue(std::move(out));
}

inline TwValue tw_soltree_node_to_value(const TwSolNode &node, int id) {
    TwValue::Dict d;
    d["id"] = TwValue((int64_t)id);
    d["kind"] = TwValue(tw_node_kind_name(node.kind));
    d["parent"] = TwValue((int64_t)node.parent);
    if (!node.name.empty()) d["name"] = TwValue(node.name);
    if (!node.args.empty()) d["args"] = TwValue(node.args);
    if (node.method_idx >= 0) d["method_idx"] = TwValue((int64_t)node.method_idx);
    if (node.plan_step >= 0) d["plan_step"] = TwValue((int64_t)node.plan_step);
    if (node.first_step >= 0) d["first_step"] = TwValue((int64_t)node.first_step);

    TwValue::Array children;
    children.reserve(node.children.size());
    for (int child : node.children) children.emplace_back((int64_t)child);
    d["children"] = TwValue(std::move(children));

    return TwValue(std::move(d));
}

inline TwValue tw_solution_tree_value(const TwSolTree &tree, const std::vector<TwCall> &plan) {
    TwValue::Dict explain;
    explain["mode"] = TwValue("native");
    explain["status"] = TwValue("ok");

    TwValue::Array plan_steps;
    plan_steps.reserve(plan.size());
    for (const TwCall &call : plan) plan_steps.push_back(tw_call_to_value(call));
    explain["plan_steps"] = TwValue(std::move(plan_steps));

    TwValue::Array nodes;
    nodes.reserve(tree.nodes.size());
    for (int i = 0; i < (int)tree.nodes.size(); ++i) {
        nodes.push_back(tw_soltree_node_to_value(tree.nodes[(size_t)i], i));
    }
    explain["solution_tree"] = TwValue(std::move(nodes));

    TwValue::Array action_nodes;
    action_nodes.reserve(tree.action_nodes.size());
    for (int idx : tree.action_nodes) action_nodes.emplace_back((int64_t)idx);
    explain["action_nodes"] = TwValue(std::move(action_nodes));

    return TwValue(std::move(explain));
}

inline TwValue tw_failure_task_value(const TwTask &task, const TwDomain &domain, int index) {
    TwValue::Dict d;
    d["index"] = TwValue((int64_t)index);

    if (const TwCall *call = std::get_if<TwCall>(&task)) {
        d["kind"] = TwValue("task_call");
        d["name"] = TwValue(call->name);
        d["args"] = TwValue(call->args);

        bool resolvable = domain.has_action(call->name) || domain.has_task(call->name);
        d["resolvable"] = TwValue(resolvable);

        if (domain.has_action(call->name)) d["symbol_type"] = TwValue("action");
        else if (domain.has_task(call->name)) d["symbol_type"] = TwValue("method");
        else d["symbol_type"] = TwValue("unknown");

        return TwValue(std::move(d));
    }

    if (const TwGoal *goal = std::get_if<TwGoal>(&task)) {
        d["kind"] = TwValue("goal");
        TwValue::Array bindings;
        for (const TwGoalBinding &b : goal->bindings) {
            TwValue::Dict bd;
            bd["var"] = TwValue(b.var);
            bd["key"] = TwValue(b.key);
            bd["desired"] = b.desired;
            bindings.emplace_back(std::move(bd));
        }
        d["bindings"] = TwValue(std::move(bindings));
        return TwValue(std::move(d));
    }

    const TwMultiGoal *mg = std::get_if<TwMultiGoal>(&task);
    d["kind"] = TwValue("multigoal");
    TwValue::Array bindings;
    for (const TwGoalBinding &b : mg->bindings) {
        TwValue::Dict bd;
        bd["var"] = TwValue(b.var);
        bd["key"] = TwValue(b.key);
        bd["desired"] = b.desired;
        bindings.emplace_back(std::move(bd));
    }
    d["bindings"] = TwValue(std::move(bindings));
    return TwValue(std::move(d));
}

inline std::string tw_no_plan_explain_json(const std::vector<TwTask> &tasks, const TwDomain &domain) {
    TwValue::Dict root;
    root["status"] = TwValue("no_plan");

    TwValue::Dict explain;
    explain["mode"] = TwValue("native");
    explain["status"] = TwValue("no_plan");
    explain["summary"] = TwValue("planner returned no_plan");

    TwValue::Array task_nodes;
    task_nodes.reserve(tasks.size());
    for (int i = 0; i < (int)tasks.size(); ++i) {
        task_nodes.push_back(tw_failure_task_value(tasks[(size_t)i], domain, i));
    }
    explain["failure_tree"] = TwValue(std::move(task_nodes));

    root["explain"] = TwValue(std::move(explain));
    return TwJson::to_json(TwValue(std::move(root)));
}
