// SPDX-License-Identifier: Apache-2.0
/// @file   plugins/handlers/store/tests/test_store.cpp
/// @brief  StoreHandler — memory backend semantics + wire dispatch +
///         extension surface, with deterministic clock.

#include <gtest/gtest.h>

#include <store.hpp>

#include <core/util/endian.hpp>
#include <sdk/cpp/test/stub_host.hpp>
#include <sdk/extensions/store.h>
#include <sdk/host_api.h>
#include <sdk/types.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/// gtest's ASSERT_TRUE on an optional does prove `has_value()` for
/// reading code but clang-tidy's `bugprone-unchecked-optional-access`
/// cannot model that through a macro expansion. Suppress the check
/// for the whole test TU rather than litter every assertion with an
/// inline NOLINT — same pattern `tests/unit/util/test_uri.cpp`
/// already uses.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace gn::handler::store {
namespace {

using StubHost = ::gn::sdk::test::HandlerStub;

inline host_api_t make_stub_api(StubHost& h) noexcept {
    return ::gn::sdk::test::make_handler_host_api(h);
}

/// Deterministic clock for TTL / since tests.
std::atomic<std::uint64_t> g_clock{0};
std::uint64_t mock_clock() { return g_clock.load(std::memory_order_acquire); }
void clock_set(std::uint64_t v) { g_clock.store(v, std::memory_order_release); }

/// Build a StoreHandler with a memory backend + mock clock.
auto make_handler(const host_api_t* api) {
    clock_set(1'000'000);  // reset for each test
    return std::make_unique<StoreHandler>(
        api, std::make_unique<MemoryStore>(), &mock_clock);
}

// ── MemoryStore behaviour ────────────────────────────────────────────────

TEST(MemoryStore_PutGet, RoundtripsValue) {
    MemoryStore s;
    const std::vector<std::uint8_t> v{1, 2, 3, 4};
    ASSERT_TRUE(s.put("key", v, 0, 0));
    auto hit = s.get("key");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value().key, "key");
    EXPECT_EQ(hit.value().value, v);
}

TEST(MemoryStore_PutGet, RejectsEmptyKey) {
    MemoryStore s;
    EXPECT_FALSE(s.put("", {}, 0, 0));
}

TEST(MemoryStore_PutGet, RejectsOversizedKey) {
    MemoryStore s;
    std::string huge(GN_STORE_KEY_MAX_LEN + 1, 'x');
    EXPECT_FALSE(s.put(huge, {}, 0, 0));
}

TEST(MemoryStore_PutGet, RejectsOversizedValue) {
    MemoryStore s;
    std::vector<std::uint8_t> huge(GN_STORE_VALUE_MAX_LEN + 1);
    EXPECT_FALSE(s.put("k", huge, 0, 0));
}

TEST(MemoryStore_PutGet, MissReturnsNullopt) {
    MemoryStore s;
    EXPECT_FALSE(s.get("missing").has_value());
}

TEST(MemoryStore_PutGet, OverwriteKeepsLatestValue) {
    MemoryStore s;
    ASSERT_TRUE(s.put("k", std::vector<std::uint8_t>{1}, 0, 0));
    ASSERT_TRUE(s.put("k", std::vector<std::uint8_t>{2, 3}, 0, 0));
    auto hit = s.get("k");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value().value, (std::vector<std::uint8_t>{2, 3}));
}

TEST(MemoryStore_Prefix, MatchesByPrefix) {
    MemoryStore s;
    ASSERT_TRUE(s.put("peer/alice", std::vector<std::uint8_t>{0xa}, 0, 0));
    ASSERT_TRUE(s.put("peer/bob",   std::vector<std::uint8_t>{0xb}, 0, 0));
    ASSERT_TRUE(s.put("svc/chat",   std::vector<std::uint8_t>{0xc}, 0, 0));
    auto hits = s.get_prefix("peer/", 256);
    EXPECT_EQ(hits.size(), 2u);
}

