// SPDX-License-Identifier: GPL-2.0-only
/// @file   plugins/handlers/store/sqlite_backend.hpp
/// @brief  File-backed IStore using SQLite.
///
/// One table `store(key BLOB PRIMARY KEY, value BLOB, timestamp_us
/// INTEGER, ttl_s INTEGER, flags INTEGER)`. Prepared statements
/// cached on the connection per legacy `apps/store/sqlite_backend.cpp`
/// — handler dispatch hits sqlite at the rate the wire delivers, so
/// statement parsing per request is a measurable hot path.
///
/// Production deployments file the DB at `~/.local/share/goodnet/
/// store.sqlite3` or wherever the manifest's `store.db_path` config
/// resolves to. Tests pass `":memory:"` for full isolation.

#pragma once

#include "store.hpp"

#include <sdk/types.h>

#include <sqlite3.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gn::handler::store {

/// Categorised failure from `SqliteStore::open`. The `code` field
/// carries a stable `gn_result_t` enumerator so the production
/// plugin path can branch on the failure category without parsing
/// the human-readable `message` — DB-open / schema-migration /
/// FS-permission errors land on `GN_ERR_INVALID_STATE` (closest
/// match in `sdk/types.h`; there is no dedicated IO_ERROR code),
/// genuine memory exhaustion lands on `GN_ERR_OUT_OF_MEMORY`,
/// every other escaped exception lands on `GN_ERR_INTERNAL`.
struct OpenError {
    gn_result_t code;
    std::string message;
};

/// SQLite-backed IStore. Single connection, single mutex — the
/// handler's outer mutex already serialises calls, but the
/// backend keeps its own lock as defence-in-depth so a future
/// caller that skips the handler doesn't see torn statement state.
class SqliteStore final : public IStore {
public:
    /// Open or create the SQLite-backed store at @p db_path. The
    /// factory wraps the throwing constructor below and surfaces
    /// `sqlite3_open` / schema-migration / FS-permission failures
    /// as `std::unexpected<OpenError>` carrying both a categorised
    /// `gn_result_t` and the underlying diagnostic — the
    /// production plugin path (which cannot rely on exception
    /// propagation across the C ABI boundary) can fail closed
    /// cleanly and surface a stable code to the caller.
    [[nodiscard]] static std::expected<std::unique_ptr<SqliteStore>, OpenError>
        open(const std::string& db_path);

    [[nodiscard]] static std::expected<std::unique_ptr<SqliteStore>, OpenError>
        open(const std::string& db_path,
             std::uint64_t (*clock)() noexcept);

    /// @param db_path File path or `":memory:"`. The file is
    ///                created on open if absent.
    /// @throws std::runtime_error on `sqlite3_open` failure or
    ///                schema migration error. Prefer the `open()`
    ///                factory above for production paths.
    explicit SqliteStore(const std::string& db_path);

    /// Optional clock injection for tests. Production callers omit
    /// it and get the monotonic wall-clock wrapper.
    SqliteStore(const std::string& db_path,
                std::uint64_t (*clock)() noexcept);

    ~SqliteStore() override;

    SqliteStore(const SqliteStore&)            = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    bool                 put(std::string_view, std::span<const std::uint8_t>,
                              std::uint64_t, std::uint8_t) override;
    std::optional<Entry> get(std::string_view) const override;
    std::vector<Entry>   get_prefix(std::string_view, std::uint32_t) const override;
    std::vector<Entry>   get_since(std::uint64_t, std::uint32_t)     const override;
    bool                 del(std::string_view) override;
    std::uint64_t        cleanup_expired(std::uint64_t) override;
    std::size_t          size() const override;

private:
    /// Create the `store` table if absent. Idempotent; safe to
    /// call against an existing DB carrying an older schema (the
    /// schema is locked at v1.0; future migrations bump the user
    /// version + run the migration here).
    void create_schema();

    /// Prepare every cached statement. Called once at open and
    /// re-called after `sqlite3_close` + reopen in error-recovery
    /// paths (not exercised today).
    void prepare_statements();

    /// Finalise every prepared statement. Called in the dtor and
    /// during error-recovery.
    void finalize_statements() noexcept;

    /// Translate one sqlite row into an Entry. Column order
    /// matches the SELECT statements below.
    Entry row_to_entry(sqlite3_stmt* stmt) const;

    sqlite3*       db_ = nullptr;

    sqlite3_stmt*  stmt_put_     = nullptr;
    sqlite3_stmt*  stmt_get_     = nullptr;
    sqlite3_stmt*  stmt_prefix_  = nullptr;
    sqlite3_stmt*  stmt_del_     = nullptr;
    sqlite3_stmt*  stmt_since_   = nullptr;
    sqlite3_stmt*  stmt_cleanup_ = nullptr;
    sqlite3_stmt*  stmt_count_   = nullptr;

    std::uint64_t (*clock_)() noexcept;
    mutable std::mutex mu_;
};

}  // namespace gn::handler::store
