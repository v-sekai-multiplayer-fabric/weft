defmodule Weft.PlaneNetworkingTest do
  use ExUnit.Case, async: true

  @moduledoc """
  A plane has no networking. This test enforces that, in the source.

  The rule is a definition and not a default, so there is no exception to check. An edge is
  a plane with networking. Every other plane opens no socket.

  The rule was broken once, and it was broken by a copy. `gyreplane` came in from
  `gyreplane`, which terminates QUIC in the same process that holds authority. That
  is not a plane, and it is not an edge either, because an edge holds no authority.

  A comment in a build file does not stop the next copy. This test does. It reads the
  source of each plane and it fails if the plane gains a socket or a transport library
  again.

  `fabric-edge` is an edge, so it is not in the list below. It may do all of this.
  """

  @planes ["native/dataplane", "fabric-store-plane", "native/nif", "gyreplane"]

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

  describe "weft builds no plane" do
    # Every plane and every edge is its own repository now. weft starts them, watches
    # them, and restarts them, and it reaches them only through the data plane. A plane
    # that grows back into this repository is a plane weft would have to build.

    test "native holds only the data plane and the NIF" do
      found =
        "native/*"
        |> Path.wildcard()
        |> Enum.filter(&File.dir?/1)
        |> Enum.map(&Path.basename/1)
        |> Enum.reject(&(&1 == "build"))
        |> Enum.sort()

      assert found == ["dataplane", "nif"],
             "native/ holds a plane again. A plane is its own repository: see " <>
               "native/README.md.\n  found: #{Enum.join(found, ", ")}"
    end

    test "weft declares no bus and vendors no library" do
      sigs = Path.wildcard("native/**/*.sigs")
      assert sigs == [], "the iceoryx2 C ABI belongs to fabric-harness:\n#{Enum.join(sigs, "\n")}"

      vendored = Path.wildcard("native/**/thirdparty")
      assert vendored == [],
             "weft vendors a native library again:\n#{Enum.join(vendored, "\n")}"
    end

    test "the root build reaches every directory that has a build file" do
      root = File.read!("native/CMakeLists.txt")

      buildable =
        "native/*/CMakeLists.txt"
        |> Path.wildcard()
        |> Enum.map(&(&1 |> Path.dirname() |> Path.basename()))

      missing = Enum.reject(buildable, &(root =~ "add_subdirectory(#{&1})"))

      assert missing == [], "native/CMakeLists.txt does not add: #{Enum.join(missing, ", ")}"
    end
  end
end
