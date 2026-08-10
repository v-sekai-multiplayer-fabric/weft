// Solution derivation tree — records which method was chosen at each compound
// task decomposition so that tw_replan_incremental can backtrack at the exact
// choice point rather than restarting the entire search.
//
// Mirrors IPyHOP's sol_tree (DiGraph of D/T/A/G/M/VG/VM nodes).
#pragma once
#include "tw_domain.hpp"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TwSolNode {
    enum class Kind : uint8_t { Root, Task, Action, Goal, MultiGoal };

    Kind              kind        = Kind::Root;
    int               parent      = -1;
    std::vector<int>  children;

    std::string       name;          // task/action/goal variable name
    std::vector<TwValue> args;

    int  method_idx  = -1;   // Task/Goal/MultiGoal: which alternative succeeded
    int  plan_step   = -1;   // Action: 0-based index in the returned plan
    int  first_step  = -1;   // Task/Goal/MultiGoal: plan_step of leftmost Action descendant
};

struct TwSolTree {
    std::vector<TwSolNode> nodes;
    std::vector<int>       action_nodes;   // node indices in plan order

    // ── Construction helpers ────────────────────────────────────────────────

    // Add a node whose parent is already in the tree.
    int add_node(TwSolNode::Kind k, int parent_id,
                 const std::string &name, std::vector<TwValue> args,
                 int method_idx = -1) {
        int id = (int)nodes.size();
        TwSolNode n;
        n.kind       = k;
        n.parent     = parent_id;
        n.name       = name;
        n.args       = std::move(args);
        n.method_idx = method_idx;
        nodes.push_back(std::move(n));
        if (parent_id >= 0 && parent_id < (int)nodes.size() - 1)
            nodes[parent_id].children.push_back(id);
        return id;
    }

    // Snapshot: record current size so we can roll back failed attempts.
    int checkpoint() const { return (int)nodes.size(); }

    // Rollback to a checkpoint: remove all nodes added since cp and unlink
    // them from their parents (parents are always before cp).
    void restore(int cp) {
        for (int i = cp; i < (int)nodes.size(); ++i) {
            int p = nodes[i].parent;
            if (p >= 0 && p < cp) {
                std::vector<int> &ch = nodes[p].children;
                ch.erase(std::remove(ch.begin(), ch.end(), i), ch.end());
            }
        }
        nodes.resize(cp);
        // Trim action_nodes to those still in the tree.
        while (!action_nodes.empty() && action_nodes.back() >= cp)
            action_nodes.pop_back();
    }

    // ── Query helpers ───────────────────────────────────────────────────────

    // Walk up from node_id; return the first ancestor of kind Task/Goal/MultiGoal
    // that still has at least one more method alternative to try.
    // Returns -1 if no such ancestor exists.
    int nearest_retryable_ancestor(int node_id,
            const TwDomain &domain) const {
        int cur = nodes[node_id].parent;
        while (cur > 0) {
            const TwSolNode &n = nodes[cur];
            if (n.kind == TwSolNode::Kind::Task) {
                auto it = domain.task_methods.find(n.name);
                if (it != domain.task_methods.end() &&
                    n.method_idx + 1 < (int)it->second.size())
                    return cur;
            } else if (n.kind == TwSolNode::Kind::Goal) {
                auto it = domain.task_methods.find(n.name);
                if (it != domain.task_methods.end() &&
                    n.method_idx + 1 < (int)it->second.size())
                    return cur;
            }
            cur = n.parent;
        }
        return -1;
    }

    // Number of plan actions that precede the subtree rooted at node_id.
    // = first_step of the ancestor, or 0 if it has none (root-level).
    int prefix_length(int node_id) const {
        int fs = nodes[node_id].first_step;
        return (fs < 0) ? 0 : fs;
    }
};

// ── Method-level skip map ───────────────────────────────────────────────────
// Maps tw_call_key(task_name, args) → set of method indices to skip.
// Used by incremental replan to avoid retrying the same method choice.
using TwMethodSkip = std::unordered_map<std::string, std::unordered_set<int>>;
