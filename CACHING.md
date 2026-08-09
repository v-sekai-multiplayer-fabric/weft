# Native binary release caching

The native NIFs (`weft_dataplane_nif`, and the OpenUSD glb→stage NIF) are built
once in CI and cached as GitHub release artifacts, so `mix compile` downloads a
verified prebuilt binary instead of recompiling from source. This matters most for
the OpenUSD NIF, whose build environment (via `fabric-stage-runtime`) is expensive
to reproduce on every machine.

Flow:

1. Tag a release (`vX.Y.Z`). `.github/workflows/precompile.yml` runs
   `mix elixir_make.precompile`, which builds `priv/*.so` for the runner target and
   packs `cache/*.tar.gz`, then attaches them to the release and commits
   `checksum.exs`.
2. On any machine, `mix compile` (via `elixir_make` + `cc_precompiler`) downloads
   the artifact matching the host triplet + NIF version from
   `make_precompiler_url`, verifies it against `checksum.exs`, and unpacks it.
3. If no matching artifact exists (or the repo is private and no token is set), it
   **falls back to building from source** with the `Makefile`.

Force a source build with `WEFT_BUILD=1 mix compile` once wired, or delete
`priv/*.so`. Private-repo artifact downloads need a GitHub token in the environment;
public releases need none.