TEST(MemoryStore_Prefix, EmptyPrefixReturnsAll) {
    MemoryStore s;
    ASSERT_TRUE(s.put("a", std::vector<std::uint8_t>{1}, 0, 0));
    ASSERT_TRUE(s.put("b", std::vector<std::uint8_t>{2}, 0, 0));
    ASSERT_TRUE(s.put("c", std::vector<std::uint8_t>{3}, 0, 0));
    EXPECT_EQ(s.get_prefix("", 256).size(), 3u);
}

TEST(MemoryStore_Prefix, RespectsMaxResults) {
    MemoryStore s;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(s.put("k" + std::to_string(i), std::vector<std::uint8_t>{1}, 0, 0));
    }
    EXPECT_EQ(s.get_prefix("k", 3).size(), 3u);
}

TEST(MemoryStore_Delete, RemovesEntry) {
    MemoryStore s;
    ASSERT_TRUE(s.put("k", std::vector<std::uint8_t>{1}, 0, 0));
    EXPECT_TRUE(s.del("k"));
    EXPECT_FALSE(s.get("k").has_value());
}

TEST(MemoryStore_Delete, MissReturnsFalse) {
    MemoryStore s;
    EXPECT_FALSE(s.del("nope"));
}

TEST(MemoryStore_Cleanup, DropsExpired) {
    MemoryStore s;
    clock_set(1'000'000);  // = 1s
    /// Inserting through MemoryStore::put stamps the entry with the
    /// REAL wall clock; bypass it by calling cleanup with a "now"
    /// far in the future so the TTL test stays deterministic.
    ASSERT_TRUE(s.put("temp", std::vector<std::uint8_t>{1}, 1 /*s*/, 0));
    ASSERT_TRUE(s.put("perm", std::vector<std::uint8_t>{2}, 0,         0));
    auto temp_entry = s.get("temp");
    ASSERT_TRUE(temp_entry.has_value());
    const std::uint64_t future = temp_entry.value().timestamp_us + 5'000'000ULL;
    EXPECT_EQ(s.cleanup_expired(future), 1u);
    EXPECT_FALSE(s.get("temp").has_value());
    EXPECT_TRUE (s.get("perm").has_value());
}

TEST(MemoryStore_Since, FiltersByTimestamp) {
    MemoryStore s;
    ASSERT_TRUE(s.put("a", std::vector<std::uint8_t>{1}, 0, 0));
    auto a_entry = s.get("a");
    ASSERT_TRUE(a_entry.has_value());
    const auto a_ts = a_entry.value().timestamp_us;
    ASSERT_TRUE(s.put("b", std::vector<std::uint8_t>{2}, 0, 0));
    auto hits = s.get_since(a_ts, 256);
    /// Only "b" was written strictly after "a".
    ASSERT_FALSE(hits.empty());
    bool seen_b = false;
    for (const auto& e : hits) if (e.key == "b") seen_b = true;
    EXPECT_TRUE(seen_b);
}

// ── extension surface (in-process) ───────────────────────────────────────

TEST(StoreExtension, PutGetRoundtripsThroughLocalAPI) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    const std::vector<std::uint8_t> value{1, 2, 3};
    ASSERT_EQ(h->put_local("k", value, 0, 0), 0);
    auto hit = h->get_local("k");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value().value, value);
}

TEST(StoreExtension, SubscribeFiresOnPut) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    struct Capture {
        gn_store_event_t last_event{};
        std::string      last_key;
        bool             called = false;
    } cap;
    const auto tok = h->subscribe_local(
        GN_STORE_QUERY_PREFIX, "peer/",
        [](void* u, gn_store_event_t ev, const gn_store_entry_t* e) {
            auto* c = static_cast<Capture*>(u);
            c->last_event = ev;
            c->last_key.assign(e->key, e->key_len);
            c->called = true;
        }, &cap);
    ASSERT_GT(tok, 0u);

    ASSERT_EQ(h->put_local("peer/alice", std::vector<std::uint8_t>{1}, 0, 0), 0);
    EXPECT_TRUE(cap.called);
    EXPECT_EQ(cap.last_event, GN_STORE_EVENT_PUT);
    EXPECT_EQ(cap.last_key, "peer/alice");
}

