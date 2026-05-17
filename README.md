# `gn.handler.store`

Distributed key-value store handler — brings the legacy
`apps/store` surface forward as a v1 GoodNet handler plugin.
A pluggable `IStore` backend sits behind a wire dispatcher that
covers seven `STORE_*` envelope types. Local callers reach the
same surface through the `gn.store` extension vtable.

## What ships today

- `MemoryStore` reference backend — hash-map, TTL, prefix sweep,
  since-timestamp filter. Loses state across restart by design.
- `SqliteStore` file-backed backend — single connection, cached
  prepared statements, file or `:memory:` DB. Operator picks the
  backend via the plugin manifest's `store.backend` config.
- `StoreHandler` wire dispatcher for the seven `STORE_*` msg_ids
  (0x0600..0x0606 under `protocol_id = "gnet-v1"`).
- `gn.store` extension vtable for in-process callers
  (`sdk/extensions/store.h`).
- Subscribe/notify on PUT + DELETE, both per-conn (wire) and
  per-callback (in-process).
- First-writer-wins authority ACL: each key binds to the
  Noise-authenticated `sender_pk` of its initial wire writer;
  subsequent PUT/DELETE from a different peer is rejected with
  status `kStatusUnauthorized` (4). Loopback / in-process /
  test-fixture envelopes with all-zero sender_pk bypass the
  gate — the kernel is implicitly trusted and the `gn.store`
  extension callers have no on-the-wire sender to authenticate.
- Unit tests covering backend semantics, the wire dispatcher,
  the extension surface, and the authority ACL — same contract
  matrix for both backends so they are operator-swappable.

## On the roadmap

- `gstore` CLI helper for ad-hoc lookups + bulk import / export.
- DHT backend (Kademlia over GoodNet itself).
- Redis cluster backend.

## Wire format

Full byte-layout tables live in
[`docs/contracts/store.en.md`](../../../docs/contracts/store.en.md).
TL;DR: big-endian length-prefixed binary, 256-byte key cap, 64 KiB
value cap, 256 entry cap per query.

## Building standalone

```sh
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$GOODNET_INSTALL_DIR
cmake --build . -j
ctest
```

The CMakeLists auto-falls back to `find_package(GoodNet REQUIRED)`
when invoked outside the kernel monorepo.
