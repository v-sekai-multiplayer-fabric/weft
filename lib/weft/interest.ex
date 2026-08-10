defmodule Weft.Interest do
  @moduledoc """
  Interest feed logic: it produces the read-only area-of-interest replica for an
  observer, the `CH_INTEREST` snapshot in `docs/reference/architecture.md`. Interest is separate from
  authority: a peer sees an entity without owning it (the authority/interest split,
  formalized in `lean-interest-mgmt`).

  This is the Elixir prototype of the interest feed plane. The production plane runs
  native over iceoryx2 and reads the game data plane's output on its own cores, so
  headset fanout never steals cycles from the authority simulation. Selection here is
  a sphere around the observer. The lean model uses a k-tick kinematic expansion, a
  later refinement.

  Coordinates are fixed-point millimetres, the ring layout in `Weft.DataPlane.Ring`.
  """

  alias Weft.DataPlane.Ring

  @type entity :: {index :: non_neg_integer(), x :: integer(), y :: integer(), z :: integer()}
  @type observer :: %{center: {integer(), integer(), integer()}, radius: integer()}

  @doc "True when the entity is inside the observer's area-of-interest sphere."
  @spec in_range?(entity(), {integer(), integer(), integer()}, non_neg_integer()) :: boolean()
  def in_range?({_i, x, y, z}, {cx, cy, cz}, radius) do
    dx = x - cx
    dy = y - cy
    dz = z - cz
    dx * dx + dy * dy + dz * dz <= radius * radius
  end

  @doc "Select the entities inside the observer's area of interest (the replica set)."
  @spec select([entity()], {integer(), integer(), integer()}, non_neg_integer()) :: [entity()]
  def select(entities, center, radius) do
    Enum.filter(entities, &in_range?(&1, center, radius))
  end

  @doc """
  Read the latest ring snapshot and return the area-of-interest replica for an
  observer as `{tick, entities}`. The BEAM never owns the entities, it only forwards
  this read-only replica to the client.
  """
  @spec from_ring(Ring.t(), observer()) :: {non_neg_integer(), [entity()]}
  def from_ring(%Ring{} = ring, %{center: center, radius: radius}) do
    {tick, coords} = Ring.read(ring)
    {tick, select(to_entities(coords, 0, []), center, radius)}
  end

  defp to_entities([], _i, acc), do: :lists.reverse(acc)

  defp to_entities([x, y, z | rest], i, acc),
    do: to_entities(rest, i + 1, [{i, x, y, z} | acc])
end
