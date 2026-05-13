// SPDX-License-Identifier: Apache-2.0
/// @file   plugins/handlers/store/store.hpp
/// @brief  Distributed key-value store handler. Exports the
///         `gn.store` extension per `sdk/extensions/store.h`.
///
/// Wire surface (`protocol_id = "gnet-v1"`, msg_id allocation
/// inherited from the legacy `apps/store` layer):
///
///   * 0x0600  STORE_PUT       — client → server: write
///   * 0x0601  STORE_GET       — client → server: read (exact / prefix / since)
///   * 0x0602  STORE_RESULT    — server → client: response envelope
///   * 0x0603  STORE_DELETE    — client → server: remove
///   * 0x0604  STORE_SUBSCRIBE — client → server: watch a key / prefix
///   * 0x0605  STORE_NOTIFY    — server → subscriber: change event
///   * 0x0606  STORE_SYNC      — symmetric: bulk replication
///
/// The on-wire framing is big-endian, length-prefixed binary; see
/// `docs/contracts/store.md` for the byte-layout tables.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sdk/extensions/store.h>
#include <sdk/handler.h>
#include <sdk/host_api.h>
#include <sdk/types.h>

namespace gn::handler::store {

/// On-wire identifiers for the seven STORE_* envelopes.
inline constexpr std::uint32_t kMsgPut       = 0x0600;
inline constexpr std::uint32_t kMsgGet       = 0x0601;
inline constexpr std::uint32_t kMsgResult    = 0x0602;
inline constexpr std::uint32_t kMsgDelete    = 0x0603;
inline constexpr std::uint32_t kMsgSubscribe = 0x0604;
inline constexpr std::uint32_t kMsgNotify    = 0x0605;
inline constexpr std::uint32_t kMsgSync      = 0x0606;

/// Stable protocol-id this handler binds to.
inline constexpr const char* kProtocolId = "gnet-v1";

/// In-memory representation of a stored entry. Owned strings/bytes
/// — handlers and tests copy on every backend boundary so the
/// borrowed `gn_store_entry_t` slot stays a temporary view, never
/// a long-lived pointer into backend storage.
struct Entry {
    std::string                key;
    std::vector<std::uint8_t>  value;
    std::uint64_t              timestamp_us = 0;
    std::uint64_t              ttl_s        = 0;
    std::uint8_t               flags        = 0;
};

/// Abstract backend. Reference implementations:
///   * `MemoryStore` — single-process, hash-map, in this header
///   * `SqliteStore` (planned, slice 2) — file-backed, prepared stmts
///
/// Every backend method is synchronous; the handler funnels every
/// call through a single mutex so the backend never sees concurrent
/// access. Production backends can ignore their own locking.
class IStore {
public:
    virtual ~IStore() = default;

    /// Insert or overwrite. Returns `true` on success.
    [[nodiscard]] virtual bool put(std::string_view key,
                                    std::span<const std::uint8_t> value,
                                    std::uint64_t ttl_s,
                                    std::uint8_t flags) = 0;

    /// Exact-match lookup. Returns the entry on hit, `nullopt` on miss.
    [[nodiscard]] virtual std::optional<Entry>
    get(std::string_view key) const = 0;

    /// Sweep entries whose key starts with @p prefix. Result is
    /// capped at @p max_results.
    [[nodiscard]] virtual std::vector<Entry>
    get_prefix(std::string_view prefix, std::uint32_t max_results) const = 0;

    /// Entries with `timestamp_us > since_us`. Result is capped at
    /// @p max_results. Used by SYNC.
    [[nodiscard]] virtual std::vector<Entry>
    get_since(std::uint64_t since_us, std::uint32_t max_results) const = 0;

    /// Remove the entry. Returns `true` when an entry existed.
    [[nodiscard]] virtual bool del(std::string_view key) = 0;

    /// Drop entries past their TTL. Returns the number dropped.
    [[nodiscard]] virtual std::uint64_t cleanup_expired(std::uint64_t now_us) = 0;

    /// Snapshot every live entry. Tests only; production callers
    /// use `get_prefix("", max)` to enumerate.
    [[nodiscard]] virtual std::size_t size() const = 0;
};

/// In-memory hash-map backend. Thread-safe via the handler's
/// outer mutex; the backend itself has no internal locking.
///
/// The optional ctor `clock` parameter lets tests inject a
/// deterministic time source so SINCE / TTL assertions remain
/// stable across runs. Production callers omit it and get
/// wall-clock microseconds.
class MemoryStore final : public IStore {
public:
    MemoryStore();
    explicit MemoryStore(std::uint64_t (*clock)() noexcept);

