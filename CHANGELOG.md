# Changelog — goodnet-handler-store

All notable changes to this plugin are listed here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions track the kernel ABI through `gn_handler_vtable_t` /
`gn_store_api_t`.

## [Unreleased]

### First-writer-wins authority ACL on wire writes

Wire-side `STORE_PUT` and `STORE_DELETE` now consult the
Noise-authenticated `sender_pk` the gnet protocol layer stamps
on every deframed envelope. Each key binds to its initial wire
writer's public key; subsequent PUT or DELETE from a different
peer is rejected with the new status code `4` /
`kStatusUnauthorized`. The original writer can update + delete
freely; ownership lapses when the owning peer deletes the key.

Loopback / in-process / test-fixture envelopes with all-zero
`sender_pk` bypass the gate — the kernel is implicitly trusted
and the `gn.store` extension callers (`put_local` / `del_local`)
have no on-the-wire sender to authenticate. The owners map is
in-memory only; SqliteStore-backed deployments lose the binding
on restart (the next first writer per key claims again).
Persisting authority across restart is a future refinement.

## [1.0.0-rc1] — 2026-05-13

### Added

- Initial release. Brings the legacy `apps/store` surface forward
  as a v1 GoodNet handler plugin.
- `gn.store` extension vtable with `put / get / query / del /
  subscribe / unsubscribe / cleanup_expired` plus the `ctx` /
  `_reserved` ABI footer. Size-prefixed per `abi-evolution.en.md`
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
- Wire contract published at `docs/contracts/store.en.md` in the
  kernel monorepo.

### Roadmap

- `SqliteStore` backend (file-backed, prepared statements).
  Operator picks the backend via plugin manifest.
- `gstore` CLI for ad-hoc lookups + bulk import / export.
- DHT backend (Kademlia over GoodNet itself).
- Redis cluster backend.
