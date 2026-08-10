# SPDX-License-Identifier: MIT
# Copyright (c) 2026 K. S. Ernest (iFire) Lee

defmodule Taskweft.MCExecutor do
  @moduledoc """
  Monte Carlo executor — runs a plan against a domain with per-action success
  probabilities and returns an execution trace.
  """

  alias Taskweft.NIF

  @spec execute(String.t(), String.t(), String.t(), non_neg_integer()) ::
          {:ok, String.t()} | {:error, term()}
  def execute(domain_json, plan_json, probs_json, seed) do
    {:ok, NIF.mc_execute(domain_json, plan_json, probs_json, seed)}
  rescue
    e -> {:error, Exception.message(e)}
  end
end
