defmodule Weft.Gateway.Request do
  @moduledoc ~S"""
  weft moves entity state in two fixed formats, one per layer. This is not a choice
  made per message. Each layer always uses its one format.

  - **Nasty, on the hot path.** A bitpacked C struct. Per entity update: a 4-byte
    slot id and two 4-byte floats for position, 12 bytes total. Decode is a cast: the
    bytes are already the struct, so applying an update is a slab write with no parse
    and no allocation. This is the format for the data plane, replication, and every
    path that runs at packet rate.
  - **Cheap, at the interop edge.** CBOR-encoded JSON-LD. Self-describing, with named
    fields and a context, so other tools can read it without weft's struct layout.
    This is the format for debugging, inspection, and exchange with outside systems.
    It never runs on the hot path.

  The rule: the hot path is always nasty, the interop edge is always cheap. Latency
  decides the hot path, so it takes the format that decodes by cast. Interoperability
  decides the edge, so it takes the self-describing format, and it is off the hot path
  where its cost does not matter.

  ## Evidence: the SUMO trace

  We measured both formats on a real workload, not synthetic data. SUMO (Eclipse
  traffic microsimulation) ran a 25 by 25 grid city with dense traffic. Each vehicle
  is a weft entity and each simulation step is a state frame. The trace is 600 frames,
  11,947 distinct vehicles, 8,637 peak concurrent, 2,950,620 entity updates.
  Reproduce with `test/bench/sumo/` (see `README` there).

  ### Wire size

  Summed over the whole trace, level 1 zstd, "z-dict" uses the previous frame as the
  dictionary (the last-frame delta):

  | Format               | bytes/entity | raw     | zstd    | z-dict  | z-dict vs nasty |
  | -------------------- | ------------ | ------- | ------- | ------- | --------------- |
  | nasty (bitpacked)    | 12           | 35.4 MB | 25.8 MB | 12.6 MB | 1.0x            |
  | cheap (CBOR JSON-LD) | 28           | 82.6 MB | 27.9 MB | 17.5 MB | 1.4x            |

  Raw, cheap is 2.3 times the bytes. After zstd with the last-frame dictionary the gap
  falls to 1.4 times, because CBOR JSON-LD repeats its field names every frame and
  zstd removes that repetition. So on the wire the size cost of cheap is small. The
  real cost of cheap is not size, it is decode.

  ### Decode and apply speed

  The nasty decode plus apply, measured in C on the real trace (`test/bench/sumo/replay.c`),
  with the entity slab resident in L2:

  | cores | pps    | ns/apply/core |
  | ----- | ------ | ------------- |
  | 1     | 840 M  | 1.19          |
  | 8     | 6.19 B | 1.29          |
  | 16    | 7.78 B | 2.06          |

  840 M applies per second on one core is 56 times the 15 M packets per second target.
  This matches the synthetic `test/bench/pps_native.c` number (826 M per core), so real
  traffic movement confirms the synthetic benchmark. Apply is never the bottleneck.

  Cheap decode is a parse, not a cast, so it is far slower and allocates per field.
  That is acceptable at the interop edge and is the reason cheap never touches the hot
  path. See `../essays/latency.md` for why the hot path stays native and copy-free.
  """

  @enforce_keys [:target, :op]
  defstruct [
    :target,
    :op,
    args: [],
    reliable: true,
    seq: nil,
    from: nil,
    avatar: nil,
    controller: nil,
    epoch: nil
  ]

  @type target :: {:actor, name :: String.t(), key :: String.t()} | {:zone, zone_id :: term()}
  @type t :: %__MODULE__{
          target: target(),
          op: atom(),
          args: [term()],
          reliable: boolean(),
          # The avatar this request drives, and the controller that claims to drive it.
          # `Weft.Authority` holds the rule that one avatar takes one controller. A request
          # that names an avatar is checked against it, and a fenced controller is refused.
          # A request that names no avatar drives no avatar and is not checked.
          avatar: term() | nil,
          controller: term() | nil,
          epoch: non_neg_integer() | nil,
          # App-level sequence for unreliable datagrams: the gateway drops any
          # request whose seq is not newer than the last seen for its target
          # (last-write-wins), so stale/out-of-order packets are discarded.
          seq: non_neg_integer() | nil,
          # The address the request came from. `Weft.Limits` counts the rate and the
          # requests in flight for each address. The native front sets it. A request
          # with no address is a call from inside the node, and it is not counted.
          from: term() | nil
        }
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
  def dispatch(%Request{target: target, op: op} = req) do
    kind = elem(target, 0)

    :telemetry.span([:weft, :gateway, :dispatch], %{target: kind, op: op}, fn ->
      {limited(req), %{}}
    end)
  end

  # The limits of `Weft.Limits`, at the edge where a caller reaches the node. A request
  # with no address comes from inside the node and is not counted, because the limits
  # bound what one caller outside may take.
  defp limited(%Request{from: nil} = req), do: routed(req)

  defp limited(%Request{from: from} = req) do
    with {:ok, _count} <- Weft.Limits.take_request(from),
         {:ok, reply} <- Weft.Limits.with_in_flight(fn -> routed(req) end) do
      reply
    end
  end

  # Unreliable + sequenced: drop the packet unless it is newer than the last one
  # seen for this target.
  defp routed(%Request{reliable: false, seq: seq, target: target} = req) when is_integer(seq) do
    if Weft.Gateway.SeqGuard.fresh?(target, seq) do
      do_dispatch(req)
    else
      {:error, :stale}
    end
  end

  defp routed(%Request{} = req), do: authorised(req)

  # One controller drives one avatar, and the gateway does not decide that.
  #
  # An earlier version called `Weft.Authority.check/3` here, and it was wrong twice. It read
  # FoundationDB on every request, including unreliable datagrams, which is a cross-machine
  # transaction at packet rate against a rule that says to keep durability off the write
  # path. And it did not fence anything: the read returned, the write happened afterwards,
  # and a controller seized from in between passed the check and still wrote.
  #
  # So the epoch travels with the request and `Weft.Zone.drive/4` enforces it, because the
  # zone is the single writer and can compare and write in one step. The gateway only
  # refuses a request that cannot be authoritative at all.
  defp authorised(%Request{avatar: nil} = req), do: do_dispatch(req)

  defp authorised(%Request{avatar: _avatar, controller: controller, epoch: epoch} = req)
       when not is_nil(controller) and is_integer(epoch),
       do: do_dispatch(req)

  # An avatar named without a controller and an epoch cannot be authoritative. The guard
  # names the case rather than leaving a bare catch-all arm: the avatar is present, and the
  # clause above already took every request that carries both a controller and an epoch. A
  # malformed request from the network is refused and it does not crash the node, because
  # the network is not a caller weft trusts.
  defp authorised(%Request{avatar: avatar}) when not is_nil(avatar), do: {:error, :fenced}

  defp do_dispatch(%Request{reliable: false, op: op}) when op in [:put, :add_entity] do
    {:error, {:requires_reliable, op}}
  end

  defp do_dispatch(%Request{target: {:actor, name, key}, op: op, args: args}) do
    with {:ok, pid} <- Actors.get_or_create(name, key) do
      apply_actor(pid, op, args)
    end
  end

  defp do_dispatch(%Request{target: {:zone, zone_id}, op: op, args: args}) do
    case resolve_zone(zone_id) do
      :ok -> apply_zone(zone_id, op, args)
      :none -> {:error, :no_zone}
    end
  end

  # Resolve a zone by lookup rather than by catching an exit: no exceptions for the
  # expected "no such zone" case.
  #
  # A miss is retried once, and the retry is not a guess. `Horde.Registry.register`
  # returns before `lookup` can see the name: `Horde.RegistryImpl.on_diffs/2` sends
  # `{:crdt_update, diffs}` to the registry process, and that process materialises the
  # name into ETS when it handles the message. So the name is behind one mailbox.
  #
  # `../../docs/logbook/control_plane.md` measures it. At 32 registrations at once, 617 of
  # 640 lookups straight after a register found nothing. After one synchronous call to the
  # registry, 0 of 640 did.
  #
  # That call is the retry. It is a synchronisation point and not a timeout, so there is no
  # interval to pick and no sleep. `:sys.get_state/1` returns after every earlier message
  # is handled, and the state it copies is four table references and two node sets, so it
  # carries no entries.
  #
  # This is rivet's shape, from `GUARD.md`: the fast path reads the cache, a failure means
  # the cached view is stale, and the retry ignores the cache. weft's cache is the ETS
  # table Horde materialises into.
  defp resolve_zone(zone_id) do
    case Horde.Registry.lookup(Weft.Registry, {:zone, zone_id}) do
      [{_pid, _}] ->
        :ok

      [] ->
        _ = :sys.get_state(Weft.Registry)

        case Horde.Registry.lookup(Weft.Registry, {:zone, zone_id}) do
          [{_pid, _}] -> :ok
          [] -> :none
        end
    end
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
