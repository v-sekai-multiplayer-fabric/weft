# asset CDN baker plane

Goal: a baker plane that turns glb into an OpenUSD stage, plus stage-tier distribution.

State: partial. `Weft.Assets.StageTier` is the desync adapter. The chunks go into
SQLite then FoundationDB. An on-demand H3/WebTransport endpoint serves the chunks. The
baker itself is not built.

Next: build the baker (fabric-stage-runtime plus Adobe glTF). This needs a native OpenUSD
toolchain, which we do not have here yet.
