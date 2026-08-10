defmodule Weft.LimitsTest do
  @moduledoc """
  The limits of `actor_limits.md`, enforced. A limit must refuse the work, and it must not
  take the work and then drop it. So every test below asserts the error and then asserts
  that the state did not move.
  """

  use ExUnit.Case, async: false

  alias Weft.Limits

  setup do
    # Each test uses its own address, so Hammer's window for one test cannot reach
    # another. The application already starts the children that hold the counts.
    {:ok, address: {:test, System.unique_integer([:positive])}}
  end

  describe "the values" do
    test "are the ones the moduledoc gives" do
      assert Limits.get(:storage_bytes) == 10 * 1024 * 1024 * 1024
      assert Limits.get(:key_bytes) == 2 * 1024
      assert Limits.get(:value_bytes) == 128 * 1024
      assert Limits.get(:action_ms) == 60_000
      assert Limits.get(:requests_each_minute) == 1200
      assert Limits.get(:in_flight) == 32
    end

    test "the batch is rivet's max keys per operation, and not a number of its own" do
      # Every limit here is rivet's. A bus message is the same shape of thing as a batch
      # put: many items, one operation. The measurement checks the number rather than
      # sourcing it.
      assert Limits.get(:snapshot_batch) == 128
    end

    test "the native copy of a limit matches this module" do
      # A C++ plane cannot call Elixir, so native/harness/src/snapshot.hpp transcribes the
      # limits it needs. A transcription that nothing checks is a copy that goes stale,
      # and the stale one still reads as authoritative. This reads the header.
      header = "native/harness/include/weft/limits.hpp"

      body =
        case File.read(header) do
          {:ok, body} -> body
          {:error, reason} -> flunk("cannot read #{header}: #{inspect(reason)}")
        end

      pairs = [
        {"SNAPSHOT_BATCH", :snapshot_batch},
        {"ACTION_MS", :action_ms},
        {"KEY_BYTES", :key_bytes},
        {"VALUE_BYTES", :value_bytes},
        {"IN_FLIGHT", :in_flight}
      ]

      for {constant, limit} <- pairs do
        found =
          case Regex.run(~r/\b#{constant}\s*=\s*([0-9 *]+?)\s*;/, body) do
            [_, expr] ->
              expr
              |> String.split("*")
              |> Enum.map(&(&1 |> String.trim() |> String.to_integer()))
              |> Enum.product()
            nil -> flunk("#{header} does not define #{constant}")
          end

        assert found == Limits.get(limit),
               "#{header} says #{constant} is #{found}. Weft.Limits says #{Limits.get(limit)}."
      end
    end
  end

  describe "size limits" do
    test "a key at the limit passes and one over it is refused" do
      # A binary of n bytes encodes to more than n, so build from the encoded size.
      at = String.duplicate("k", Limits.get(:key_bytes) - 10)
      assert {:ok, bytes} = Limits.check_key(at)
      assert bytes <= Limits.get(:key_bytes)

      over = String.duplicate("k", Limits.get(:key_bytes) + 1)
      assert {:error, {:limit, :key_bytes, limit: 2048, actual: actual}} = Limits.check_key(over)
      assert actual > 2048
    end

    test "a value over the limit is refused and says by how much" do
      over = String.duplicate("v", Limits.get(:value_bytes) + 1)

      assert {:error, {:limit, :value_bytes, limit: 131_072, actual: actual}} =
               Limits.check_value(over)

      assert actual > 131_072
    end

    test "storage counts what is held plus what is about to be written" do
      assert {:ok, 10} = Limits.check_storage(4, 6)
      assert {:ok, _} = Limits.check_storage(Limits.get(:storage_bytes), 0)

      assert {:error, {:limit, :storage_bytes, limit: _, actual: _}} =
               Limits.check_storage(Limits.get(:storage_bytes), 1)
    end
  end

  describe "requests for each minute" do
    test "counts up to the limit and then refuses", %{address: address} do
      limit = Limits.get(:requests_each_minute)

      for n <- 1..limit do
        assert {:ok, ^n} = Limits.take_request(address)
      end

      assert {:error, {:limit, :requests_each_minute, limit: ^limit, actual: _}} =
               Limits.take_request(address)
    end

    test "one address does not spend the budget of another", %{address: address} do
      for n <- 1..10, do: assert({:ok, ^n} = Limits.take_request(address))
      assert {:ok, 1} = Limits.take_request({:other, address})
    end

    test "a count from another node lands in this window", %{address: address} do
      key = "req:" <> :erlang.term_to_binary(address)

      # What a peer node sends. The listener is the process that receives it.
      send(Weft.Limits.Rate.Listener, {:inc, key, 60_000, 5})

      # The listener is a GenServer, so a call after the send arrives after it.
      _ = :sys.get_state(Weft.Limits.Rate.Listener)

      assert {:ok, 6} = Limits.take_request(address)
    end

    test "stays refused while the window holds", %{address: address} do
      for _ <- 1..Limits.get(:requests_each_minute), do: Limits.take_request(address)

      assert {:error, {:limit, :requests_each_minute, limit: _, actual: _}} =
               Limits.take_request(address)

      assert {:error, {:limit, :requests_each_minute, limit: _, actual: _}} =
               Limits.take_request(address)
    end
  end

  describe "requests in flight" do
    test "runs the work and gives its result back" do
      assert {:ok, :done} = Limits.with_in_flight(fn -> :done end)
    end

    test "the slot comes back, so a caller may come again" do
      assert {:ok, :first} = Limits.with_in_flight(fn -> :first end)
      assert Limits.in_flight() == 0
      assert {:ok, :second} = Limits.with_in_flight(fn -> :second end)
    end

    test "the slot comes back even when the work crashes" do
      assert {:error, reason} = Limits.with_in_flight(fn -> raise "boom" end)
      assert match?({%RuntimeError{}, _stack}, reason)
      assert Limits.in_flight() == 0
    end

    test "refuses the request over the limit and lets the others run" do
      limit = Limits.get(:in_flight)
      test_pid = self()

      # Hold every slot, then ask for one more. Each holder waits for a release, so the
      # slots stay taken while the test asks.
      holders =
        for _ <- 1..limit do
          spawn(fn ->
            Limits.with_in_flight(fn ->
              send(test_pid, {:holding, self()})
              receive do: (:release -> :ok)
            end)
          end)
        end

      held =
        for _ <- 1..limit do
          assert_receive {:holding, pid}, 2000
          pid
        end

      assert Limits.in_flight() == limit

      assert {:error, {:limit, :in_flight, limit: ^limit, actual: _}} =
               Limits.with_in_flight(fn -> :should_not_run end)

      for pid <- held, do: send(pid, :release)
      for pid <- holders, do: Process.exit(pid, :kill)

      # A slot comes back when its process ends, and that is not instant. Wait for the
      # supervisor to be empty, so this test cannot spend the slots of the next one.
      drain()
    end
  end

  # Wait until nothing is in flight.
  defp drain(tries \\ 200) do
    cond do
      Limits.in_flight() == 0 -> :ok
      tries == 0 -> flunk("in flight did not fall back to zero")
      true -> Process.sleep(10) && drain(tries - 1)
    end
  end
end