    bool                 put(std::string_view, std::span<const std::uint8_t>,
                              std::uint64_t, std::uint8_t) override;
    std::optional<Entry> get(std::string_view) const override;
    std::vector<Entry>   get_prefix(std::string_view, std::uint32_t) const override;
    std::vector<Entry>   get_since(std::uint64_t, std::uint32_t)     const override;
    bool                 del(std::string_view) override;
    std::uint64_t        cleanup_expired(std::uint64_t) override;
    std::size_t          size() const override { return map_.size(); }

private:
    std::unordered_map<std::string, Entry> map_;
    std::uint64_t (*clock_)() noexcept;
};

/// Wall-clock source. Production binds to `system_clock`; tests
/// inject a deterministic mock. Per `clock.md` §2.
using ClockNowUs = std::uint64_t (*)();

/// Default `ClockNowUs` reading microseconds from `system_clock`
/// (NOT `steady_clock` — entries cross node boundaries through SYNC
/// where the wall-clock anchor is the only stable reference).
[[nodiscard]] std::uint64_t default_clock_us() noexcept;

/// Per-subscription record kept inside the handler.
struct Subscription {
    std::uint64_t        token;       ///< stable id returned to caller
    gn_conn_id_t         conn_id;     ///< 0 for in-process; else owning conn
    std::string          key;
    gn_store_query_t     mode;
    gn_store_event_cb_t  cb;          ///< NULL for wire-side subscribers
    void*                user_data;
};

/// Store handler. Owns the backend, the subscription table, and
/// the wire dispatcher. The handler implements
/// `gn_handler_vtable_t` directly through the static thunks at the
/// bottom of this header — same shape as `HeartbeatHandler`.
class StoreHandler {
public:
    /// Default ctor required by `GN_HANDLER_PLUGIN`: spins up the
    /// reference `MemoryStore` backend. Tests use the 2-arg overload
    /// below to inject a stub backend.
    explicit StoreHandler(const host_api_t* api);

    StoreHandler(const host_api_t* api,
                 std::unique_ptr<IStore> backend,
                 ClockNowUs clock = &default_clock_us);
    ~StoreHandler();

    StoreHandler(const StoreHandler&)            = delete;
    StoreHandler& operator=(const StoreHandler&) = delete;

    /// Static metadata read by `GN_HANDLER_PLUGIN`.
    static constexpr const char*    protocol_id() noexcept { return kProtocolId; }
    static constexpr std::uint32_t  msg_id()      noexcept { return kMsgPut; }
    static constexpr std::uint8_t   priority()    noexcept { return 200; }
    static constexpr const char*    extension_name()    noexcept { return GN_EXT_STORE; }
    static constexpr std::uint32_t  extension_version() noexcept { return GN_EXT_STORE_VERSION; }

    /// Wire dispatch. Handles all 7 STORE_* msg_ids.
    [[nodiscard]] gn_propagation_t handle_message(const gn_message_t* env);
    [[nodiscard]] gn_propagation_t handle_message(const gn_message_t& env) {
        return handle_message(&env);
    }

    // ── in-process extension surface ─────────────────────────────

    [[nodiscard]] int put_local(std::string_view key,
                                std::span<const std::uint8_t> value,
                                std::uint64_t ttl_s,
                                std::uint8_t flags);

    [[nodiscard]] std::optional<Entry> get_local(std::string_view key) const;

    [[nodiscard]] std::vector<Entry>
    query_local(gn_store_query_t mode, std::string_view key,
                std::uint64_t since_us, std::uint32_t max_results) const;

    [[nodiscard]] bool del_local(std::string_view key);

    [[nodiscard]] std::uint64_t subscribe_local(gn_store_query_t mode,
                                                 std::string_view key,
                                                 gn_store_event_cb_t cb,
                                                 void* user_data);

    void unsubscribe_local(std::uint64_t token) noexcept;

    [[nodiscard]] std::uint64_t cleanup_expired_local();

    /// Vtable accessors for `GN_HANDLER_PLUGIN`.
    [[nodiscard]] const gn_handler_vtable_t& vtable() const noexcept { return vtable_; }
    [[nodiscard]] const gn_store_api_t*      extension_vtable() const noexcept { return &ext_vtable_; }

    /// Test-only: subscription count for assertions.
    [[nodiscard]] std::size_t subscription_count() const noexcept;

private:
    /// Notify every subscriber whose key matches @p entry.
    void notify(const Entry& entry, gn_store_event_t event);

    /// Send a STORE_RESULT envelope back to @p conn.
    void send_result(gn_conn_id_t conn, std::uint64_t request_id,
                     std::uint8_t status, const std::vector<Entry>& entries);

    /// Build the static vtable wired into `vtable_`.
    static const char* vtable_protocol_id(void* self);
    static void        vtable_supported_msg_ids(void* self,
                                                  const std::uint32_t** out_ids,
                                                  std::size_t* out_count);
    static gn_propagation_t vtable_handle_message(void* self,
                                                   const gn_message_t* env);

    /// Extension thunks bridging C ABI `void*` to `StoreHandler*`.
    static int      ext_put(void* ctx, const char* k, size_t kl,
                             const std::uint8_t* v, size_t vl,
                             std::uint64_t ttl, std::uint8_t flags);
    static int      ext_get(void* ctx, const char* k, size_t kl,
                             gn_store_entry_t* out);
    static int      ext_query(void* ctx, gn_store_query_t mode,
                               const char* k, size_t kl,
                               std::uint64_t since_us,
                               std::uint32_t max_results,
                               void (*emit)(void*, const gn_store_entry_t*),
                               void* emit_user);
    static int      ext_del(void* ctx, const char* k, size_t kl);
    static uint64_t ext_subscribe(void* ctx, gn_store_query_t mode,
                                   const char* k, size_t kl,
                                   gn_store_event_cb_t cb, void* user);
    static void     ext_unsubscribe(void* ctx, std::uint64_t token);
    static uint64_t ext_cleanup_expired(void* ctx);

    const host_api_t*                 api_;
    std::unique_ptr<IStore>           backend_;
    ClockNowUs                        now_us_;
    gn_handler_vtable_t               vtable_{};
    gn_store_api_t                    ext_vtable_{};

    mutable std::mutex                mu_;
    std::vector<Subscription>         subs_;
    std::atomic<std::uint64_t>        next_token_{1};
};

}  // namespace gn::handler::store
