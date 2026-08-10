# native store plane

Goal: port the store plane to native. It keeps a local SQLite WAL primary and an async
FoundationDB replica. It runs over Eclipse iceoryx.

State: the Elixir prototype works. `Weft.Actor.Store.Replicated` and `.Replicator` pass the
three FoundationDB tests against a live FoundationDB.

Next: port the store plane to a native process over iceoryx. This needs a native C++ and
iceoryx toolchain, which we do not have here yet.
