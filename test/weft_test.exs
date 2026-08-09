defmodule WeftTest do
  use ExUnit.Case
  doctest Weft

  test "greets the world" do
    assert Weft.hello() == :world
  end
end
