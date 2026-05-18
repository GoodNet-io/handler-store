// SPDX-License-Identifier: GPL-2.0-only
#include "sqlite_backend.hpp"

#include <cstring>
#include <new>
#include <stdexcept>

namespace gn::handler::store {

namespace {

/// Internal exception carrying a categorised `gn_result_t` from the
/// throw site (DB open, pragma exec, schema migration, statement
/// prepare) up to the `open()` factory's catch handler. Keeping the
/// category at the throw site is the only way to avoid mapping
/// every failure to `GN_ERR_OUT_OF_MEMORY` (or, worse, an opaque
/// generic code) downstream — once the exception bubbles up as a
/// plain `std::runtime_error` the category is lost.
class OpenFailure : public std::runtime_error {
public:
    OpenFailure(gn_result_t code, std::string message)
        : std::runtime_error(std::move(message)), code_{code} {}

    [[nodiscard]] gn_result_t code() const noexcept { return code_; }

private:
    gn_result_t code_;
};

constexpr const char* kCreateSchema =
    "CREATE TABLE IF NOT EXISTS store ("
    "  key          BLOB    PRIMARY KEY,"
    "  value        BLOB    NOT NULL,"
    "  timestamp_us INTEGER NOT NULL,"
    "  ttl_s        INTEGER NOT NULL DEFAULT 0,"
    "  flags        INTEGER NOT NULL DEFAULT 0"
    ");";

constexpr const char* kPragmas =
    /// WAL gives us concurrent reads against writes — the handler
    /// path itself is single-threaded but a test or admin tool
    /// that opens a second connection (read-only) for diagnostics
    /// should not block the writer.
    "PRAGMA journal_mode  = WAL;"
    "PRAGMA synchronous   = NORMAL;"  ///< full FSYNC on each commit is overkill for a KV store
    "PRAGMA foreign_keys  = ON;"
    "PRAGMA busy_timeout  = 5000;";

constexpr const char* kStmtPut =
    "INSERT INTO store(key, value, timestamp_us, ttl_s, flags) "
    "VALUES(?, ?, ?, ?, ?) "
    "ON CONFLICT(key) DO UPDATE SET "
    "  value=excluded.value, timestamp_us=excluded.timestamp_us, "
    "  ttl_s=excluded.ttl_s, flags=excluded.flags;";

constexpr const char* kStmtGet =
    "SELECT key, value, timestamp_us, ttl_s, flags "
    "FROM store WHERE key = ?;";

/// `||` is sqlite's string concat; binding the suffix as the next
/// codepoint after the prefix is the canonical no-end-bound prefix
/// scan. `?2` is `prefix || char(255-byte-sentinel)` — except sqlite
/// has no clean way to express that for BLOB, so we drive the
/// upper bound through a second bind that carries `prefix +
/// '\xff' * pad`. The `LIMIT` is supplied by the caller.
constexpr const char* kStmtPrefix =
    "SELECT key, value, timestamp_us, ttl_s, flags "
    "FROM store WHERE key >= ? AND key < ? "
    "ORDER BY key LIMIT ?;";

constexpr const char* kStmtDel =
    "DELETE FROM store WHERE key = ?;";

constexpr const char* kStmtSince =
    "SELECT key, value, timestamp_us, ttl_s, flags "
    "FROM store WHERE timestamp_us > ? "
    "ORDER BY timestamp_us LIMIT ?;";

constexpr const char* kStmtCleanup =
    /// `ttl_s = 0` means permanent — leave those alone. For the
    /// rest, drop when `timestamp_us + ttl_s * 1_000_000 <= now`.
    "DELETE FROM store WHERE ttl_s > 0 "
    "AND (timestamp_us + ttl_s * 1000000) <= ?;";

constexpr const char* kStmtCount =
    "SELECT COUNT(*) FROM store;";

/// Build the upper-bound BLOB for a prefix scan by padding with
/// 0xFF bytes up to `GN_STORE_KEY_MAX_LEN + 1` total length. The
/// previous shape — `prefix || 0xFF` (single byte appended) —
/// missed keys of the form `prefix || 0xFF || <anything>`,
/// because BLOB comparison treats the longer string as greater
/// when the shared prefix is equal. A stored key
/// `abc\xff\x00` (5 bytes) compared against the bound `abc\xff`
/// (4 bytes) returns "stored > bound" and falls outside the
/// `key >= ? AND key < ?` range, even though `abc` is its
/// prefix. Padding to one byte beyond the configured key cap
/// guarantees the bound exceeds every valid key in the prefix
/// family.
std::vector<std::uint8_t>
prefix_upper_bound(std::string_view prefix) {
    std::vector<std::uint8_t> out(prefix.begin(), prefix.end());
    if (out.size() < static_cast<std::size_t>(GN_STORE_KEY_MAX_LEN) + 1) {
        out.resize(static_cast<std::size_t>(GN_STORE_KEY_MAX_LEN) + 1, 0xFF);
    } else {
        /// Prefix already longer than the cap — no valid key can
        /// match. Push a sentinel byte so the bind still receives
        /// a non-empty BLOB and the query trivially returns zero
        /// rows through the LIMIT path.
        out.push_back(0xFF);
    }
    return out;
}

}  // namespace

std::expected<std::unique_ptr<SqliteStore>, OpenError>
SqliteStore::open(const std::string& db_path) {
    return open(db_path, &monotonic_default_clock_us);
}

std::expected<std::unique_ptr<SqliteStore>, OpenError>
SqliteStore::open(const std::string& db_path,
                  std::uint64_t (*clock)() noexcept) {
    /// Catch ladder maps each throw category to a stable
    /// `gn_result_t` code so the production plugin path can branch
    /// on the failure category. The previous shape coalesced every
    /// exception into a single string (and a hypothetical kernel
    /// adapter mapping that to `GN_ERR_OUT_OF_MEMORY` would be
    /// wrong — DB-open / pragma / schema / FS-permission failures
    /// are state errors, not memory exhaustion). `OpenFailure`
    /// carries the category from the ctor throw site to here;
    /// `std::bad_alloc` is the genuine OOM signal; everything
    /// else is a true escapee that we surface as
    /// `GN_ERR_INTERNAL`.
    try {
        return std::unique_ptr<SqliteStore>(new SqliteStore(db_path, clock));
    } catch (const OpenFailure& e) {
        return std::unexpected<OpenError>({e.code(), e.what()});
    } catch (const std::bad_alloc& e) {
        return std::unexpected<OpenError>(
            {GN_ERR_OUT_OF_MEMORY, e.what()});
    } catch (const std::exception& e) {
        return std::unexpected<OpenError>(
            {GN_ERR_INTERNAL, e.what()});
    }
}

SqliteStore::SqliteStore(const std::string& db_path)
    : SqliteStore(db_path, &monotonic_default_clock_us) {}

SqliteStore::SqliteStore(const std::string& db_path,
                          std::uint64_t (*clock)() noexcept)
    : clock_(clock != nullptr ? clock : &monotonic_default_clock_us) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        const std::string err =
            db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        /// DB-open failure: bad path, directory missing, FS
        /// permission denied, file locked by another process,
        /// disk full on initial create. None of these are memory
        /// exhaustion — they are environmental / state errors.
        throw OpenFailure(GN_ERR_INVALID_STATE, "SqliteStore: " + err);
    }
    char* err = nullptr;
    if (sqlite3_exec(db_, kPragmas, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "pragma exec failed";
        sqlite3_free(err);
        sqlite3_close(db_);
        db_ = nullptr;
        /// PRAGMA exec failure: most often the DB file exists but
        /// is locked or corrupt (WAL mode toggle on a stale
        /// journal, foreign_keys flip on a half-migrated schema).
        /// Same state-error category as the open path.
        throw OpenFailure(GN_ERR_INVALID_STATE, "SqliteStore: " + msg);
    }
    create_schema();
    prepare_statements();
}

