defmodule Weft.Entities do
  @moduledoc """
  Durable entity ownership backed by FoundationDB, with **atomic handoff**.

  Each entity has one owning zone. `handoff/3` moves ownership in a single FDB
  transaction (read the current owner, verify it, write the new owner, and fix the
  per-zone index), so a crash can never leave an entity lost or owned by two zones
  at once. This is the entity-owner-pointer model rivet uses, and it hardens the
  best-effort take-then-put handoff in `Weft.Zone`.

  Layout (tuple layer):

    * `("weft", "entity", entity_id)` -> term `{owner_zone, data}`
    * `("weft", "zone_entities", zone_id, entity_id)` -> "" (index for `by_zone/1`)
  """

  @prefix "weft"
  @entity "entity"
  @zone_index "zone_entities"

  @doc "Set (or replace) an entity's owning zone and data."
  @spec put(term(), term(), term()) :: :ok
  def put(entity_id, zone_id, data) do
    :erlfdb.transactional(db(), fn tx -> write_owner(tx, entity_id, zone_id, data) end)
    :ok
  end

  @doc "Fetch an entity's owning zone and data."
  @spec get(term()) :: {:ok, zone_id :: term(), data :: term()} | :not_found
  def get(entity_id) do
    :erlfdb.transactional(db(), fn tx ->
      case fetch(tx, entity_id) do
        :not_found -> :not_found
        {zone_id, data} -> {:ok, zone_id, data}
      end
    end)
  end

  @doc """
  Atomically move `entity_id` from `from_zone` to `to_zone`. Refuses (without
  writing) if the entity is missing or not currently owned by `from_zone`, so
  concurrent or duplicate handoffs cannot both win.
  """
  @spec handoff(term(), term(), term()) ::
          :ok | {:error, :not_found} | {:error, {:owned_by, term()}}
  def handoff(entity_id, from_zone, to_zone) do
    :erlfdb.transactional(db(), fn tx ->
      case fetch(tx, entity_id) do
        :not_found ->
          {:error, :not_found}

        {^from_zone, data} ->
          :erlfdb.clear(tx, zone_index_key(from_zone, entity_id))
          write_owner(tx, entity_id, to_zone, data)
          :ok

        {other_zone, _data} ->
          {:error, {:owned_by, other_zone}}
      end
    end)
  end

  @doc "List the entity ids a zone currently owns."
  @spec by_zone(term()) :: [term()]
  def by_zone(zone_id) do
    prefix = :erlfdb_tuple.pack({@prefix, @zone_index, zone_id})

    db()
    |> :erlfdb.get_range_startswith(prefix)
    |> Enum.map(fn {packed_key, _} ->
      {@prefix, @zone_index, ^zone_id, entity_id} = :erlfdb_tuple.unpack(packed_key)
      entity_id
    end)
  end

  defp write_owner(tx, entity_id, zone_id, data) do
    :erlfdb.set(tx, entity_key(entity_id), :erlang.term_to_binary({zone_id, data}))
    :erlfdb.set(tx, zone_index_key(zone_id, entity_id), <<>>)
  end

  defp fetch(tx, entity_id) do
    case :erlfdb.wait(:erlfdb.get(tx, entity_key(entity_id))) do
      :not_found -> :not_found
      bin -> :erlang.binary_to_term(bin)
    end
  end

  defp entity_key(id), do: :erlfdb_tuple.pack({@prefix, @entity, id})
  defp zone_index_key(zone, id), do: :erlfdb_tuple.pack({@prefix, @zone_index, zone, id})

  defp db do
    case :persistent_term.get({__MODULE__, :db}, nil) do
      nil ->
        db = :erlfdb.open(cluster_file())
        :persistent_term.put({__MODULE__, :db}, db)
        db

      db ->
        db
    end
  end

  defp cluster_file do
    case Application.get_env(:weft, :fdb_cluster_file) do
      path when is_binary(path) -> path
      nil -> raise "Weft.Entities requires config :weft, :fdb_cluster_file"
    end
  end
end
