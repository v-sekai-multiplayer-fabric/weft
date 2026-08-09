defmodule Weft.Gateway.Request do
  @moduledoc """
  A decoded client request to route. Produced by the H3/WebTransport front from a
  stream (reliable) or datagram (unreliable), never from HTTP/1.1.
  """

  @enforce_keys [:target, :op]
  defstruct [:target, :op, args: [], reliable: true]

  @type target :: {:actor, name :: String.t(), key :: String.t()} | {:zone, zone_id :: term()}
  @type t :: %__MODULE__{target: target(), op: atom(), args: [term()], reliable: boolean()}
end

defmodule Weft.Gateway do
  @moduledoc """
  Transport-agnostic routing core: resolve an actor or zone by id via Horde and
  dispatch a request to it.

  The client-facing transport is **HTTP/3 (QUIC) + WebTransport, never HTTP/1.1**.
  A native H3/QUIC front (like rivet's guard) terminates WebTransport sessions,
  decodes each **stream** (reliable control: actor RPC, asset transfer) or
  **datagram** (unreliable real-time signalling) into a `Weft.Gateway.Request`, and
  calls `dispatch/1`. This module is the routing/dispatch half of that boundary;
  the native front feeds it. Datagram-borne requests (`reliable: false`) may only
  carry fire-and-forget signalling ops.
  """

  alias Weft.{Actor, Actors, Zone}
  alias Weft.Gateway.Request

  @spec dispatch(Request.t()) :: {:ok, term()} | {:error, term()}
  def dispatch(%Request{reliable: false, op: op}) when op in [:put, :add_entity] do
    {:error, {:requires_reliable, op}}
  end

  def dispatch(%Request{target: {:actor, name, key}, op: op, args: args}) do
    with {:ok, pid} <- Actors.get_or_create(name, key) do
      apply_actor(pid, op, args)
    end
  end

  def dispatch(%Request{target: {:zone, zone_id}, op: op, args: args}) do
    apply_zone(zone_id, op, args)
  catch
    # No zone registered for this id (unregistered :via call exits with :noproc).
    :exit, _ -> {:error, :no_zone}
  end

  defp apply_actor(pid, :put, [k, v]), do: {:ok, Actor.put(pid, k, v)}
  defp apply_actor(pid, :get, [k]), do: {:ok, Actor.get(pid, k)}
  defp apply_actor(_pid, op, _args), do: {:error, {:unknown_op, op}}

  defp apply_zone(zone_id, :add_entity, [id, data]), do: {:ok, Zone.add_entity(zone_id, id, data)}
  defp apply_zone(zone_id, :entities, []), do: {:ok, Zone.entities(zone_id)}
  defp apply_zone(zone_id, :command, [cmd]), do: {:ok, Zone.command(zone_id, cmd)}
  defp apply_zone(zone_id, :snapshot, []), do: {:ok, Zone.latest(zone_id)}
  defp apply_zone(_zone_id, op, _args), do: {:error, {:unknown_op, op}}
end