SqliteStore::~SqliteStore() {
    finalize_statements();
    if (db_) (void)sqlite3_close(db_);
}

void SqliteStore::create_schema() {
    char* err = nullptr;
    if (sqlite3_exec(db_, kCreateSchema, nullptr, nullptr, &err)
        != SQLITE_OK) {
        const std::string msg = err ? err : "create_schema failed";
        sqlite3_free(err);
        /// Schema migration failure: existing DB carries an
        /// incompatible schema, or the kernel saw an `IO_ERR`
        /// back from sqlite (disk full, FS read-only). State /
        /// environmental error, never OOM.
        throw OpenFailure(GN_ERR_INVALID_STATE, "SqliteStore: " + msg);
    }
}

void SqliteStore::prepare_statements() {
    /// Compact macro keeps the seven prepare calls grouped without
    /// hiding which statement maps to which slot.
    auto prep = [this](sqlite3_stmt*& slot, const char* sql) {
        if (sqlite3_prepare_v2(db_, sql, -1, &slot, nullptr) != SQLITE_OK) {
            /// `sqlite3_prepare_v2` fails when the SQL is invalid
            /// against the current schema (e.g. a future migration
            /// added a column that this build expects but the DB
            /// file lacks). State error — same category as
            /// schema migration.
            throw OpenFailure(GN_ERR_INVALID_STATE,
                std::string{"SqliteStore: prepare failed: "}
                + sqlite3_errmsg(db_));
        }
    };
    prep(stmt_put_,     kStmtPut);
    prep(stmt_get_,     kStmtGet);
    prep(stmt_prefix_,  kStmtPrefix);
    prep(stmt_del_,     kStmtDel);
    prep(stmt_since_,   kStmtSince);
    prep(stmt_cleanup_, kStmtCleanup);
    prep(stmt_count_,   kStmtCount);
}

