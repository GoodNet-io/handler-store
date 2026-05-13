// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"

#include <core/util/endian.hpp>

#include <sdk/convenience.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gn::handler::store {

// ── clock ────────────────────────────────────────────────────────────────

std::uint64_t default_clock_us() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// ── MemoryStore ──────────────────────────────────────────────────────────

namespace {
/// Per-process monotonic counter for the wall-clock fallback. The
/// default `system_clock` resolution is microseconds on Linux, so
/// two consecutive puts CAN tie at the same `now()` reading. Tests
/// that pivot on `get_since` need strictly-monotonic timestamps;
/// bumping the counter on ties resolves the tie deterministically
/// without forcing every caller through a mock clock.
std::uint64_t monotonic_default_clock() noexcept {
    static std::atomic<std::uint64_t> last{0};
    std::uint64_t want = default_clock_us();
    std::uint64_t prev = last.load(std::memory_order_relaxed);
    while (true) {
        const std::uint64_t next = want > prev ? want : prev + 1;
        if (last.compare_exchange_weak(prev, next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return next;
        }
    }
}
}  // namespace

MemoryStore::MemoryStore()
    : clock_(&monotonic_default_clock) {}

MemoryStore::MemoryStore(std::uint64_t (*clock)() noexcept)
    : clock_(clock != nullptr ? clock : &monotonic_default_clock) {}

bool MemoryStore::put(std::string_view key,
                      std::span<const std::uint8_t> value,
                      std::uint64_t ttl_s,
                      std::uint8_t flags) {
    if (key.empty() || key.size() > GN_STORE_KEY_MAX_LEN) return false;
    if (value.size() > GN_STORE_VALUE_MAX_LEN)            return false;
    Entry e{
        .key          = std::string{key},
        .value        = std::vector<std::uint8_t>(value.begin(), value.end()),
        .timestamp_us = clock_(),
        .ttl_s        = ttl_s,
        .flags        = flags,
    };
    map_.insert_or_assign(std::string{key}, std::move(e));
    return true;
}

std::optional<Entry> MemoryStore::get(std::string_view key) const {
    auto it = map_.find(std::string{key});
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

std::vector<Entry>
MemoryStore::get_prefix(std::string_view prefix, std::uint32_t max_results) const {
    std::vector<Entry> out;
    out.reserve(std::min<std::size_t>(max_results, map_.size()));
    for (const auto& [k, v] : map_) {
        if (out.size() >= max_results) break;
        if (k.size() >= prefix.size() &&
            std::memcmp(k.data(), prefix.data(), prefix.size()) == 0) {
            out.push_back(v);
        }
    }
    return out;
}

std::vector<Entry>
MemoryStore::get_since(std::uint64_t since_us, std::uint32_t max_results) const {
    std::vector<Entry> out;
    out.reserve(std::min<std::size_t>(max_results, map_.size()));
    for (const auto& [_, v] : map_) {
        if (out.size() >= max_results) break;
        if (v.timestamp_us > since_us) out.push_back(v);
    }
    return out;
}

bool MemoryStore::del(std::string_view key) {
    return map_.erase(std::string{key}) > 0;
}

std::uint64_t MemoryStore::cleanup_expired(std::uint64_t now_us) {
    std::uint64_t dropped = 0;
    for (auto it = map_.begin(); it != map_.end(); ) {
        const auto& e = it->second;
        if (e.ttl_s > 0 &&
            e.timestamp_us + e.ttl_s * 1'000'000ULL <= now_us) {
            it = map_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

// ── wire framing ─────────────────────────────────────────────────────────

namespace {

constexpr std::uint8_t kStatusOk           = 0;
constexpr std::uint8_t kStatusBadSize      = 1;
constexpr std::uint8_t kStatusNotFound     = 2;
constexpr std::uint8_t kStatusBackendError = 3;

constexpr std::size_t kHeaderPut    = 24;  // req(8) + ttl(8) + flags(1) + pad(1) + key_len(2) + value_len(4)
constexpr std::size_t kHeaderGet    = 28;  // req(8) + mode(1) + pad(1) + max(2) + pad(4) + since(8) + key_len(2) + pad(2)
constexpr std::size_t kHeaderDelete = 16;  // req(8) + key_len(2) + pad(6)
constexpr std::size_t kHeaderSub    = 16;  // req(8) + mode(1) + pad(1) + key_len(2) + pad(4)
constexpr std::size_t kHeaderSync   = 20;  // req(8) + since(8) + max(2) + count(2)
constexpr std::size_t kHeaderNotify = 10;  // ts_us(8) + event(1) + pad(1)
constexpr std::size_t kHeaderResult = 12;  // req(8) + status(1) + pad(1) + entry_count(2)

/// Per-entry framing: ts(8) + ttl(8) + flags(1) + pad(1) + key_len(2) + value_len(4) + key + value
constexpr std::size_t kEntryHeaderSize = 24;

/// All multi-byte fields are big-endian per gnet-v1 wire convention.
using gn::util::write_be;
using gn::util::read_be;

void append_entry(std::vector<std::uint8_t>& buf, const Entry& e) {
    const std::size_t off = buf.size();
    buf.resize(off + kEntryHeaderSize + e.key.size() + e.value.size());
    auto* p = buf.data() + off;
    write_be<std::uint64_t>({p +  0, 8}, e.timestamp_us);
    write_be<std::uint64_t>({p +  8, 8}, e.ttl_s);
    p[16] = e.flags;
    p[17] = 0;
    write_be<std::uint16_t>({p + 18, 2}, static_cast<std::uint16_t>(e.key.size()));
    write_be<std::uint32_t>({p + 20, 4}, static_cast<std::uint32_t>(e.value.size()));
    std::memcpy(p + kEntryHeaderSize, e.key.data(), e.key.size());
    std::memcpy(p + kEntryHeaderSize + e.key.size(),
                e.value.data(), e.value.size());
}

/// Build a STORE_RESULT envelope. `entries` is empty for PUT/DELETE
/// acknowledgements; carries the matched records on GET/SYNC.
std::vector<std::uint8_t>
build_result(std::uint64_t request_id, std::uint8_t status,
             const std::vector<Entry>& entries) {
    std::vector<std::uint8_t> buf(kHeaderResult);
    write_be<std::uint64_t>({buf.data() + 0, 8}, request_id);
    buf[8] = status;
    buf[9] = 0;
    write_be<std::uint16_t>(
        {buf.data() + 10, 2}, static_cast<std::uint16_t>(entries.size()));
    for (const auto& e : entries) append_entry(buf, e);
    return buf;
}

/// Build a STORE_NOTIFY envelope.
std::vector<std::uint8_t>
build_notify(std::uint64_t timestamp_us,
             gn_store_event_t event,
             const Entry& entry) {
    std::vector<std::uint8_t> buf(kHeaderNotify);
    write_be<std::uint64_t>({buf.data() + 0, 8}, timestamp_us);
    buf[8] = static_cast<std::uint8_t>(event);
    buf[9] = 0;
    append_entry(buf, entry);
    return buf;
}

/// Decode a PUT envelope. Returns the dispatch result + the entry
/// view (borrowed against @p src).
struct PutView {
    std::uint64_t            request_id;
    std::uint64_t            ttl_s;
    std::uint8_t             flags;
    std::string_view         key;
    std::span<const std::uint8_t> value;
};

std::optional<PutView> parse_put(std::span<const std::uint8_t> src) {
    if (src.size() < kHeaderPut) return std::nullopt;
    PutView v;
    v.request_id   = read_be<std::uint64_t>(src.subspan(0, 8));
    v.ttl_s        = read_be<std::uint64_t>(src.subspan(8, 8));
    v.flags        = src[16];
    const auto kl  = read_be<std::uint16_t>(src.subspan(18, 2));
    const auto vl  = read_be<std::uint32_t>(src.subspan(20, 4));
    if (kl == 0 || kl > GN_STORE_KEY_MAX_LEN)   return std::nullopt;
    if (vl > GN_STORE_VALUE_MAX_LEN)            return std::nullopt;
    if (src.size() != kHeaderPut + kl + vl)     return std::nullopt;
    v.key   = std::string_view{
        reinterpret_cast<const char*>(src.data() + kHeaderPut), kl};
    v.value = src.subspan(kHeaderPut + kl, vl);
    return v;
}

struct GetView {
    std::uint64_t        request_id;
    gn_store_query_t     mode;
    std::uint32_t        max_results;
    std::uint64_t        since_us;
    std::string_view     key;
};

std::optional<GetView> parse_get(std::span<const std::uint8_t> src) {
    if (src.size() < kHeaderGet) return std::nullopt;
    GetView v;
    v.request_id  = read_be<std::uint64_t>(src.subspan(0, 8));
    const auto m  = src[8];
    if (m > GN_STORE_QUERY_SINCE) return std::nullopt;
    v.mode        = static_cast<gn_store_query_t>(m);
    v.max_results = read_be<std::uint16_t>(src.subspan(10, 2));
    v.since_us    = read_be<std::uint64_t>(src.subspan(16, 8));
    const auto kl = read_be<std::uint16_t>(src.subspan(24, 2));
    if (kl > GN_STORE_KEY_MAX_LEN) return std::nullopt;
    if (src.size() != kHeaderGet + kl) return std::nullopt;
    v.key = std::string_view{
        reinterpret_cast<const char*>(src.data() + kHeaderGet), kl};
    return v;
}

struct DeleteView {
    std::uint64_t    request_id;
    std::string_view key;
};

std::optional<DeleteView> parse_delete(std::span<const std::uint8_t> src) {
    if (src.size() < kHeaderDelete) return std::nullopt;
    DeleteView v;
    v.request_id  = read_be<std::uint64_t>(src.subspan(0, 8));
    const auto kl = read_be<std::uint16_t>(src.subspan(8, 2));
    if (kl == 0 || kl > GN_STORE_KEY_MAX_LEN) return std::nullopt;
    if (src.size() != kHeaderDelete + kl)     return std::nullopt;
    v.key = std::string_view{
        reinterpret_cast<const char*>(src.data() + kHeaderDelete), kl};
    return v;
}

struct SubscribeView {
    std::uint64_t        request_id;
    gn_store_query_t     mode;
    std::string_view     key;
};

std::optional<SubscribeView>
parse_subscribe(std::span<const std::uint8_t> src) {
    if (src.size() < kHeaderSub) return std::nullopt;
    SubscribeView v;
    v.request_id  = read_be<std::uint64_t>(src.subspan(0, 8));
    const auto m  = src[8];
    if (m != GN_STORE_QUERY_EXACT && m != GN_STORE_QUERY_PREFIX) {
        return std::nullopt;
    }
    v.mode        = static_cast<gn_store_query_t>(m);
    const auto kl = read_be<std::uint16_t>(src.subspan(10, 2));
    if (kl > GN_STORE_KEY_MAX_LEN) return std::nullopt;
    if (src.size() != kHeaderSub + kl) return std::nullopt;
    v.key = std::string_view{
        reinterpret_cast<const char*>(src.data() + kHeaderSub), kl};
    return v;
}

struct SyncView {
    std::uint64_t  request_id;
    std::uint64_t  since_us;
    std::uint32_t  max_results;
};

std::optional<SyncView> parse_sync(std::span<const std::uint8_t> src) {
    if (src.size() < kHeaderSync) return std::nullopt;
    SyncView v;
    v.request_id  = read_be<std::uint64_t>(src.subspan(0, 8));
    v.since_us    = read_be<std::uint64_t>(src.subspan(8, 8));
    v.max_results = read_be<std::uint16_t>(src.subspan(16, 2));
    return v;
}

bool key_matches(std::string_view k, std::string_view filter,
                 gn_store_query_t mode) {
    if (mode == GN_STORE_QUERY_PREFIX) {
        return k.size() >= filter.size() &&
            std::memcmp(k.data(), filter.data(), filter.size()) == 0;
    }
    return k == filter;
}

/// Per-handler msg_id registration.
constexpr std::uint32_t kSupportedMsgIds[] = {
    kMsgPut, kMsgGet, kMsgDelete, kMsgSubscribe, kMsgSync,
};

}  // namespace

// ── StoreHandler ─────────────────────────────────────────────────────────

StoreHandler::StoreHandler(const host_api_t* api)
    : StoreHandler(api, std::make_unique<MemoryStore>(), &default_clock_us) {}

StoreHandler::StoreHandler(const host_api_t* api,
                            std::unique_ptr<IStore> backend,
                            ClockNowUs clock)
    : api_(api),
      backend_(std::move(backend)),
      now_us_(clock)
{
    vtable_.api_size           = sizeof(gn_handler_vtable_t);
    vtable_.protocol_id        = &StoreHandler::vtable_protocol_id;
    vtable_.supported_msg_ids  = &StoreHandler::vtable_supported_msg_ids;
    vtable_.handle_message     = &StoreHandler::vtable_handle_message;

    ext_vtable_.api_size          = sizeof(gn_store_api_t);
    ext_vtable_.put               = &StoreHandler::ext_put;
    ext_vtable_.get               = &StoreHandler::ext_get;
    ext_vtable_.query             = &StoreHandler::ext_query;
    ext_vtable_.del               = &StoreHandler::ext_del;
    ext_vtable_.subscribe         = &StoreHandler::ext_subscribe;
    ext_vtable_.unsubscribe       = &StoreHandler::ext_unsubscribe;
    ext_vtable_.cleanup_expired   = &StoreHandler::ext_cleanup_expired;
    ext_vtable_.ctx               = this;
}

StoreHandler::~StoreHandler() = default;

std::size_t StoreHandler::subscription_count() const noexcept {
    std::lock_guard lk(mu_);
    return subs_.size();
}

// ── wire dispatch ────────────────────────────────────────────────────────

gn_propagation_t StoreHandler::handle_message(const gn_message_t* env) {
    if (env == nullptr || env->payload == nullptr) return GN_PROPAGATION_CONTINUE;

    const std::span<const std::uint8_t> payload{env->payload, env->payload_size};
    const auto sender = env->conn_id;

    switch (env->msg_id) {
    case kMsgPut: {
        const auto v = parse_put(payload);
        if (!v) {
            send_result(sender, 0, kStatusBadSize, {});
            return GN_PROPAGATION_CONSUMED;
        }
        Entry e_view{
            .key          = std::string{v->key},
            .value        = std::vector<std::uint8_t>(v->value.begin(), v->value.end()),
            .timestamp_us = now_us_(),
            .ttl_s        = v->ttl_s,
            .flags        = v->flags,
        };
        bool ok;
        {
            std::lock_guard lk(mu_);
            ok = backend_->put(v->key, v->value, v->ttl_s, v->flags);
        }
        send_result(sender, v->request_id,
                    ok ? kStatusOk : kStatusBackendError, {});
        if (ok) notify(e_view, GN_STORE_EVENT_PUT);
        return GN_PROPAGATION_CONSUMED;
    }

    case kMsgGet: {
        const auto v = parse_get(payload);
        if (!v) {
            send_result(sender, 0, kStatusBadSize, {});
            return GN_PROPAGATION_CONSUMED;
        }
        std::vector<Entry> hits;
        {
            std::lock_guard lk(mu_);
            switch (v->mode) {
            case GN_STORE_QUERY_EXACT:
                if (auto e = backend_->get(v->key)) hits.push_back(*e);
                break;
            case GN_STORE_QUERY_PREFIX:
                hits = backend_->get_prefix(v->key,
                    std::min(v->max_results, GN_STORE_QUERY_MAX_RESULTS));
                break;
            case GN_STORE_QUERY_SINCE:
                hits = backend_->get_since(v->since_us,
                    std::min(v->max_results, GN_STORE_QUERY_MAX_RESULTS));
                break;
            }
        }
        send_result(sender, v->request_id,
                    hits.empty() ? kStatusNotFound : kStatusOk, hits);
        return GN_PROPAGATION_CONSUMED;
    }

    case kMsgDelete: {
        const auto v = parse_delete(payload);
        if (!v) {
            send_result(sender, 0, kStatusBadSize, {});
            return GN_PROPAGATION_CONSUMED;
        }
        std::optional<Entry> snap;
        bool removed;
        {
            std::lock_guard lk(mu_);
            snap    = backend_->get(v->key);
            removed = backend_->del(v->key);
        }
        send_result(sender, v->request_id,
                    removed ? kStatusOk : kStatusNotFound, {});
        if (removed && snap) notify(*snap, GN_STORE_EVENT_DELETE);
        return GN_PROPAGATION_CONSUMED;
    }

    case kMsgSubscribe: {
        const auto v = parse_subscribe(payload);
        if (!v) {
            send_result(sender, 0, kStatusBadSize, {});
            return GN_PROPAGATION_CONSUMED;
        }
        const auto tok = next_token_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lk(mu_);
            subs_.push_back(Subscription{
                .token     = tok,
                .conn_id   = sender,
                .key       = std::string{v->key},
                .mode      = v->mode,
                .cb        = nullptr,
                .user_data = nullptr,
            });
        }
        send_result(sender, v->request_id, kStatusOk, {});
        return GN_PROPAGATION_CONSUMED;
    }

    case kMsgSync: {
        const auto v = parse_sync(payload);
        if (!v) {
            send_result(sender, 0, kStatusBadSize, {});
            return GN_PROPAGATION_CONSUMED;
        }
        std::vector<Entry> hits;
        {
            std::lock_guard lk(mu_);
            hits = backend_->get_since(v->since_us,
                std::min(v->max_results, GN_STORE_QUERY_MAX_RESULTS));
        }
        send_result(sender, v->request_id, kStatusOk, hits);
        return GN_PROPAGATION_CONSUMED;
    }

    default:
        return GN_PROPAGATION_CONTINUE;
    }
}

void StoreHandler::send_result(gn_conn_id_t conn,
                                 std::uint64_t request_id,
                                 std::uint8_t status,
                                 const std::vector<Entry>& entries) {
    if (!api_ || !api_->send || conn == 0) return;
    const auto buf = build_result(request_id, status, entries);
    (void)api_->send(api_->host_ctx, conn, kMsgResult,
                      buf.data(), buf.size());
}

void StoreHandler::notify(const Entry& entry, gn_store_event_t event) {
    /// Drain the matching subscription list under the mutex into a
    /// snapshot of (conn, in-process cb) pairs, then fire callbacks
    /// outside the lock so subscriber code can re-enter the store.
    struct Target {
        gn_conn_id_t         conn;
        gn_store_event_cb_t  cb;
        void*                user_data;
    };
    std::vector<Target> targets;
    {
        std::lock_guard lk(mu_);
        for (const auto& s : subs_) {
            if (key_matches(entry.key, s.key, s.mode)) {
                targets.push_back({s.conn_id, s.cb, s.user_data});
            }
        }
    }
    if (targets.empty()) return;

    const auto buf = build_notify(now_us_(), event, entry);
    for (const auto& t : targets) {
        if (t.cb != nullptr) {
            gn_store_entry_t view{
                .key          = entry.key.data(),
                .key_len      = entry.key.size(),
                .value        = entry.value.data(),
                .value_len    = entry.value.size(),
                .timestamp_us = entry.timestamp_us,
                .ttl_s        = entry.ttl_s,
                .flags        = entry.flags,
            };
            t.cb(t.user_data, event, &view);
        } else if (api_ && api_->send && t.conn != 0) {
            (void)api_->send(api_->host_ctx, t.conn, kMsgNotify,
                              buf.data(), buf.size());
        }
    }
}

// ── in-process surface ───────────────────────────────────────────────────

int StoreHandler::put_local(std::string_view key,
                             std::span<const std::uint8_t> value,
                             std::uint64_t ttl_s, std::uint8_t flags) {
    if (key.empty() || key.size() > GN_STORE_KEY_MAX_LEN) return -1;
    if (value.size() > GN_STORE_VALUE_MAX_LEN)            return -1;
    bool ok;
    {
        std::lock_guard lk(mu_);
        ok = backend_->put(key, value, ttl_s, flags);
    }
    if (!ok) return -2;
    Entry snap{
        .key          = std::string{key},
        .value        = std::vector<std::uint8_t>(value.begin(), value.end()),
        .timestamp_us = now_us_(),
        .ttl_s        = ttl_s,
        .flags        = flags,
    };
    notify(snap, GN_STORE_EVENT_PUT);
    return 0;
}

std::optional<Entry> StoreHandler::get_local(std::string_view key) const {
    std::lock_guard lk(mu_);
    return backend_->get(key);
}

std::vector<Entry>
StoreHandler::query_local(gn_store_query_t mode, std::string_view key,
                            std::uint64_t since_us,
                            std::uint32_t max_results) const {
    std::lock_guard lk(mu_);
    const auto cap = std::min(max_results, GN_STORE_QUERY_MAX_RESULTS);
    switch (mode) {
    case GN_STORE_QUERY_EXACT:
        if (auto e = backend_->get(key)) return {*e};
        return {};
    case GN_STORE_QUERY_PREFIX: return backend_->get_prefix(key, cap);
    case GN_STORE_QUERY_SINCE:  return backend_->get_since(since_us, cap);
    }
    return {};
}

bool StoreHandler::del_local(std::string_view key) {
    std::optional<Entry> snap;
    bool removed;
    {
        std::lock_guard lk(mu_);
        snap    = backend_->get(key);
        removed = backend_->del(key);
    }
    if (removed && snap) notify(*snap, GN_STORE_EVENT_DELETE);
    return removed;
}

std::uint64_t StoreHandler::subscribe_local(gn_store_query_t mode,
                                              std::string_view key,
                                              gn_store_event_cb_t cb,
                                              void* user_data) {
    if (cb == nullptr) return 0;
    const auto tok = next_token_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(mu_);
    subs_.push_back(Subscription{
        .token     = tok,
        .conn_id   = 0,
        .key       = std::string{key},
        .mode      = mode,
        .cb        = cb,
        .user_data = user_data,
    });
    return tok;
}

void StoreHandler::unsubscribe_local(std::uint64_t token) noexcept {
    if (token == 0) return;
    std::lock_guard lk(mu_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
        [token](const Subscription& s) { return s.token == token; }),
        subs_.end());
}

std::uint64_t StoreHandler::cleanup_expired_local() {
    std::lock_guard lk(mu_);
    return backend_->cleanup_expired(now_us_());
}

// ── vtable thunks ────────────────────────────────────────────────────────

const char* StoreHandler::vtable_protocol_id(void* /*self*/) {
    return kProtocolId;
}

void StoreHandler::vtable_supported_msg_ids(void* /*self*/,
                                              const std::uint32_t** out_ids,
                                              std::size_t* out_count) {
    *out_ids   = kSupportedMsgIds;
    *out_count = sizeof(kSupportedMsgIds) / sizeof(kSupportedMsgIds[0]);
}

gn_propagation_t
StoreHandler::vtable_handle_message(void* self, const gn_message_t* env) {
    return static_cast<StoreHandler*>(self)->handle_message(env);
}

// ── extension thunks ─────────────────────────────────────────────────────

int StoreHandler::ext_put(void* ctx, const char* k, size_t kl,
                            const std::uint8_t* v, size_t vl,
                            std::uint64_t ttl, std::uint8_t flags) {
    if (k == nullptr) return -1;
    return static_cast<StoreHandler*>(ctx)->put_local(
        {k, kl}, {v, vl}, ttl, flags);
}

int StoreHandler::ext_get(void* ctx, const char* k, size_t kl,
                            gn_store_entry_t* out) {
    if (k == nullptr || out == nullptr) return -2;
    auto* self = static_cast<StoreHandler*>(ctx);
    auto hit = self->get_local({k, kl});
    if (!hit) return -1;
    /// Caller-owned: keep a thread_local snapshot alive for the
    /// duration of the call so `out->key` / `out->value` remain
    /// valid until the caller copies. Same pattern PerConnMap uses.
    thread_local Entry snap;
    snap = std::move(*hit);
    out->key          = snap.key.data();
    out->key_len      = snap.key.size();
    out->value        = snap.value.data();
    out->value_len    = snap.value.size();
    out->timestamp_us = snap.timestamp_us;
    out->ttl_s        = snap.ttl_s;
    out->flags        = snap.flags;
    return 0;
}

int StoreHandler::ext_query(void* ctx, gn_store_query_t mode,
                              const char* k, size_t kl,
                              std::uint64_t since_us,
                              std::uint32_t max_results,
                              void (*emit)(void*, const gn_store_entry_t*),
                              void* emit_user) {
    if (emit == nullptr) return 0;
    auto* self = static_cast<StoreHandler*>(ctx);
    auto hits = self->query_local(mode,
        std::string_view{k ? k : "", kl}, since_us, max_results);
    for (const auto& e : hits) {
        gn_store_entry_t view{
            .key          = e.key.data(),
            .key_len      = e.key.size(),
            .value        = e.value.data(),
            .value_len    = e.value.size(),
            .timestamp_us = e.timestamp_us,
            .ttl_s        = e.ttl_s,
            .flags        = e.flags,
        };
        emit(emit_user, &view);
    }
    return static_cast<int>(hits.size());
}

int StoreHandler::ext_del(void* ctx, const char* k, size_t kl) {
    if (k == nullptr) return -2;
    return static_cast<StoreHandler*>(ctx)->del_local({k, kl}) ? 0 : -1;
}

uint64_t StoreHandler::ext_subscribe(void* ctx, gn_store_query_t mode,
                                       const char* k, size_t kl,
                                       gn_store_event_cb_t cb, void* user) {
    if (k == nullptr || cb == nullptr) return 0;
    return static_cast<StoreHandler*>(ctx)->subscribe_local(
        mode, {k, kl}, cb, user);
}

void StoreHandler::ext_unsubscribe(void* ctx, std::uint64_t token) {
    static_cast<StoreHandler*>(ctx)->unsubscribe_local(token);
}

uint64_t StoreHandler::ext_cleanup_expired(void* ctx) {
    return static_cast<StoreHandler*>(ctx)->cleanup_expired_local();
}

}  // namespace gn::handler::store
