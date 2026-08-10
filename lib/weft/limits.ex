defmodule Weft.Limits.Rate do
  @moduledoc """
  The rate limiter behind `Weft.Limits.take_request/1`, across the cluster.

  Hammer owns the window, the table, and the cleanup. weft owns the number, and it owns
  the part that makes the count cluster-wide.

  A count held on one node is not a limit. A client reaches an edge, and a cluster has
  more than one edge, so a client that spreads its requests over the edges gets the limit
  times the number of edges. weft counts on every node instead.

  The method is the one in Hammer's guide for a distributed ETS backend. Each node counts
  what it sees, and it tells the other nodes, which add the same count to their own
  window. The guide uses `Phoenix.PubSub`. weft has no Phoenix, so it uses `:pg`, which
  is in OTP and already knows which nodes are in the cluster.

  ## What this gives, and what it does not

  The count is eventually consistent, and that is a real cost, not a detail.

  - A burst that lands on two nodes at once can pass the limit, because neither node has
    heard from the other yet. The window is a minute and a message crosses a cluster in
    less than a millisecond, so the overshoot is small.
  - A node that joins starts with an empty window. It undercounts until the next window.
  - A network split leaves each side counting alone. Each side then allows the whole
    limit, so a split doubles what a client can send.

  Every one of these fails open, which is the correct direction for a rate limit. A limit
  that fails closed would drop good traffic on a node that has just started.
  """

  @group __MODULE__

  defmodule Local do
    @moduledoc false
    use Hammer, backend: :ets
  end

  defmodule Listener do
    @moduledoc false
    use GenServer

    @doc false
    def start_link(opts), do: GenServer.start_link(__MODULE__, opts, name: __MODULE__)

    @impl true
    def init(_opts) do
      :ok = :pg.join(Weft.Limits.Rate, self())
      {:ok, []}
    end

    # A count from another node. It goes into this window, and it is not sent on, so a
    # message cannot go round the cluster twice.
    @impl true
    def handle_info({:inc, key, scale, increment}, state) do
      _count = Weft.Limits.Rate.Local.inc(key, scale, increment)
      {:noreply, state}
    end

    def handle_info(_other, state), do: {:noreply, state}
  end

  @doc false
  def child_spec(opts) do
    %{id: __MODULE__, start: {__MODULE__, :start_link, [opts]}, type: :supervisor}
  end

  @doc false
  def start_link(opts) do
    children = [
      # `:pg` needs its scope before a listener can join it.
      %{id: :pg, start: {:pg, :start_link, []}},
      {Local, opts},
      Listener
    ]

    Supervisor.start_link(children, strategy: :one_for_one, name: __MODULE__.Supervisor)
  end

  @doc """
  Count one hit here, and tell the other nodes to count it too.

  The local count decides the answer, so the caller waits for no other node.
  """
  def hit(key, scale, limit, increment \\ 1) do
    broadcast({:inc, key, scale, increment})
    Local.hit(key, scale, limit, increment)
  end

  defp broadcast(message) do
    local = Process.whereis(Listener)

    for pid <- :pg.get_members(@group), pid != local do
      send(pid, message)
    end

    :ok
  end
end

