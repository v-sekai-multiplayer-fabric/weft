# zone-guest-gyre

The Gyre: the three.js/SlugHorn web MUD client, and its Fly.io deploy
(`fly/`, `.github/workflows/deploy-gyre.yml`). Ported out of
`zone-server-h2o` (`mud/web/`, `fly/`), where it used to live in-tree.

## Status

Phase 1 of a two-phase move: this repo now holds the source, unchanged
from `zone-server-h2o`. `zone-server-h2o` still also has its own copy in
`mud/web/` and `fly/` for now — nothing has been deleted there yet, and
its live deploy (`gyre.fly.dev`) still runs from that copy, not this one.

Phase 2 (not started): a real release/CDN pipeline here, and
`zone-server-h2o` switching to fetch this repo's built web assets from
that CDN instead of keeping them in-tree.

## References

- `v-sekai-multiplayer-fabric/zone-server-h2o`: the server this client
  talks to, and this repo's origin.
- `rfd/0085-the-gyre-mud-setting-on-the-loot-action-shell`
  (`multiplayer-fabric-manuals`): the game content this client renders.