void SqliteStore::finalize_statements() noexcept {
    sqlite3_stmt** slots[] = {
        &stmt_put_, &stmt_get_, &stmt_prefix_, &stmt_del_,
        &stmt_since_, &stmt_cleanup_, &stmt_count_,
    };
    for (auto* s : slots) {
        if (*s != nullptr) {
            (void)sqlite3_finalize(*s);
            *s = nullptr;
        }
    }
}

Entry SqliteStore::row_to_entry(sqlite3_stmt* stmt) const {
    Entry e;
    const auto* key_blob = sqlite3_column_blob(stmt, 0);
    const auto  key_len  = static_cast<std::size_t>(
        sqlite3_column_bytes(stmt, 0));
    e.key.assign(static_cast<const char*>(key_blob), key_len);

    const auto* val_blob = sqlite3_column_blob(stmt, 1);
    const auto  val_len  = static_cast<std::size_t>(
        sqlite3_column_bytes(stmt, 1));
    e.value.assign(
        static_cast<const std::uint8_t*>(val_blob),
        static_cast<const std::uint8_t*>(val_blob) + val_len);

    e.timestamp_us = static_cast<std::uint64_t>(
        sqlite3_column_int64(stmt, 2));
    e.ttl_s        = static_cast<std::uint64_t>(
        sqlite3_column_int64(stmt, 3));
    e.flags        = static_cast<std::uint8_t>(
        sqlite3_column_int(stmt, 4) & 0xFF);
    return e;
}