defmodule Weft.Limits do
  @moduledoc ~S"""
  The limits an actor and a caller must obey.

  A limit here is a contract with the caller, and it is not a tuning constant. A tuning
  constant is a guess about a workload. A value below is one of two things. It is a promise
  weft makes to the person who writes an actor, or it is a bound from a measurement.

  ## The values, and where each one is enforced

  | limit | value | enforced at | where the value comes from |
  | --- | --- | --- | --- |
  | storage for one actor | 10 GiB | `Weft.Actor`, before the write | a promise |
  | one key | 2 KiB | `Weft.Actor`, before the write | a promise |
  | one value | 128 KiB | `Weft.Actor`, before the write | a promise |
  | one action | 60 s | `with_in_flight/1` | a promise |
  | requests for each minute for each address | 1200 | `Weft.Gateway.dispatch/1` | a promise |
  | requests in flight | 32 | `Weft.Gateway.dispatch/1` | measured, see below |
  | entities in one bus message | 336 | the harness, when it exists | measured, see below |

  A promise is what weft tells the person who writes an actor. A measured value comes from
  a run that is written down, and the run says which one.

  ## Why 32 in flight

  `store_plane_logbook.md` sweeps the number of commits in flight from 1 to 512. The
  latency stays nearly flat to about 64, and then it carries the load. From 64 to 512 the
  rate rises 2.2 times and the latency rises 3.4 times.

  So 32 sits inside the flat part of the curve. A caller at 32 gets 12918 commits each
  second, at 1.7 times the unloaded latency. A larger number buys throughput that one
  caller cannot use. It pays for that in the latency of every other caller.

  ## Why 336 entities in a bus message

  The batch sweep in `data_plane_logbook.md` splits the cost of a message in two. A fixed
  part of 419 ns that a message pays once, and a marginal part of 1.25 ns for each entity
  in it. Both come out of the same run.

  336 is where those two are equal. Below it a message spends more time on the bus than on
  its payload, and above it the payload dominates. So it is a knee and not a size that was
  picked, the same way `in_flight` is a knee and not a number that was picked.

  Read the floor and the knee as two different things, because the difference is large.

  | entities in a message | fixed cost for each entity | overhead |
  | --- | --- | --- |
  | 7 | 59.9 ns | 98% |
  | 32 | 13.1 ns | 91% |
  | 256 | 1.6 ns | 57% |
  | 336 | 1.25 ns | 50% |
  | 1024 | 0.4 ns | 25% |

  **The floor is 7.** That is 15 M snapshots each second divided by the 2.38 M messages
  each second the bus does. Below 7 the bus cannot reach the target however many cores it
  gets. A message of 7 is 98% overhead, so the floor says where the bus stops failing, and
  it does not say where to run.

  **The knee is 336.** A replication frame already carries 256, which is 57% overhead and
  close enough that no change is needed for it.

  Both numbers move on their own when a measurement changes, which is the point. A faster
  bus lowers the floor. A cheaper payload raises the knee. Neither is a guess about a
  workload.

  ## What enforces each one

  weft holds the numbers. It does not hold the mechanism, because each mechanism exists
  already and is tested by more people than weft has.

  - **The rate for each address** is Hammer, and it counts across the cluster.
    `Weft.Limits.Rate` says how, and what an eventually consistent count costs.
  - **The requests in flight** is `Task.Supervisor` with `max_children`. OTP counts the
    children and refuses the one that goes over. A child that dies is removed by its
    supervisor, so a crash cannot leak a slot.
  - **The time for one action** is a receive timeout, the same one that
    `GenServer.call/3` applies to every call.
  - **The sizes** are `byte_size/1` on the encoded term.

  ## What refuses, and what does not

  A limit refuses the work. It does not accept the work and then drop it. Every function
  here returns `{:ok, _}` or `{:error, _}`. The error says which limit and by how much, so
  a caller can report the cause without guessing.

  `Weft.Actor.put/3` returns `{:error, {:limit, which, limit: _, actual: _}}` and it
  writes nothing. `Weft.Gateway.dispatch/1` returns the same shape.

  A request with no address is a call from inside the node. It is not counted, because
  these limits bound what one caller outside may take.

  ## What is not enforced yet

  The action limit bounds a request at the gateway. It does not bound an actor action that
  runs long inside its own `handle_call`. That call holds the process of the actor, and no
  gateway request waits on it.
  """

  @storage_bytes 10 * 1024 * 1024 * 1024
  @key_bytes 2 * 1024
  @value_bytes 128 * 1024
  @action_ms 60_000
  @requests_each_minute 1200
  @in_flight 32

  # The measurements the two batch numbers come out of. None is a limit on its own, so
  # none is in `get/1`. `data_plane_logbook.md` holds the run for each.
  #
  # The rates give the floor: below it the bus cannot reach the target at all.
  @snapshots_each_second 15_000_000
  @bus_messages_each_second 2_380_000

  # Three points from the batch sweep, in nanoseconds for one message. The fixed part and
  # the marginal part are derived from them rather than written down, so a rounding here
  # cannot drift from the run.
  @message_ns_at_1 420.3
  @message_ns_at_8 424.5
  @message_ns_at_1024 1695.1

  @window_ms 60_000
  @flight_supervisor __MODULE__.InFlight

  @type limit ::
          :storage_bytes
          | :key_bytes
          | :value_bytes
          | :action_ms
          | :requests_each_minute
          | :in_flight
          | :snapshot_batch
          | :snapshot_batch_floor

  @type error :: {:limit, limit(), [{:limit, non_neg_integer()} | {:actual, non_neg_integer()}]}

  @doc """
  The children that hold the counts.

  Hammer keeps the window for each address. The supervisor counts what is in flight, and
  it holds no state of its own beyond its children.
  """
  @spec children() :: [Supervisor.child_spec() | {module(), term()}]
  def children do
    [
      {Weft.Limits.Rate, [clean_period: :timer.minutes(10)]},
      {Task.Supervisor, name: @flight_supervisor, max_children: @in_flight}
    ]
  end

  @doc "The value of one limit."
  @spec get(limit()) :: non_neg_integer()
  def get(:storage_bytes), do: @storage_bytes
  def get(:key_bytes), do: @key_bytes
  def get(:value_bytes), do: @value_bytes
  def get(:action_ms), do: @action_ms
  def get(:requests_each_minute), do: @requests_each_minute
  def get(:in_flight), do: @in_flight
  def get(:snapshot_batch) do
    # What one entity adds, from the two ends of the flat part of the sweep.
    marginal = (@message_ns_at_1024 - @message_ns_at_8) / (1024 - 8)

    # What a message pays once, which is the one-entity cost less that one entity.
    fixed = @message_ns_at_1 - marginal

    ceil(fixed / marginal)
  end
  def get(:snapshot_batch_floor), do: ceil(@snapshots_each_second / @bus_messages_each_second)

  @doc """
  Check a key against the key limit.

  The size is the size of the encoded term, because that is what the store writes.
  """
  @spec check_key(term()) :: {:ok, non_neg_integer()} | {:error, error()}
  def check_key(key), do: check_size(:key_bytes, byte_size(:erlang.term_to_binary(key)))

  @doc "Check a value against the value limit."
  @spec check_value(term()) :: {:ok, non_neg_integer()} | {:error, error()}
  def check_value(value), do: check_size(:value_bytes, byte_size(:erlang.term_to_binary(value)))

  @doc """
  Check what one actor holds against the storage limit.

  `added` is what a write is about to add, so a caller can ask before it writes rather
  than after.
  """
  @spec check_storage(non_neg_integer(), non_neg_integer()) ::
          {:ok, non_neg_integer()} | {:error, error()}
  def check_storage(held, added \\ 0)
      when is_integer(held) and held >= 0 and is_integer(added) and added >= 0 do
    check_size(:storage_bytes, held + added)
  end

  defp check_size(limit, actual) do
    allowed = get(limit)

    if actual > allowed do
      {:error, {:limit, limit, limit: allowed, actual: actual}}
    else
      {:ok, actual}
    end
  end

  @doc """
  Count one request from `address`, and refuse it when the address is over its rate.

  Hammer keeps a sliding window of one minute for each address.
  """
  @spec take_request(term()) :: {:ok, non_neg_integer()} | {:error, error()}
  def take_request(address) do
    key = "req:" <> :erlang.term_to_binary(address)

    case Weft.Limits.Rate.hit(key, @window_ms, @requests_each_minute) do
      {:allow, count} ->
        {:ok, count}

      {:deny, _retry_after_ms} ->
        {:error,
         {:limit, :requests_each_minute,
          limit: @requests_each_minute, actual: @requests_each_minute + 1}}
    end
  end

  @doc """
  Run `fun` while it counts against the in-flight limit.

  OTP counts the children of the supervisor and refuses the one that goes over, so the
  count cannot leak. A child that crashes is removed by its supervisor, and the slot comes
  back with it.

  The caller waits for one action at most, which is the action limit.
  """
  @spec with_in_flight((-> result)) :: {:ok, result} | {:error, error() | term()}
        when result: term()
  def with_in_flight(fun) when is_function(fun, 0) do
    parent = self()

    case Task.Supervisor.start_child(@flight_supervisor, fn ->
           send(parent, {__MODULE__, self(), fun.()})
         end) do
      {:ok, pid} ->
        await(Process.monitor(pid), pid)

      {:error, :max_children} ->
        {:error, {:limit, :in_flight, limit: @in_flight, actual: @in_flight + 1}}

      {:error, reason} ->
        {:error, reason}
    end
  end

  defp await(ref, pid) do
    receive do
      {__MODULE__, ^pid, result} ->
        # Wait for the process to end, and not only for its result. The supervisor frees
        # the slot when the child dies, which is just after it sends. A caller that
        # returned on the result alone could be refused for the slot of its own call that
        # had already finished.
        receive do
          {:DOWN, ^ref, :process, ^pid, _reason} -> :ok
        after
          @action_ms ->
            Process.demonitor(ref, [:flush])
            :ok
        end

        {:ok, result}

      {:DOWN, ^ref, :process, ^pid, reason} ->
        {:error, reason}
    after
      @action_ms ->
        Process.demonitor(ref, [:flush])
        Process.exit(pid, :kill)
        {:error, {:limit, :action_ms, limit: @action_ms, actual: @action_ms}}
    end
  end

  @doc "How many requests are in flight."
  @spec in_flight() :: non_neg_integer()
  def in_flight, do: length(Task.Supervisor.children(@flight_supervisor))
end
