# `gn.handler.store`

Distributed key-value store handler — brings the legacy
`apps/store` surface forward as a v1 GoodNet handler plugin.
A pluggable `IStore` backend (memory reference in slice 1;
sqlite + DHT + Redis planned) sits behind a wire dispatcher that
covers seven `STORE_*` envelope types. Local callers reach the
same surface through the `gn.store` extension vtable.

## What's in slice 1

- `MemoryStore` reference backend — hash-map, TTL, prefix sweep,
  since-timestamp filter. Loses state across restart by design.
- `StoreHandler` wire dispatcher for the seven `STORE_*` msg_ids
  (0x0600..0x0606 under `protocol_id = "gnet-v1"`).
- `gn.store` extension vtable for in-process callers
  (`sdk/extensions/store.h`).
- Subscribe/notify on PUT + DELETE, both per-conn (wire) and
  per-callback (in-process).
- 22 unit tests covering backend semantics, the wire dispatcher,
  and the extension surface.

## What lands later

- Slice 2 — `SqliteStore` backend (file-backed, prepared
  statements). Operator picks the backend by plugin manifest.
- Slice 3 — `gstore` CLI helper for ad-hoc lookups.
- Slice 4 — graduate to `goodnet-io/handler-store` git submodule
  alongside `handler-heartbeat`.
- Slice N — DHT backend (Kademlia over GoodNet itself) + Redis
  cluster backend.

## Wire format

Full byte-layout tables live in
[`docs/contracts/store.md`](../../../docs/contracts/store.en.md).
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