bool SqliteStore::put(std::string_view key,
                       std::span<const std::uint8_t> value,
                       std::uint64_t ttl_s,
                       std::uint8_t flags) {
    if (key.empty() || key.size() > GN_STORE_KEY_MAX_LEN) return false;
    if (value.size() > GN_STORE_VALUE_MAX_LEN)            return false;
    std::lock_guard lk(mu_);
    sqlite3_reset(stmt_put_);
    sqlite3_clear_bindings(stmt_put_);
    sqlite3_bind_blob(stmt_put_, 1, key.data(),
        static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt_put_, 2, value.data(),
        static_cast<int>(value.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_put_, 3, static_cast<sqlite3_int64>(clock_()));
    sqlite3_bind_int64(stmt_put_, 4, static_cast<sqlite3_int64>(ttl_s));
    sqlite3_bind_int(stmt_put_, 5, static_cast<int>(flags));
    return sqlite3_step(stmt_put_) == SQLITE_DONE;
}

std::optional<Entry> SqliteStore::get(std::string_view key) const {
    std::lock_guard lk(mu_);
    sqlite3_reset(stmt_get_);
    sqlite3_clear_bindings(stmt_get_);
    sqlite3_bind_blob(stmt_get_, 1, key.data(),
        static_cast<int>(key.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_get_) != SQLITE_ROW) return std::nullopt;
    return row_to_entry(stmt_get_);
}

std::vector<Entry>
SqliteStore::get_prefix(std::string_view prefix,
                          std::uint32_t max_results) const {
    std::vector<Entry> out;
    const auto cap = std::min(max_results, GN_STORE_QUERY_MAX_RESULTS);
    std::lock_guard lk(mu_);
    const auto upper = prefix_upper_bound(prefix);
    sqlite3_reset(stmt_prefix_);
    sqlite3_clear_bindings(stmt_prefix_);
    sqlite3_bind_blob(stmt_prefix_, 1, prefix.data(),
        static_cast<int>(prefix.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt_prefix_, 2, upper.data(),
        static_cast<int>(upper.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_prefix_, 3, static_cast<int>(cap));
    while (sqlite3_step(stmt_prefix_) == SQLITE_ROW) {
        out.push_back(row_to_entry(stmt_prefix_));
        if (out.size() >= cap) break;
    }
    return out;
}

std::vector<Entry>
SqliteStore::get_since(std::uint64_t since_us,
                         std::uint32_t max_results) const {
    std::vector<Entry> out;
    const auto cap = std::min(max_results, GN_STORE_QUERY_MAX_RESULTS);
    std::lock_guard lk(mu_);
    sqlite3_reset(stmt_since_);
    sqlite3_clear_bindings(stmt_since_);
    sqlite3_bind_int64(stmt_since_, 1, static_cast<sqlite3_int64>(since_us));
    sqlite3_bind_int(stmt_since_, 2, static_cast<int>(cap));
    while (sqlite3_step(stmt_since_) == SQLITE_ROW) {
        out.push_back(row_to_entry(stmt_since_));
        if (out.size() >= cap) break;
    }
    return out;
}

bool SqliteStore::del(std::string_view key) {
    std::lock_guard lk(mu_);
    sqlite3_reset(stmt_del_);
    sqlite3_clear_bindings(stmt_del_);
    sqlite3_bind_blob(stmt_del_, 1, key.data(),
        static_cast<int>(key.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_del_) != SQLITE_DONE) return false;
    return sqlite3_changes(db_) > 0;
}

std::uint64_t SqliteStore::cleanup_expired(std::uint64_t now_us) {
    std::lock_guard lk(mu_);
    sqlite3_reset(stmt_cleanup_);
    sqlite3_clear_bindings(stmt_cleanup_);
    sqlite3_bind_int64(stmt_cleanup_, 1, static_cast<sqlite3_int64>(now_us));
    if (sqlite3_step(stmt_cleanup_) != SQLITE_DONE) return 0;
    return static_cast<std::uint64_t>(sqlite3_changes(db_));
}

std::size_t SqliteStore::size() const {
    std::lock_guard lk(mu_);
    sqlite3_reset(stmt_count_);
    if (sqlite3_step(stmt_count_) != SQLITE_ROW) return 0;
    return static_cast<std::size_t>(sqlite3_column_int64(stmt_count_, 0));
}

}  // namespace gn::handler::store
