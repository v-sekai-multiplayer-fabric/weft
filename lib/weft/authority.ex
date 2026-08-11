defmodule Weft.Authority do
  @moduledoc """
  One controller for one avatar, enforced across machines.

  `Weft` defines an avatar as a Character that a controller embodies. A controller is the
  human or the AI that controls it. This module holds the rule that at most one controller
  drives one avatar at a time.

  The rule must hold between machines and not only inside one node. A controller reaches
  weft through an edge, and two connections may land on two different machines. Those
  machines do not talk to each other. So an ETS table or a Horde registry cannot answer the
  question, because each node holds its own answer. FoundationDB is the one place that gives
  a global transaction, and `Weft.Entities` already puts entity ownership there for the same
  reason.

  Layout (tuple layer):

    * `("weft", "authority", avatar_id)` -> term `{controller_id, epoch}`

  ## The epoch is a fence, and there is no lease

  A dead controller cannot release an avatar. A machine that loses power releases nothing.
  The usual answer is a lease with an expiry, and weft does not use one. An expiry is a
  guess about a workload nobody measured, and `CLAUDE.md` forbids a tuning constant.

  Each claim carries an epoch instead. The epoch rises and it never falls, and it never
  repeats for one avatar. Every write names the epoch it was made under. `check/3` accepts
  a write only under the current epoch, so a superseded controller is fenced at once. Its
  packets are refused whether it knows it lost the avatar or not.

  So a takeover is explicit and immediate. `claim/2` refuses an avatar that another
  controller holds. `seize/2` takes it and fences the previous holder. There is no interval
  to wait out, and there is no interval to tune.

  A release keeps the epoch and clears the controller. The epoch therefore survives a
  release, and a controller that was fenced before the release cannot return under its old
  epoch.

  ## Testing

  Every decision is a pure function of the stored record and the request. `decide_claim/2`,
  `decide_seize/2`, `decide_release/3` and `decide_check/3` take a record and return the
  next record. FoundationDB supplies the transaction and nothing else. So the rule is
  tested without a database, and the database is tested for the transaction.
  """

  @prefix "weft"
  @authority "authority"

  @typedoc "The stored holding. A controller of `nil` means the avatar is free."
  @type holding :: {controller :: term() | nil, epoch :: non_neg_integer()}

  @typedoc "What is read from the store before a decision."
  @type current :: holding() | :not_found

  # ── Decisions ──────────────────────────────────────────────────────────────
  # Pure. Each takes what the store holds and returns what the store must hold next.

  @doc """
  Decide a claim. A claim succeeds on a free avatar. A claim by the controller that already
  holds the avatar succeeds and keeps its epoch, so the claim is idempotent and an in-flight
  write stays valid. A claim on an avatar that another controller holds is refused.
  """
  @spec decide_claim(current(), term()) ::
          {:ok, holding()} | {:error, {:held_by, term()}}
  def decide_claim(:not_found, controller), do: {:ok, {controller, 1}}
  def decide_claim({nil, epoch}, controller), do: {:ok, {controller, epoch + 1}}
  def decide_claim({controller, epoch}, controller), do: {:ok, {controller, epoch}}
  def decide_claim({holder, _epoch}, _controller), do: {:error, {:held_by, holder}}

  @doc """
  Decide a seizure. A seizure always succeeds and it always raises the epoch. The previous
  holder is fenced at the moment the transaction commits.
  """
  @spec decide_seize(current(), term()) :: {:ok, holding()}
  def decide_seize(:not_found, controller), do: {:ok, {controller, 1}}
  def decide_seize({_holder, epoch}, controller), do: {:ok, {controller, epoch + 1}}

  @doc """
  Decide a release. Only the current holder under the current epoch may release. The epoch
  is kept, so a fenced controller cannot return under an epoch it used before.
  """
  @spec decide_release(current(), term(), non_neg_integer()) ::
          {:ok, holding()} | {:error, :not_found} | {:error, :fenced}
  def decide_release(:not_found, _controller, _epoch), do: {:error, :not_found}
  def decide_release({controller, epoch}, controller, epoch), do: {:ok, {nil, epoch}}
  def decide_release({_holder, _epoch}, _controller, _claimed), do: {:error, :fenced}

  @doc """
  Decide whether a write is authoritative. A write is authoritative only from the current
  holder under the current epoch. Every other case is fenced.
  """
  @spec decide_check(current(), term(), non_neg_integer()) :: :ok | {:error, :fenced}
  def decide_check({controller, epoch}, controller, epoch), do: :ok
  def decide_check(_current, _controller, _epoch), do: {:error, :fenced}

  # ── The store ──────────────────────────────────────────────────────────────
  # One transaction each. The decision above chooses, and this writes what it chose.

  @doc "Claim `avatar_id` for `controller`. Refuses an avatar another controller holds."
  @spec claim(term(), term()) :: {:ok, non_neg_integer()} | {:error, {:held_by, term()}}
  def claim(avatar_id, controller) do
    :erlfdb.transactional(db(), fn tx ->
      case decide_claim(fetch(tx, avatar_id), controller) do
        {:ok, {_c, epoch} = record} ->
          write(tx, avatar_id, record)
          {:ok, epoch}

        {:error, reason} ->
          {:error, reason}
      end
    end)
  end

  @doc "Take `avatar_id` for `controller` and fence the previous holder."
  @spec seize(term(), term()) :: {:ok, non_neg_integer()}
  def seize(avatar_id, controller) do
    :erlfdb.transactional(db(), fn tx ->
      {:ok, {_c, epoch} = record} = decide_seize(fetch(tx, avatar_id), controller)
      write(tx, avatar_id, record)
      {:ok, epoch}
    end)
  end

  @doc "Release `avatar_id`. Only the current holder under the current epoch may release."
  @spec release(term(), term(), non_neg_integer()) ::
          :ok | {:error, :not_found} | {:error, :fenced}
  def release(avatar_id, controller, epoch) do
    :erlfdb.transactional(db(), fn tx ->
      case decide_release(fetch(tx, avatar_id), controller, epoch) do
        {:ok, record} ->
          write(tx, avatar_id, record)
          :ok

        {:error, reason} ->
          {:error, reason}
      end
    end)
  end

  @doc """
  Check that `controller` may write `avatar_id` under `epoch`. This is the guard on the
  write path, and a fenced controller is refused here.
  """
  @spec check(term(), term(), non_neg_integer()) :: :ok | {:error, :fenced}
  def check(avatar_id, controller, epoch) do
    :erlfdb.transactional(db(), fn tx ->
      decide_check(fetch(tx, avatar_id), controller, epoch)
    end)
  end

  @doc "Read the controller that holds `avatar_id`, and the epoch it holds it under."
  @spec holder(term()) :: {:ok, term() | nil, non_neg_integer()} | :not_found
  def holder(avatar_id) do
    :erlfdb.transactional(db(), fn tx ->
      case fetch(tx, avatar_id) do
        :not_found -> :not_found
        {controller, epoch} -> {:ok, controller, epoch}
      end
    end)
  end

  defp write(tx, avatar_id, record) do
    :erlfdb.set(tx, key(avatar_id), :erlang.term_to_binary(record))
  end

  defp fetch(tx, avatar_id) do
    case :erlfdb.wait(:erlfdb.get(tx, key(avatar_id))) do
      :not_found -> :not_found
      bin -> :erlang.binary_to_term(bin)
    end
  end

  defp key(avatar_id), do: :erlfdb_tuple.pack({@prefix, @authority, avatar_id})

  defp db do
    case :persistent_term.get({__MODULE__, :db}, nil) do
      nil ->
        db = :erlfdb.open(cluster_file())
        :persistent_term.put({__MODULE__, :db}, db)
        db

      db ->
        db
    end
  end

  defp cluster_file do
    case Application.get_env(:weft, :fdb_cluster_file) do
      path when is_binary(path) -> path
      nil -> raise "Weft.Authority requires config :weft, :fdb_cluster_file"
    end
  end
end
