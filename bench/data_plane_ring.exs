# Data-plane ingestion via the shared ring (contract 2 done right): the worker
# overwrites a lock-free :atomics slot, the BEAM samples it. No per-snapshot
# message, no copy. Measures write throughput single-core and aggregated across
# cores (each producer = one zone's worker on its own core).
# Run: mix run bench/data_plane_ring.exs
alias Weft.DataPlane.Ring

entities = 8
coords = List.duplicate(0, entities * 3)
per_producer = 5_000_000

spin = fn
  _spin, _ring, _coords, 0 ->
    :ok

  spin, ring, coords, n ->
    Ring.write(ring, n, coords)
    spin.(spin, ring, coords, n - 1)
end

measure = fn producers ->
  t0 = System.monotonic_time(:microsecond)

  1..producers
  |> Enum.map(fn _ ->
    Task.async(fn -> spin.(spin, Ring.new(entities), coords, per_producer) end)
  end)
  |> Task.await_many(300_000)

  elapsed_s = (System.monotonic_time(:microsecond) - t0) / 1_000_000
  round(producers * per_producer / elapsed_s)
end

# Sampling cost: what the BEAM actually pays at tick rate.
sample_ring = Ring.new(entities)
Ring.write(sample_ring, 1, coords)
{sample_us, _} = :timer.tc(fn -> for _ <- 1..1_000_000, do: Ring.read(sample_ring) end)
IO.puts("ring sample (read): #{Float.round(sample_us / 1_000_000, 3)} µs/read")

IO.puts("\nring write throughput (#{entities} entities/snapshot):")

for producers <- Enum.uniq([1, 2, 4, 8, System.schedulers_online()]) do
  rate = measure.(producers)
  flag = if rate >= 15_000_000, do: "  >= 15M target met", else: ""

  IO.puts(
    "  #{String.pad_leading(to_string(producers), 2)} producer(s): #{rate} snapshots/sec#{flag}"
  )
end
