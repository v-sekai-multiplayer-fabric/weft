# Data-plane boundary throughput probe (contract 2): the BEAM-side ceiling for
# ingesting digested snapshots from a data-plane worker. We bypass the stub's
# timer and flood the zone directly, then measure how fast it drains, so the number
# is the control plane's capacity, not the worker's synthetic cadence.
# Run: mix run bench/data_plane.exs
alias Weft.Zone
alias Weft.DataPlane.Snapshot

zone_id = "bench-#{System.unique_integer([:positive])}"
# Idle worker (long tick) so only our flood drives the zone.
{:ok, zone} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 3_600_000])

entities = for i <- 1..8, do: %{id: i, x: i * 1.0, y: 0.0, z: 0.0}
base = %Snapshot{zone_id: zone_id, tick: 0, entities: entities, generated_at: 0}

n = 1_000_000

drain = fn drain ->
  {:message_queue_len, len} = Process.info(zone, :message_queue_len)

  if len > 0,
    do:
      (
        Process.sleep(1)
        drain.(drain)
      ),
    else: :ok
end

t0 = System.monotonic_time(:microsecond)
for i <- 1..n, do: send(zone, {:dp_snapshot, zone_id, %{base | tick: i}})
drain.(drain)
t1 = System.monotonic_time(:microsecond)

elapsed_s = (t1 - t0) / 1_000_000
per_sec = round(n / elapsed_s)

IO.puts("""

data-plane -> BEAM ingestion ceiling (8 entities/snapshot, single zone)
  snapshots:         #{n}
  elapsed:           #{Float.round(elapsed_s, 3)} s
  snapshots/sec:     #{per_sec}
""")

GenServer.stop(zone)
