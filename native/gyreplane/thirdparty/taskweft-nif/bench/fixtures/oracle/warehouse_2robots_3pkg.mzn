% MiniZinc "teacher" oracle for
% bench/fixtures/domains/warehouse_domain_2robots.jsonld +
% bench/fixtures/problems/problem_three_deliveries_2robots.jsonld.
%
% This is a closed-form model of the domain's real, verified cost
% semantics, not a generic planner encoding -- the domain's own action
% durations and control flow were traced by hand against real
% Taskweft.plan/1 output (see the PR this file ships with) and confirmed
% exactly:
%
%   - The plan Taskweft.plan/1 returns is a single sequential action
%     trace. There is no concurrency in its temporal model at all, so
%     total time is the plain sum of each delivery's own cost, in
%     whatever order the planner happens to interleave them -- order
%     itself never changes the total, only which agent does which
%     package does.
%   - Every delivery costs exactly 20 minutes (move+recharge+move+
%     pickup+move+recharge+move+dropoff = 2+5+2+1+2+5+2+1), EXCEPT an
%     agent's own very first delivery when that agent starts the whole
%     plan already at "dock": ensure_charged's own go_dock leg is then a
%     genuine no-op (0 move cost instead of 2), saving exactly 2
%     minutes, once, for that one agent only.
%
% n_packages agents, each package assigned to exactly one of two agents.
% agent_starts_at_dock[a] is true iff that agent's own initial location
% (from the problem file) is "dock" -- the only location that earns the
% one-time discount.

int: n_packages;
array[1..2] of bool: agent_starts_at_dock;

array[1..n_packages] of var 1..2: assign;

% Whether agent a gets its one-time dock discount: it must start at
% dock AND actually be assigned at least one package (an agent that
% delivers nothing never reaches ensure_charged at all).
array[1..2] of var bool: agent_used =
  [ exists(p in 1..n_packages)(assign[p] = a) | a in 1..2 ];
array[1..2] of var bool: agent_discounted =
  [ agent_starts_at_dock[a] /\ agent_used[a] | a in 1..2 ];

var int: total_minutes =
  20 * n_packages - sum(a in 1..2)(bool2int(agent_discounted[a]) * 2);

solve minimize total_minutes;

output ["assign = \(assign);\n",
        "total_minutes = \(total_minutes);\n"];
