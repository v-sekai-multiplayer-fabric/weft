defmodule RivetExTest do
  use ExUnit.Case
  doctest RivetEx

  test "greets the world" do
    assert RivetEx.hello() == :world
  end
end
