defmodule Taskweft do
  @moduledoc """
  Elixir bindings for the Taskweft C++20 HTN planner NIF.

  Provides planning, replanning, temporal consistency checking,
  HRR (Holographic Reduced Representation) encoding for semantic memory,
  and ReBAC (relationship-based access control) checks.

  Used by `multiplayer-fabric-artifacts-mmog` and
  `multiplayer-fabric-zone-console`. Breaking API changes require
  coordinated updates in both consumers.

  ## Domain JSON format

  All JSON strings use the taskweft JSON-LD domain format. Domains are
  JSON-LD documents; the `"op"` field syntax (`"add"`, `"get"`) must match
  what `domain.ex` emits — do not revert to the old `"type": "math/add"`
  form. See `standalone/tw_domain.hpp` and `priv/plans/` for examples.

  ## Nested transactions via savepoints

  SQLite does not support `BEGIN` inside an open transaction. The adapter
  uses `txn_depth` in GenServer state to track nesting:

  - Depth 0 → 1: `BEGIN`
  - Depth N → N+1: `SAVEPOINT sp{N}`
  - Depth 1 → 0 commit: `COMMIT`, then rebuild dirty bundles
  - Depth N → N-1 commit: `RELEASE sp{N-1}`
  - Depth 1 → 0 rollback: `ROLLBACK`
  - Depth N → N-1 rollback: `ROLLBACK TO sp{N-1}`

  `Ecto.Repo.transaction/2` calls nest: an inner `Repo.transaction`
  inside an outer one promotes to a savepoint.

  ## Deferred bundle rebuild inside transactions

  `hrr_bundles` holds the superposition of all `record_vector`s for a
  source. Rebuilding on every insert/delete inside a transaction is wrong;
  partial-commit state would corrupt the bundle:

  - Inserts and deletes inside a transaction mark the source in a `dirty`
    `MapSet` but skip `rebuild_bundle`.
  - At outermost `COMMIT`, the dirty set is drained and each source is
    rebuilt after the SQLite `COMMIT` returns.
  - Nested savepoint `ROLLBACK TO` does not clear the dirty set — the
    outer transaction may still commit.

  ## Ecto adapter

  All four adapter behaviours (`Ecto.Adapter`, `Ecto.Adapter.Schema`,
  `Ecto.Adapter.Queryable`, `Ecto.Adapter.Transaction`) are guarded by
  `Code.ensure_loaded?` at compile time. Projects without Ecto pay no
  compile-time cost and the NIF tests still pass.
  """

  alias Taskweft.NIF

  def plan(domain_json, origin_iso \\ "PT0S", reference_date \\ "") do
    if reference_date == "" do
      {:ok, NIF.plan_with_temporal(domain_json, origin_iso)}
    else
      {:ok, NIF.plan_with_temporal_civil(domain_json, origin_iso, reference_date)}
    end
  rescue
    e -> {:error, Exception.message(e)}
  end

  def plan_explain(domain_json, origin_iso \\ "PT0S", reference_date \\ "") do
    if reference_date == "" do
      {:ok, NIF.plan_with_temporal_explain(domain_json, origin_iso)}
    else
      {:ok, NIF.plan_with_temporal_civil_explain(domain_json, origin_iso, reference_date)}
    end
  rescue
    e -> {:error, Exception.message(e)}
  end

  def replan(domain_json, plan_json, fail_step \\ -1) do
    {:ok, NIF.replan(domain_json, plan_json, fail_step)}
  rescue
    e -> {:error, Exception.message(e)}
  end

  def check_temporal(domain_json, plan_json, origin_iso \\ "PT0S", reference_date \\ "") do
    if reference_date == "" do
      {:ok, NIF.check_temporal(domain_json, plan_json, origin_iso)}
    else
      {:ok, NIF.check_temporal_civil(domain_json, plan_json, origin_iso, reference_date)}
    end
  rescue
    e -> {:error, Exception.message(e)}
  end

  def rebac_check(graph_json, subj, expr_json, obj, fuel \\ 8) do
    NIF.rebac_check(graph_json, subj, expr_json, obj, fuel)
  end

  def rebac_expand(graph_json, rel, obj, fuel \\ 8) do
    NIF.rebac_expand(graph_json, rel, obj, fuel)
  end

  def bridge_extract_entities(state_json) do
    NIF.bridge_extract_entities(state_json)
  end

  def bridge_plan_contents(plan_json, domain, entities_json) do
    NIF.bridge_plan_contents(plan_json, domain, entities_json)
  end
end
