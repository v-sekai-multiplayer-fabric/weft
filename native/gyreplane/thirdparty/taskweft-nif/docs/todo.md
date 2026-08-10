# ============================================================================
# TODO: Order-of-Magnitude Planner Speedup Implementation
# ============================================================================
#
# The following tasks implement the SAT-free HTN planning optimizations
# documented in order_of_magnitude_speedup.cff, ranked by expected speedup.
#
# ----------------------------------------------------------------------------
# PHASE 1: Predictive Pruning (10x+) — HIGH PRIORITY
# ----------------------------------------------------------------------------
#
# TASK 1: Plausible Witness DAG
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: HIGH (aggressive dead-end pruning)
#   Impact: Order of magnitude (10x+)
#   Rank: #1
#
#   Files to modify:
#   - lib/tw_planner_wip.c (add witness oracle call)
#   - lean/Planner/WitnessDAG.lean (new file - Lean proofs)
#   - include/tw_planner.h (witness oracle function pointer)
#
#   Steps:
#   1. Port witness search from https://github.com/fire/plausible-witness-dag.git
#   2. Add `witness_oracle : fn(State, Tasks) -> Bool` to planner config
#   3. Call oracle before action expansion:
#      - If oracle returns false: skip action entirely
#      - If oracle returns true: proceed with expansion
#   4. Implement iterative deepening: increase depth bound on failure
#   5. Prove oracle soundness: never prunes real solution
#   6. Prove oracle completeness: only prunes dead-ends
#   7. Add benchmarks comparing with/without oracle
#
# ----------------------------------------------------------------------------
# PHASE 2: Failure Analysis & Cache (2x-100x) — HIGH IMPACT
# ----------------------------------------------------------------------------
#
# TASK 2: Failure Cache
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: HIGH
#   Impact: 2x-100x depending on problem structure
#   Rank: #2
#
#   Files to modify:
#   - lean/Planner/FailCache.lean (Lean proofs - already done)
#   - lib/tw_planner_state.lean (add failure_cache field)
#   - c_src/tw_planner_wip.c (cache operations)
#
#   Steps:
#   1. Add `failure_cache : HashSet Signature` to planner state
#   2. On expansion failure: add (state_hash, tasks_hash) to cache
#   3. Before expansion: check cache; skip if key present
#   4. Use the existing FailCache.lean proofs for correctness
#   5. Add benchmarks
#
# ----------------------------------------------------------------------------
# PHASE 3: Foundational Optimizations (4x-74x standalone)
# ----------------------------------------------------------------------------
#
# TASK 3: Expand IS_MEMBER_OF Index
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: MEDIUM (foundation for all ReBAC lookups)
#   Impact: 4x-74x speedup on capability graph expansion
#   Rank: #3
#
#   Files to modify:
#   - lib/tw_rebac_graph.lean (NIF module)
#   - lean/Planner/ExpandIndex.lean (Lean proofs)
#   - lean/Planner/Capabilities.lean (use indexed edges)
#
#   Steps:
#   1. Add `member_edges : Array Int` field to TwReBACGraph record
#   2. In add_edge NIF function:
#      - If edge is IS_MEMBER_OF: append edge index to member_edges
#      - Otherwise: don't modify member_edges
#   3. Create new expand/3 function that uses member_edges instead of linear scan
#   4. Update expand/1 to call the new indexed version
#   5. Prove IndexSound and IndexComplete in ExpandIndex.lean
#   6. Prove indexed_edges == linear scan result
#
# TASK 4: Fuel-Bounded ReBAC Capabilities
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: MEDIUM (enables safe caching)
#   Impact: Prevents blowup; enables cache-friendly capability checks
#   Rank: #6 (enabler — do after expand_index)
#
#   Files to modify:
#   - lean/Planner/Capabilities.lean
#   - c_src/tw_planner_wip.c (NIF bindings)
#
#   Steps:
#   1. Add fuel parameter to hasCapability function (default: 3)
#   2. Modify recursive IS_MEMBER_OF traversal to decrement fuel
#   3. Add fuel to hasCapabilityString lookup table
#   4. Add fuel to ReBAC checkRelationExpr
#   5. Prove fuel-decreasing terminates
#   6. Prove expand still sound at fuel+1
#
# ----------------------------------------------------------------------------
# PHASE 4: Branching Factor Reduction (3x-20x)
# ----------------------------------------------------------------------------
#
# TASK 5: Entity Capabilities Guard
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: MEDIUM (reduces action branching factor)
#   Impact: 3x-20x depending on domain action count
#   Rank: #4
#
#   Files to modify:
#   - lean/Planner/Capabilities.lean (add guard logic)
#   - lib/tw_domain.lean (add capability requirements to actions)
#   - c_src/tw_planner_wip.c (NIF guard check)
#
#   Steps:
#   1. Add `required_capabilities : Dict String (List String)` to domain definition
#   2. Before expanding each action:
#      - Check agent has required capabilities
#      - Skip action if missing
#   3. Prove guard soundness: blocked actions are infeasible
#   4. Prove guard completeness: unblocked actions are feasible
#
# TASK 6: Temporal Plan Reuse
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: MEDIUM (plan-level caching)
#   Impact: 5x-50x for incremental state changes
#   Rank: #5
#
#   Files to modify:
#   - lean/Planner/Temporal.lean (add cache)
#   - lib/tw_planner_state.lean (add plan cache field)
#   - c_src/tw_planner_wip.c (cache lookup on replan)
#
#   Steps:
#   1. Add `plan_cache : HashMap Signature Plan` to planner state
#   2. Define Signature = hash(current_state, pending_tasks, capabilities)
#   3. On replan:
#      - Check cache for matching signature
#      - Return cached plan if found
#      - Otherwise run planner and cache result
#   4. Prove cache hit returns valid plan for current state
#   5. Prove cache miss triggers full search
#
# ----------------------------------------------------------------------------
# PHASE 5: Heuristic Guidance
# ----------------------------------------------------------------------------
#
# TASK 7: Landmark-Based Pruning
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: LOW (heuristic guidance)
#   Impact: 2x-10x with good landmark heuristic
#   Rank: #7
#
#   Files to modify:
#   - lib/tw_planner_wip.c (landmark checking)
#   - lean/Planner/Landmarks.lean (new file - Lean proofs)
#   - include/tw_planner.h (landmark set in state)
#
#   Steps:
#   1. Implement landmark generation (AND/OR graph based)
#   2. Add landmark checking before node expansion
#   3. If current path can't reach next landmark: backtrack immediately
#   4. Prove landmark soundness: pruned paths truly infeasible
#   5. Prove landmark completeness: all solutions pass landmark check
#
# ----------------------------------------------------------------------------
# BUILD & VERIFY
# ----------------------------------------------------------------------------
#
# TASK 8: Verify Lean Build
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: HIGH
#
#   Steps:
#   1. Run `lake build` in taskweft_repo/
#   2. Fix any compilation errors
#   3. Run `lake test` to verify all proofs pass
#   4. Run benchmarks: `mix bench` in nif/
#
# TASK 9: Documentation
# ────────────────────────────────────────────────────────────────────────────
#
#   Status: PENDING
#   Priority: MEDIUM
#
#   Steps:
#   1. Add implementation notes to order_of_magnitude_speedup.cff
#   2. Update README.md with speedup architecture decisions
#   3. Add benchmarks to ExpandIndex.lean (already done)
#
# ============================================================================
# IMPLEMENTATION ORDER SUMMARY
# ============================================================================
#
# Ranked by expected speedup:
#
# 1. Plausible Witness DAG      — 10x+ (predictive pruning)
# 2. Failure Cache                — 2x-100x (cache dead-ends)
# 3. Expand IS_MEMBER_OF Index   — 4x-74x (capability graph)
# 4. Entity Capabilities Guard    — 3x-20x (branch reduction)
# 5. Temporal Plan Reuse         — 5x-50x (plan caching)
# 6. Fuel-Bounded ReBAC           — enabler (do after #3)
# 7. Landmark Pruning             — 2x-10x (heuristic)
# 8. Verify Lean Build            — compile + test
# 9. Documentation                — update docs
#
# Expected cumulative speedup: 10x-10000x depending on problem domain.
# Start with tasks 1-2 for maximum impact.
