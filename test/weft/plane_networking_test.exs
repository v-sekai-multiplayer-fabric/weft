defmodule Weft.PlaneNetworkingTest do
  use ExUnit.Case, async: true

  @moduledoc """
  A plane has no networking. This test enforces that, in the source.

  The rule is a definition and not a default, so there is no exception to check. An edge is
  a plane with networking. Every other plane opens no socket.

  The rule was broken once, and it was broken by a copy. `native/gyreplane` came in from
  `zone-server-h2o`, which terminates QUIC in the same process that holds authority. That
  is not a plane, and it is not an edge either, because an edge holds no authority.

  A comment in a build file does not stop the next copy. This test does. It reads the
  source of each plane and it fails if the plane gains a socket or a transport library
  again.

  `native/gyreedge` is an edge, so it is not in the list below. It may do all of this.
  """

  @planes ["native/dataplane", "native/storeplane", "native/nif", "native/gyreplane"]

  # Vendored code is not weft's to change. A plane may hold a library that contains a
  # socket, and the rule is about what the plane itself calls and what its build links.
  @vendored ["thirdparty"]

  @sources [".c", ".h", ".cc", ".cpp", ".hpp"]

  # The calls that open or use a socket. A plane makes none of them.
  @socket_calls ~w(socket bind listen accept accept4 recvfrom sendto recvmsg sendmsg
                   recvmmsg sendmmsg getaddrinfo setsockopt)

  # The transport libraries. A plane links none of them.
  @transports ~w(picoquic picotls quiche ngtcp2 nghttp3 lsquic msquic)

  defp files_of(plane, extensions) do
    plane
    |> Kernel.<>("/**/*")
    |> Path.wildcard()
    |> Enum.filter(&File.regular?/1)
    |> Enum.reject(fn path ->
      Enum.any?(@vendored, &(&1 in Path.split(path)))
    end)
    |> Enum.filter(&(extensions == :any or Path.extname(&1) in extensions))
  end

  defp offences(plane, extensions, pattern) do
    plane
    |> files_of(extensions)
    |> Enum.filter(fn path ->
      case File.read(path) do
        {:ok, body} -> Regex.match?(pattern, body)
        _ -> false
      end
    end)
  end

  describe "a plane has no networking" do
    test "no plane calls a socket function" do
      pattern = ~r/\b(?:#{Enum.join(@socket_calls, "|")})\s*\(/

      found =
        Enum.flat_map(@planes, fn plane ->
          plane |> offences(@sources, pattern) |> Enum.map(&{plane, &1})
        end)

      assert found == [],
             "a plane called a socket function. A plane has no networking, and an edge " <>
               "is a plane with networking. Move the code to an edge.\n" <>
               Enum.map_join(found, "\n", fn {plane, path} -> "  #{plane}: #{path}" end)
    end

    test "no plane includes a transport library header" do
      pattern = ~r/#\s*include\s*[<"][^>"]*(?:#{Enum.join(@transports, "|")})/i

      found =
        Enum.flat_map(@planes, fn plane ->
          plane |> offences(@sources, pattern) |> Enum.map(&{plane, &1})
        end)

      assert found == [], "a plane included a transport header:\n" <> inspect(found, pretty: true)
    end

    test "no plane vendors a transport library" do
      found =
        for plane <- @planes,
            transport <- @transports,
            path = Path.join([plane, "thirdparty", transport]),
            File.exists?(path),
            do: path

      assert found == [],
             "a plane vendors a transport library. It belongs to an edge:\n" <>
               Enum.join(found, "\n")
    end

    test "no plane build links a transport library" do
      pattern = ~r/^[^#\n]*(?:#{Enum.join(@transports, "|")})/im

      found =
        Enum.flat_map(@planes, fn plane ->
          plane
          |> offences([".txt", ".cmake"], pattern)
          |> Enum.map(&{plane, &1})
        end)

      assert found == [],
             "a plane build referenced a transport library outside a comment:\n" <>
               inspect(found, pretty: true)
    end
  end

  describe "an edge is a plane with networking" do
    test "the edge holds the transport the plane gave up" do
      # This is the other half of the rule. If the transport is in neither directory, then
      # the split above deleted working code rather than moving it.
      assert File.dir?("native/gyreedge/transport"),
             "the QUIC transport is gone from the plane and it is not in the edge either"

      assert File.dir?("native/gyreedge/thirdparty/picoquic"),
             "picoquic is gone from the plane and it is not in the edge either"
    end
  end
end
