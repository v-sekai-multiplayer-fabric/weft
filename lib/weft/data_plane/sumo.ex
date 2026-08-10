defmodule Weft.DataPlane.Sumo do
  @moduledoc """
  SUMO game-data-plane producer: it plays back a traffic simulation into a zone's
  ring, one frame per tick. Each vehicle is an entity, each simulation step a frame.
  The BEAM samples the ring at its own tick rate (contract 2 in `Weft.DataPlane`);
  nothing is copied per snapshot.

  This is the Elixir producer for the ring. The production plane is a native process
  writing the same ring through a NIF over iceoryx2, faster than the BEAM,
  so the raw packet flood never enters the VM. Here the frames come from a decoded
  SUMO trace (see `test/bench/sumo/extract_frames.py`) rather than a live socket.
  """

  use GenServer

  alias Weft.DataPlane.Ring

  @magic 0x53554D4F

  # ── Frame decoding ──────────────────────────────────────────────────────────

  @doc """
  Decode the compact SUMO frame binary (`test/bench/sumo/extract_frames.py` format) into a
  list of frames, each a list of `{slot, x, y}` with float metre coordinates.
  """
  @spec decode_frames(binary()) :: {:ok, [[{non_neg_integer(), float(), float()}]]} | :error
  def decode_frames(
        <<@magic::little-32, _max_slots::little-32, nframes::little-32, rest::binary>>
      ) do
    decode_frames(rest, nframes, [])
  end

  def decode_frames(_), do: :error

  defp decode_frames(_bin, 0, acc), do: {:ok, Enum.reverse(acc)}

  defp decode_frames(<<count::little-32, rest::binary>>, n, acc) do
    {frame, rest2} = decode_entities(rest, count, [])
    decode_frames(rest2, n - 1, [frame | acc])
  end

  defp decode_entities(bin, 0, acc), do: {Enum.reverse(acc), bin}

  defp decode_entities(
         <<slot::little-32, x::little-float-32, y::little-float-32, rest::binary>>,
         n,
         acc
       ) do
    decode_entities(rest, n - 1, [{slot, x, y} | acc])
  end

  @doc """
  Flatten a frame into the ring's coordinate layout: the first `max_entities`
  entities as fixed-point millimetres `[x, y, z, ...]`, z zero, padded with zeros.
  """
  @spec frame_to_coords([{non_neg_integer(), float(), float()}], pos_integer()) :: [integer()]
  def frame_to_coords(frame, max_entities) do
    taken = Enum.take(frame, max_entities)
    coords = Enum.flat_map(taken, fn {_slot, x, y} -> [mm(x), mm(y), 0] end)
    coords ++ List.duplicate(0, (max_entities - length(taken)) * 3)
  end

  defp mm(v), do: round(v * 1000)

  # ── Producer ────────────────────────────────────────────────────────────────

  @doc """
  Start playing `frames` into `ring` for `zone_id`. With `interval_ms > 0` the plane
  self-schedules ticks (event-driven, not a busy loop) and loops the trace. With
  `interval_ms: 0` it is driven manually with `step/1`, for deterministic tests.
  """
  def start_link(zone_id, %Ring{} = ring, frames, opts \\ []) when is_list(frames) do
    GenServer.start_link(__MODULE__, {zone_id, ring, frames, opts})
  end

  @doc "Write the current frame to the ring and advance. Returns the frame index written."
  def step(pid), do: GenServer.call(pid, :step)

  def stop(pid), do: GenServer.stop(pid, :normal)

  @impl true
  def init({zone_id, ring, frames, opts}) do
    interval = Keyword.get(opts, :interval_ms, 0)

    state = %{
      zone_id: zone_id,
      ring: ring,
      frames: List.to_tuple(frames),
      count: length(frames),
      index: 0,
      interval: interval
    }

    _ = if interval > 0 and state.count > 0, do: Process.send_after(self(), :tick, interval)
    {:ok, state}
  end

  @impl true
  def handle_call(:step, _from, state) do
    {index, state} = tick(state)
    {:reply, index, state}
  end

  @impl true
  def handle_info(:tick, state) do
    {_index, state} = tick(state)
    Process.send_after(self(), :tick, state.interval)
    {:noreply, state}
  end

  # Write the current frame to the ring, emit telemetry, advance with wraparound.
  defp tick(%{count: 0} = state), do: {0, state}

  defp tick(state) do
    frame = elem(state.frames, state.index)
    coords = frame_to_coords(frame, state.ring.max_entities)
    Ring.write(state.ring, state.index, coords)

    :telemetry.execute(
      [:weft, :sumo, :tick],
      %{entities: length(frame)},
      %{zone_id: state.zone_id, frame: state.index}
    )

    {state.index, %{state | index: rem(state.index + 1, state.count)}}
  end
end