TEST(StoreExtension, SubscribeFiresOnDelete) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    ASSERT_EQ(h->put_local("svc/chat", std::vector<std::uint8_t>{1}, 0, 0), 0);

    struct Capture { gn_store_event_t ev{}; bool called = false; } cap;
    (void)h->subscribe_local(GN_STORE_QUERY_EXACT, "svc/chat",
        [](void* u, gn_store_event_t e, const gn_store_entry_t*) {
            auto* c = static_cast<Capture*>(u);
            c->ev = e;
            c->called = true;
        }, &cap);

    EXPECT_TRUE(h->del_local("svc/chat"));
    EXPECT_TRUE(cap.called);
    EXPECT_EQ(cap.ev, GN_STORE_EVENT_DELETE);
}

TEST(StoreExtension, UnsubscribeStopsNotifications) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    struct Capture { int count = 0; } cap;
    const auto tok = h->subscribe_local(
        GN_STORE_QUERY_PREFIX, "p/",
        [](void* u, gn_store_event_t, const gn_store_entry_t*) {
            static_cast<Capture*>(u)->count++;
        }, &cap);

    ASSERT_EQ(h->put_local("p/1", std::vector<std::uint8_t>{1}, 0, 0), 0);
    h->unsubscribe_local(tok);
    ASSERT_EQ(h->put_local("p/2", std::vector<std::uint8_t>{2}, 0, 0), 0);
    EXPECT_EQ(cap.count, 1);
    EXPECT_EQ(h->subscription_count(), 0u);
}

// ── wire-protocol dispatch ───────────────────────────────────────────────

namespace wire {
    /// Mirror of the framing helpers in store.cpp — duplicated here
    /// rather than exported because the tests are the authoritative
    /// consumer of the wire spec, so a future framing change must
    /// fail both sites and produce visible test diffs.
    using gn::util::write_be;

    std::vector<std::uint8_t>
    put(std::uint64_t req, std::uint64_t ttl, std::uint8_t flags,
        std::string_view key, std::span<const std::uint8_t> value) {
        std::vector<std::uint8_t> b(24 + key.size() + value.size());
        write_be<std::uint64_t>({b.data() + 0, 8}, req);
        write_be<std::uint64_t>({b.data() + 8, 8}, ttl);
        b[16] = flags;
        write_be<std::uint16_t>(
            {b.data() + 18, 2}, static_cast<std::uint16_t>(key.size()));
        write_be<std::uint32_t>(
            {b.data() + 20, 4}, static_cast<std::uint32_t>(value.size()));
        std::memcpy(b.data() + 24, key.data(), key.size());
        std::memcpy(b.data() + 24 + key.size(), value.data(), value.size());
        return b;
    }

    std::vector<std::uint8_t>
    get(std::uint64_t req, gn_store_query_t mode, std::uint32_t max,
        std::uint64_t since_us, std::string_view key) {
        std::vector<std::uint8_t> b(28 + key.size());
        write_be<std::uint64_t>({b.data() + 0, 8}, req);
        b[8] = static_cast<std::uint8_t>(mode);
        write_be<std::uint16_t>(
            {b.data() + 10, 2}, static_cast<std::uint16_t>(max));
        write_be<std::uint64_t>({b.data() + 16, 8}, since_us);
        write_be<std::uint16_t>(
            {b.data() + 24, 2}, static_cast<std::uint16_t>(key.size()));
        std::memcpy(b.data() + 28, key.data(), key.size());
        return b;
    }
}  // namespace wire

gn_message_t make_env(std::uint32_t msg_id, gn_conn_id_t conn,
                       std::span<const std::uint8_t> payload) {
    gn_message_t e{};
    e.msg_id       = msg_id;
    e.conn_id      = conn;
    e.payload      = payload.data();
    e.payload_size = payload.size();
    return e;
}

