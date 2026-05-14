# Changelog — goodnet-handler-store

All notable changes to this plugin are listed here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions track the kernel ABI through `gn_handler_vtable_t` /
`gn_store_api_t`.

## [1.0.0-rc1] — 2026-05-13

### Added

- Initial release. Brings the legacy `apps/store` surface forward
  as a v1 GoodNet handler plugin.
- `gn.store` extension vtable with `put / get / query / del /
  subscribe / unsubscribe / cleanup_expired` plus the `ctx` /
  `_reserved` ABI footer. Size-prefixed per `abi-evolution.md`
  §3, version `0x00010000`.
- `MemoryStore` reference backend — hash-map, TTL, prefix sweep,
  since-timestamp filter. Per-process monotonic clock wrapper
  guarantees strictly-increasing timestamps even when
  `system_clock` ticks coincide.
- Wire dispatcher for seven `STORE_*` envelopes under
  `protocol_id = "gnet-v1"`, `msg_id` range `0x0600..0x0606`.
  Big-endian length-prefixed binary; 256-byte key cap, 64 KiB
  value cap, 256 entries per query.
- 22 unit tests covering backend semantics, the extension
  surface, and the wire dispatcher (including a cross-conn
  wire-subscribe → notify path).
- Wire contract published at `docs/contracts/store.md` in the
  kernel monorepo.

### Roadmap

- `SqliteStore` backend (file-backed, prepared statements).
  Operator picks the backend via plugin manifest.
- `gstore` CLI for ad-hoc lookups + bulk import / export.
- DHT backend (Kademlia over GoodNet itself).
- Redis cluster backend.
