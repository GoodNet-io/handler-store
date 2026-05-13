// SPDX-License-Identifier: GPL-2.0-only
/// @file   plugins/handlers/store/tests/test_sqlite_backend.cpp
/// @brief  SqliteStore — backend contract round-trips on the same
///         test matrix that pins MemoryStore (slice 1 reference).
///         A handler driven through SqliteStore must observe the
///         same put/get/prefix/since/del/cleanup semantics so an
///         operator can swap backends via plugin manifest with no
///         user-visible behaviour change.

#include <gtest/gtest.h>

#include <sqlite_backend.hpp>
#include <store.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace gn::handler::store {
namespace {

TEST(SqliteStore_PutGet, RoundtripsValueInMemory) {
    SqliteStore s(":memory:");
    const std::vector<std::uint8_t> v{1, 2, 3, 4};
    ASSERT_TRUE(s.put("key", v, 0, 0));
    auto hit = s.get("key");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value().key, "key");
    EXPECT_EQ(hit.value().value, v);
    EXPECT_EQ(s.size(), 1u);
}

TEST(SqliteStore_PutGet, RejectsEmptyKey) {
    SqliteStore s(":memory:");
    EXPECT_FALSE(s.put("", {}, 0, 0));
    EXPECT_EQ(s.size(), 0u);
}

TEST(SqliteStore_PutGet, RejectsOversizedKey) {
    SqliteStore s(":memory:");
    std::string huge(GN_STORE_KEY_MAX_LEN + 1, 'x');
    EXPECT_FALSE(s.put(huge, {}, 0, 0));
}

TEST(SqliteStore_PutGet, RejectsOversizedValue) {
    SqliteStore s(":memory:");
    std::vector<std::uint8_t> huge(GN_STORE_VALUE_MAX_LEN + 1);
    EXPECT_FALSE(s.put("k", huge, 0, 0));
}

TEST(SqliteStore_PutGet, MissReturnsNullopt) {
    SqliteStore s(":memory:");
    EXPECT_FALSE(s.get("missing").has_value());
}

TEST(SqliteStore_PutGet, OverwriteKeepsLatestValue) {
    SqliteStore s(":memory:");
    ASSERT_TRUE(s.put("k", std::vector<std::uint8_t>{1}, 0, 0));
    ASSERT_TRUE(s.put("k", std::vector<std::uint8_t>{2, 3, 4}, 0, 0));
    auto hit = s.get("k");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value().value, (std::vector<std::uint8_t>{2, 3, 4}));
    EXPECT_EQ(s.size(), 1u);
}

TEST(SqliteStore_PutGet, PreservesBinaryKey) {
    SqliteStore s(":memory:");
    /// Keys are BLOBs in SQLite — embedded NUL must round-trip.
    const std::string key{"a\0b\0c", 5};
    ASSERT_TRUE(s.put(key, std::vector<std::uint8_t>{0xff}, 0, 0));
    auto hit = s.get(key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value().key.size(), 5u);
    EXPECT_EQ(hit.value().key, key);
}

TEST(SqliteStore_Prefix, MatchesByPrefix) {
    SqliteStore s(":memory:");
    ASSERT_TRUE(s.put("peer/alice", std::vector<std::uint8_t>{0xa}, 0, 0));
    ASSERT_TRUE(s.put("peer/bob",   std::vector<std::uint8_t>{0xb}, 0, 0));
    ASSERT_TRUE(s.put("svc/chat",   std::vector<std::uint8_t>{0xc}, 0, 0));
    auto hits = s.get_prefix("peer/", 256);
    EXPECT_EQ(hits.size(), 2u);
}

TEST(SqliteStore_Prefix, EmptyPrefixReturnsAll) {
    SqliteStore s(":memory:");
    ASSERT_TRUE(s.put("a", std::vector<std::uint8_t>{1}, 0, 0));
    ASSERT_TRUE(s.put("b", std::vector<std::uint8_t>{2}, 0, 0));
    ASSERT_TRUE(s.put("c", std::vector<std::uint8_t>{3}, 0, 0));
    EXPECT_EQ(s.get_prefix("", 256).size(), 3u);
}

