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
  | keys in one batch operation | 128 | the harness, when it exists | rivet, see below |

  A promise is what weft tells the person who writes an actor. A measured value comes from
  a run that is written down, and the run says which one.

  ## Where the numbers come from

  Every value in this module is rivet's, at <https://rivet.dev/docs/actors/limits/>. weft
  copies rivet's store layout, so it copies rivet's limits with it. The whole table is
  transcribed below, and not only the rows weft enforces today.

  This is deliberate, and it is cheaper than it looks. A number weft invents is a guess
  about a workload weft has not seen. A number rivet publishes is one that a running
  system already lives with. So the rule for a new limit is to look there first, and to
  invent one only when nothing there fits.

  Transcribing a row weft does not enforce is not waste. The cost of a missing row is that
  the next person invents a number, and then two numbers exist for one concept. The cost
  of an unused row is a line.

  rivet splits a limit into soft and hard. weft records one value for each row: the soft
  limit where rivet has one, because that is what an application meets first, and the hard
  limit otherwise. `get/1` returns that value.

  ## Why 32 in flight

  `../logbook/store_plane.md` sweeps the number of commits in flight from 1 to 512. The
  latency stays nearly flat to about 64, and then it carries the load. From 64 to 512 the
  rate rises 2.2 times and the latency rises 3.4 times.

  So 32 sits inside the flat part of the curve. A caller at 32 gets 12918 commits each
  second, at 1.7 times the unloaded latency. A larger number buys throughput that one
  caller cannot use. It pays for that in the latency of every other caller.

  ## Why 128 entities in a bus message

  It is rivet's max keys per operation, which is the count of items in one batch get, put,
  or delete. A bus message is the same shape of thing: many items, one operation. So this
  is not a new number.

  What the measurement adds is whether that number lands in the right place for a bus.
  `../logbook/data_plane.md` says it does, and it bounds 128 on both sides.

  **Below 7 the bus fails.** 15 M snapshots each second divided by the 2.38 M messages
  each second the bus does at batch 1. A message that small is 98% overhead, so the floor
  says where the bus stops reaching the target, and not where to run.

  **At 336 a message stops being mostly overhead.** A message costs 419 ns once plus
  1.25 ns for each entity, and those are equal at 336.

  128 sits between them. It is 18 times the floor, it is 72% overhead, and it carries
  214.68 M snapshots each second on one core, which clears the 15 M target by 14 times.
  Going to 256 buys 1.5 times the rate and costs 1.3 times the latency of a message.

  Neither 7 nor 336 is a limit here. They are the check on 128, and the logbook holds
  them.

  ## Every limit, by what it bounds

  ### WebSocket

  These bound a connection that uses `.connect()`.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:ws_incoming_bytes` | 64 KiB | max incoming message |
  | `:ws_outgoing_bytes` | 1 MiB | max outgoing message |
  | `:ws_frame_bytes` | 32 MiB | max frame |
  | `:ws_open_ms` | 15 s | open timeout |
  | `:ws_ack_ms` | 30 s | message ack timeout |

  ### Hibernating WebSocket

  These apply while an actor sleeps and its clients stay connected.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:hibernate_buffer_bytes` | 128 MiB | max pending buffer |
  | `:hibernate_messages` | 65535 | max pending messages |
  | `:hibernate_ms` | 90 s | hibernation timeout |

  ### HTTP

  These bound an action that does not use `.connect()`.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:http_request_bytes` | 20 MiB | max request body |
  | `:http_response_bytes` | 20 MiB | max response body |

  ### Networking

  One limit, and it only shows up when the network is already broken.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:ping_ms` | 30 s | connection ping timeout |

  ### Queue

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:queue_messages` | 1000 | max queue size |
  | `:queue_message_bytes` | 128 KiB | max queue message, effective |

  ### Key and value storage

  The layer under an actor. `Weft.Actor` enforces the first three.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:storage_bytes` | 10 GiB | max storage for one actor |
  | `:key_bytes` | 2 KiB | max key |
  | `:value_bytes` | 128 KiB | max value |
  | `:keys_each_operation` | 128 | max keys in one operation |
  | `:batch_put_bytes` | 976 KiB | max batch put payload |
  | `:list_keys` | 16384 | default list limit |

  ### SQLite storage

  The store plane. `../../native/storeplane/README.md` derives its transaction bounds from the FoundationDB value size, and these are the shape rivet arrived at.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:commit_dirty_bytes` | 1310720 B, which is 320 pages of 4 KiB | max dirty data for one commit |
  | `:transaction_queue` | 128 operations | transaction coordinator queue |

  ### Actor runtime socket

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:socket_frame_bytes` | 32 MiB | frame payload |

  ### Preloading

  What an actor may be handed at start, so it does not go to storage first.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:preload_bytes` | 1 MiB | max total preload |
  | `:preload_workflow_bytes` | 128 KiB | max workflow preload |
  | `:preload_connections_bytes` | 64 KiB | max connections preload |

  ### Actor input and naming

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:input_bytes` | 4 MiB | max actor input |
  | `:connection_params_bytes` | 4 KiB | max connection params |
  | `:key_component_bytes` | 128 B | max key component |
  | `:key_total_bytes` | 1024 B | max key total |
  | `:name_length` | 64 characters | max name length |

  ### Timeouts

  `action_ms` is the one weft enforces. The rest bound a lifecycle hook rivet has and weft does not have yet.

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:action_ms` | 60 s | action, and raw request |
  | `:before_connect_ms` | 5 s | before connect hook |
  | `:create_vars_ms` | 5 s | create vars hook |
  | `:create_conn_state_ms` | 5 s | create connection state hook |
  | `:on_connect_ms` | 5 s | connect hook |
  | `:on_migrate_ms` | 30 s | migrate hook |
  | `:sleep_grace_ms` | 15 s | graceful shutdown budget |
  | `:sleep_ms` | 30 s | inactivity before hibernation |
  | `:state_save_ms` | 1 s | interval between state saves |
  | `:liveness_ms` | 2.5 s | connection liveness timeout |
  | `:liveness_interval_ms` | 5 s | connection liveness interval |

  ### Shutdown and lifecycle

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:request_lifespan_ms` | 3600 s | request lifespan before drain |
  | `:drain_grace_ms` | 30 min | runner drain grace |
  | `:drain_fallback_ms` | 10 s | engine drain fallback |
  | `:actor_start_ms` | 30 s | start threshold |
  | `:actor_stop_ms` | 30 min | stop threshold |

  ### Rate

  | limit | value | what rivet calls it |
  | --- | --- | --- |
  | `:requests_each_minute` | 1200 | requests for each minute for each address |
  | `:in_flight` | 32 | requests in flight |

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

  # rivet's max keys per operation. A bus message is the same shape of thing as a batch
  # put: many items, one operation. docs/logbook/data_plane.md checks it against the batch
  # sweep, which bounds it at 7 below and 336 above.
  @snapshot_batch 128

  # rivet's remaining limits, transcribed. weft records every one, and it enforces the
  # few marked in the tables above. A number that is written down cannot be invented
  # again later by someone who did not find it.
  @ws_incoming_bytes 64 * 1024
  @ws_outgoing_bytes 1024 * 1024
  @ws_frame_bytes 32 * 1024 * 1024
  @ws_open_ms 15_000
  @ws_ack_ms 30_000
  @hibernate_buffer_bytes 128 * 1024 * 1024
  @hibernate_messages 65_535
  @hibernate_ms 90_000
  @http_request_bytes 20 * 1024 * 1024
  @http_response_bytes 20 * 1024 * 1024
  @ping_ms 30_000
  @queue_messages 1_000
  @queue_message_bytes 128 * 1024
  @batch_put_bytes 976 * 1024
  @list_keys 16_384
  @commit_dirty_bytes 320 * 4 * 1024
  @transaction_queue 128
  @socket_frame_bytes 32 * 1024 * 1024
  @preload_bytes 1024 * 1024
  @preload_workflow_bytes 128 * 1024
  @preload_connections_bytes 64 * 1024
  @input_bytes 4 * 1024 * 1024
  @connection_params_bytes 4 * 1024
  @key_component_bytes 128
  @key_total_bytes 1_024
  @name_length 64
  @before_connect_ms 5_000
  @create_vars_ms 5_000
  @create_conn_state_ms 5_000
  @on_connect_ms 5_000
  @on_migrate_ms 30_000
  @sleep_grace_ms 15_000
  @sleep_ms 30_000
  @state_save_ms 1_000
  @liveness_ms 2_500
  @liveness_interval_ms 5_000
  @request_lifespan_ms 3_600_000
  @drain_grace_ms 30 * 60_000
  @drain_fallback_ms 10_000
  @actor_start_ms 30_000
  @actor_stop_ms 30 * 60_000

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
          | :ws_incoming_bytes
          | :ws_outgoing_bytes
          | :ws_frame_bytes
          | :ws_open_ms
          | :ws_ack_ms
          | :hibernate_buffer_bytes
          | :hibernate_messages
          | :hibernate_ms
          | :http_request_bytes
          | :http_response_bytes
          | :ping_ms
          | :queue_messages
          | :queue_message_bytes
          | :keys_each_operation
          | :batch_put_bytes
          | :list_keys
          | :commit_dirty_bytes
          | :transaction_queue
          | :socket_frame_bytes
          | :preload_bytes
          | :preload_workflow_bytes
          | :preload_connections_bytes
          | :input_bytes
          | :connection_params_bytes
          | :key_component_bytes
          | :key_total_bytes
          | :name_length
          | :before_connect_ms
          | :create_vars_ms
          | :create_conn_state_ms
          | :on_connect_ms
          | :on_migrate_ms
          | :sleep_grace_ms
          | :sleep_ms
          | :state_save_ms
          | :liveness_ms
          | :liveness_interval_ms
          | :request_lifespan_ms
          | :drain_grace_ms
          | :drain_fallback_ms
          | :actor_start_ms
          | :actor_stop_ms

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
  def get(:snapshot_batch), do: @snapshot_batch
  def get(:ws_incoming_bytes), do: @ws_incoming_bytes
  def get(:ws_outgoing_bytes), do: @ws_outgoing_bytes
  def get(:ws_frame_bytes), do: @ws_frame_bytes
  def get(:ws_open_ms), do: @ws_open_ms
  def get(:ws_ack_ms), do: @ws_ack_ms
  def get(:hibernate_buffer_bytes), do: @hibernate_buffer_bytes
  def get(:hibernate_messages), do: @hibernate_messages
  def get(:hibernate_ms), do: @hibernate_ms
  def get(:http_request_bytes), do: @http_request_bytes
  def get(:http_response_bytes), do: @http_response_bytes
  def get(:ping_ms), do: @ping_ms
  def get(:queue_messages), do: @queue_messages
  def get(:queue_message_bytes), do: @queue_message_bytes
  def get(:keys_each_operation), do: @snapshot_batch
  def get(:batch_put_bytes), do: @batch_put_bytes
  def get(:list_keys), do: @list_keys
  def get(:commit_dirty_bytes), do: @commit_dirty_bytes
  def get(:transaction_queue), do: @transaction_queue
  def get(:socket_frame_bytes), do: @socket_frame_bytes
  def get(:preload_bytes), do: @preload_bytes
  def get(:preload_workflow_bytes), do: @preload_workflow_bytes
  def get(:preload_connections_bytes), do: @preload_connections_bytes
  def get(:input_bytes), do: @input_bytes
  def get(:connection_params_bytes), do: @connection_params_bytes
  def get(:key_component_bytes), do: @key_component_bytes
  def get(:key_total_bytes), do: @key_total_bytes
  def get(:name_length), do: @name_length
  def get(:before_connect_ms), do: @before_connect_ms
  def get(:create_vars_ms), do: @create_vars_ms
  def get(:create_conn_state_ms), do: @create_conn_state_ms
  def get(:on_connect_ms), do: @on_connect_ms
  def get(:on_migrate_ms), do: @on_migrate_ms
  def get(:sleep_grace_ms), do: @sleep_grace_ms
  def get(:sleep_ms), do: @sleep_ms
  def get(:state_save_ms), do: @state_save_ms
  def get(:liveness_ms), do: @liveness_ms
  def get(:liveness_interval_ms), do: @liveness_interval_ms
  def get(:request_lifespan_ms), do: @request_lifespan_ms
  def get(:drain_grace_ms), do: @drain_grace_ms
  def get(:drain_fallback_ms), do: @drain_fallback_ms
  def get(:actor_start_ms), do: @actor_start_ms
  def get(:actor_stop_ms), do: @actor_stop_ms

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