TEST(StoreWire, PutFollowedByGetReturnsValue) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    const auto put_bytes = wire::put(1, 0, 0, "k",
        std::vector<std::uint8_t>{1, 2, 3});
    auto env_put = make_env(kMsgPut, 42, put_bytes);
    EXPECT_EQ(h->handle_message(&env_put), GN_PROPAGATION_CONSUMED);

    /// One STORE_RESULT envelope on the wire (the ACK).
    EXPECT_EQ(host.send_calls.load(), 1);
    {
        std::lock_guard lk(host.mu);
        EXPECT_EQ(host.sent_msg_ids[0], kMsgResult);
        EXPECT_EQ(host.sent_conns[0],   42u);
    }
}

TEST(StoreWire, MalformedPutEmitsErrorResult) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    /// Payload too small for the header — handler must answer with
    /// a status=BadSize result, not silently drop.
    std::vector<std::uint8_t> tiny{0x01, 0x02};
    auto env = make_env(kMsgPut, 7, tiny);
    EXPECT_EQ(h->handle_message(&env), GN_PROPAGATION_CONSUMED);
    EXPECT_EQ(host.send_calls.load(), 1);
}

TEST(StoreWire, GetMissEmitsNotFoundStatus) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    const auto get_bytes = wire::get(11, GN_STORE_QUERY_EXACT, 0, 0, "missing");
    auto env = make_env(kMsgGet, 99, get_bytes);
    EXPECT_EQ(h->handle_message(&env), GN_PROPAGATION_CONSUMED);
    EXPECT_EQ(host.send_calls.load(), 1);
    /// Status byte sits at offset 8 of the STORE_RESULT envelope.
    {
        std::lock_guard lk(host.mu);
        ASSERT_EQ(host.sent_payloads.size(), 1u);
        EXPECT_EQ(host.sent_payloads[0][8], 2u /* kStatusNotFound */);
    }
}

TEST(StoreWire, UnknownMsgIdPropagates) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    std::vector<std::uint8_t> body{0xff, 0xee};
    auto env = make_env(0x9999, 5, body);
    /// Unrelated msg_id — handler MUST let the dispatch chain continue.
    EXPECT_EQ(h->handle_message(&env), GN_PROPAGATION_CONTINUE);
    EXPECT_EQ(host.send_calls.load(), 0);
}

TEST(StoreWire, SubscribeOverWireFiresNotifyOnPut) {
    StubHost host;
    auto api = make_stub_api(host);
    auto h = make_handler(&api);

    /// Subscribe conn=42 to prefix "peer/".
    std::vector<std::uint8_t> sub(16 + 5);
    gn::util::write_be<std::uint64_t>({sub.data() + 0, 8}, 1ULL);  // req
    sub[8] = static_cast<std::uint8_t>(GN_STORE_QUERY_PREFIX);
    gn::util::write_be<std::uint16_t>(
        {sub.data() + 10, 2}, static_cast<std::uint16_t>(5));
    std::memcpy(sub.data() + 16, "peer/", 5);
    auto env_sub = make_env(kMsgSubscribe, 42, sub);
    EXPECT_EQ(h->handle_message(&env_sub), GN_PROPAGATION_CONSUMED);

    /// Drive a PUT from a DIFFERENT conn — subscriber must receive
    /// a STORE_NOTIFY on conn 42 even though it wasn't the writer.
    const auto put_bytes = wire::put(2, 0, 0, "peer/alice",
        std::vector<std::uint8_t>{0xa});
    auto env_put = make_env(kMsgPut, 7, put_bytes);
    EXPECT_EQ(h->handle_message(&env_put), GN_PROPAGATION_CONSUMED);

    /// One ACK for the subscribe, one ACK for the put, one notify
    /// to conn 42 → 3 total wire frames.
    EXPECT_EQ(host.send_calls.load(), 3);
    std::lock_guard lk(host.mu);
    int notify_count = 0;
    int notify_to_42 = 0;
    for (std::size_t i = 0; i < host.sent_msg_ids.size(); ++i) {
        if (host.sent_msg_ids[i] == kMsgNotify) {
            ++notify_count;
            if (host.sent_conns[i] == 42u) ++notify_to_42;
        }
    }
    EXPECT_EQ(notify_count, 1);
    EXPECT_EQ(notify_to_42, 1);
}

}  // namespace
}  // namespace gn::handler::store

// NOLINTEND(bugprone-unchecked-optional-access)