TEST(SqliteStore_Prefix, RespectsMaxResults) {
    SqliteStore s(":memory:");
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(s.put("k" + std::to_string(i),
            std::vector<std::uint8_t>{1}, 0, 0));
    }
    EXPECT_EQ(s.get_prefix("k", 3).size(), 3u);
}

TEST(SqliteStore_Delete, RemovesEntry) {
    SqliteStore s(":memory:");
    ASSERT_TRUE(s.put("k", std::vector<std::uint8_t>{1}, 0, 0));
    EXPECT_TRUE(s.del("k"));
    EXPECT_FALSE(s.get("k").has_value());
    EXPECT_EQ(s.size(), 0u);
}

TEST(SqliteStore_Delete, MissReturnsFalse) {
    SqliteStore s(":memory:");
    EXPECT_FALSE(s.del("nope"));
}

TEST(SqliteStore_Cleanup, DropsExpired) {
    SqliteStore s(":memory:");
    ASSERT_TRUE(s.put("temp", std::vector<std::uint8_t>{1}, 1 /*s*/, 0));
    ASSERT_TRUE(s.put("perm", std::vector<std::uint8_t>{2}, 0,      0));
    auto temp_entry = s.get("temp");
    ASSERT_TRUE(temp_entry.has_value());
    const std::uint64_t future = temp_entry.value().timestamp_us + 5'000'000ULL;
    EXPECT_EQ(s.cleanup_expired(future), 1u);
    EXPECT_FALSE(s.get("temp").has_value());
    EXPECT_TRUE (s.get("perm").has_value());
}

TEST(SqliteStore_Since, FiltersByTimestamp) {
    SqliteStore s(":memory:");
    ASSERT_TRUE(s.put("a", std::vector<std::uint8_t>{1}, 0, 0));
    auto a_entry = s.get("a");
    ASSERT_TRUE(a_entry.has_value());
    const auto a_ts = a_entry.value().timestamp_us;
    ASSERT_TRUE(s.put("b", std::vector<std::uint8_t>{2}, 0, 0));
    auto hits = s.get_since(a_ts, 256);
    ASSERT_FALSE(hits.empty());
    bool seen_b = false;
    for (const auto& e : hits) if (e.key == "b") seen_b = true;
    EXPECT_TRUE(seen_b);
}

TEST(SqliteStore_Persistence, RoundtripsThroughFile) {
    /// File path under the test's temp dir; cleaned up after the
    /// test. Use a fresh subdirectory so a parallel test run on
    /// the same machine can't tread on the same DB file.
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path()
        / ("goodnet_store_test_" +
           std::to_string(::testing::UnitTest::GetInstance()
               ->random_seed()));
    fs::create_directories(dir);
    const auto path = (dir / "store.sqlite3").string();

    /// Write phase — open, put, close.
    {
        SqliteStore s(path);
        ASSERT_TRUE(s.put("persist/key",
            std::vector<std::uint8_t>{0xa, 0xb, 0xc}, 0, 7));
    }
    /// Read phase — re-open the same file, expect the entry back.
    {
        SqliteStore s(path);
        auto hit = s.get("persist/key");
        ASSERT_TRUE(hit.has_value());
        EXPECT_EQ(hit.value().value,
                  (std::vector<std::uint8_t>{0xa, 0xb, 0xc}));
        EXPECT_EQ(hit.value().flags, 7u);
    }
    fs::remove_all(dir);
}

TEST(SqliteStore_Constructor, RejectsInvalidPath) {
    /// A path inside a non-existent directory should fail at open.
    /// SQLite returns the error through the open call; the ctor
    /// translates it to a runtime_error.
    EXPECT_THROW(
        SqliteStore("/this/path/should/never/exist/store.sqlite3"),
        std::runtime_error);
}

}  // namespace
}  // namespace gn::handler::store

// NOLINTEND(bugprone-unchecked-optional-access)
