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
  constant is a guess about a workload. Each value below is either a promise weft makes to
  the person who writes an actor, or a bound that comes from a measurement.

  ## The values

  | limit | value | where it comes from |
  | --- | --- | --- |
  | storage for one actor | 10 GiB | the promise in `actor_limits.md` |
  | one key | 2 KiB | the promise in `actor_limits.md` |
  | one value | 128 KiB | the promise in `actor_limits.md` |
  | one action | 60 s | the promise in `actor_limits.md` |
  | requests for each minute for each address | 1200 | the promise in `actor_limits.md` |
  | requests in flight | 32 | measured, see below |

  ## Why 32 in flight

  `store_plane_logbook.md` sweeps the number of commits in flight from 1 to 512. The
  latency stays nearly flat to about 64, and then it carries the load. From 64 to 512 the
  rate rises 2.2 times and the latency rises 3.4 times.

  So 32 sits inside the flat part of the curve. A caller at 32 gets 12918 commits each
  second, at 1.7 times the unloaded latency. A larger number buys throughput that one
  caller cannot use, and it pays for that in the latency of every other caller.

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
  here returns `{:ok, _}` or `{:error, _}`, and the error says which limit and by how
  much, so a caller can report the cause without guessing.
  """

  @storage_bytes 10 * 1024 * 1024 * 1024
  @key_bytes 2 * 1024
  @value_bytes 128 * 1024
  @action_ms 60_000
  @requests_each_minute 1200
  @in_flight 32

  @window_ms 60_000
  @flight_supervisor __MODULE__.InFlight

  @type limit ::
          :storage_bytes
          | :key_bytes
          | :value_bytes
          | :action_ms
          | :requests_each_minute
          | :in_flight

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

  @doc "Every limit and its value."
  @spec all() :: %{limit() => non_neg_integer()}
  def all do
    %{
      storage_bytes: @storage_bytes,
      key_bytes: @key_bytes,
      value_bytes: @value_bytes,
      action_ms: @action_ms,
      requests_each_minute: @requests_each_minute,
      in_flight: @in_flight
    }
  end

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

  @doc "Check how long an action ran, or is allowed to run, against the action limit."
  @spec check_action_ms(non_neg_integer()) :: {:ok, non_neg_integer()} | {:error, error()}
  def check_action_ms(ms) when is_integer(ms) and ms >= 0, do: check_size(:action_ms, ms)

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
        Process.demonitor(ref, [:flush])
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
