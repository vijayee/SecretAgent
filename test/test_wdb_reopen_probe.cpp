// test_wdb_reopen_probe.cpp — MINIMAL WaveDB reopen-amnesia probe, opened
// DIRECTLY on WaveDB's API with the library THIS harness links (the alias
// archive) — no SecretAgent layer in between. The recorded defect: a write
// committed after a reopen is durable but invisible to the SAME session's
// own scans and point-gets.
//
//   boot 1: write k1 (subtree batch), read it back on the same handle (sanity)
//   boot 2: reopen; read k1 (recovery replay, sanity); write k2 (batch);
//           read k2 on the SAME handle (THE probe); reverse-scan the range
//           on the SAME handle (THE scan probe)
//   boot 3: reopen; read k2 (durable/fresh-handle view)
//
// The test asserts the HEALTHY contract: when the substrate is fixed every
// probe below sees the writes and any miss is a failure — the recorded
// defect makes the same-handle probes fail here today, which IS the
// reproducible evidence.
#include <gtest/gtest.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_subtree.h"
#include "Database/database_iterator.h"
#include "Buffer/buffer.h"
#include "HBTrie/path.h"
}

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

static std::string probe_raw(database_t* db, const char* key) {
    uint8_t* value = nullptr;
    size_t len = 0;
    int rc = database_get_sync_raw(db, key, strlen(key), '/', &value, &len);
    std::string out;
    if (rc == 0 && value != nullptr) {
        out.assign((char*)value, len);
        free(value);
    }
    return out;
}

static int subtree_put(database_subtree_t* st, const char* key,
                       const char* value) {
    raw_op_t op;
    op.key = key;
    op.key_len = strlen(key);
    op.value = (const uint8_t*)value;
    op.value_len = strlen(value);
    op.type = 0;
    return database_subtree_batch_sync_raw(st, '/', &op, 1);
}

static std::string subtree_probe(database_subtree_t* st, const char* key) {
    uint8_t* value = nullptr;
    size_t len = 0;
    int rc = database_subtree_get_sync_raw(st, key, strlen(key), '/', &value,
                                           &len);
    std::string out;
    if (rc == 0 && value != nullptr) {
        out.assign((char*)value, len);
        free(value);
    }
    return out;
}

// The store actor's reverse-scan shape, joined in scan order (descending).
static std::string reverse_scan_join(database_t* db, const char* start_s,
                                     const char* end_s) {
    path_t* start = path_create_from_raw(start_s, strlen(start_s), '/', 0);
    path_t* end = path_create_from_raw(end_s, strlen(end_s), '/', 0);
    EXPECT_NE(start, nullptr);
    EXPECT_NE(end, nullptr);
    std::string joined;
    database_iterator_t* iter = database_scan_start_reverse(db, start, end);
    if (iter != nullptr) {
        path_t* k = nullptr;
        identifier_t* v = nullptr;
        while (database_scan_prev(iter, &k, &v) == 0) {
            size_t len = 0;
            uint8_t* data = identifier_get_data_copy(v, &len);
            if (data != nullptr) {
                if (!joined.empty()) joined += ",";
                joined.append((char*)data, len);
                free(data);
            }
            path_destroy(k);
            identifier_destroy(v);
            k = nullptr;
            v = nullptr;
        }
        database_scan_end(iter);
    }
    if (start != nullptr) path_destroy(start);
    if (end != nullptr) path_destroy(end);
    return joined;
}

static database_t* open_db(const std::string& location) {
    database_config_t* config = database_config_default();
    config->enable_persist = 1;
    database_config_set_sync_only(config, 0);   // the CONCURRENT mode
    int err = 0;
    database_t* db = database_create_with_config(location.c_str(), config,
                                                 &err);
    database_config_destroy(config);
    return db;
}

TEST(TestWdbReopenProbe, PostReopenWriteVisibleToThatHandlesOwnReads) {
    const char* base = getenv("TMPDIR");
    std::string tmpl = std::string((base != NULL && base[0] != '\0') ? base
                                                                    : "/tmp") +
                       "/sa-wdb-reopen-probe-XXXXXX";
    std::vector<char> tbuf(tmpl.begin(), tmpl.end());
    tbuf.push_back('\0');
    char* dir = mkdtemp(tbuf.data());
    ASSERT_NE(dir, nullptr);
    std::string loc = std::string(dir) + "/db";

    database_t* db = open_db(loc);
    ASSERT_NE(db, nullptr);
    database_subtree_t* s1 = database_subtree_open(db, "sessions/s1", '/');
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(subtree_put(s1, "events/k1", "v1"), 0);
    EXPECT_EQ(subtree_probe(s1, "events/k1"), "v1")
        << "boot-1 sanity: the same handle reads its own pre-restart write";
    database_subtree_close(s1);

    database_destroy(db);
    db = open_db(loc);
    ASSERT_NE(db, nullptr);
    s1 = database_subtree_open(db, "sessions/s1", '/');
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(subtree_probe(s1, "events/k1"), "v1")
        << "boot-2 sanity: the replayed record is readable";

    EXPECT_EQ(subtree_put(s1, "events/k2", "v2"), 0);
    std::string same_handle_get = subtree_probe(s1, "events/k2");
    std::string same_handle_scan =
        reverse_scan_join(db, "sessions/s1/events", "sessions/s1/events0");
    database_subtree_close(s1);
    database_destroy(db);

    db = open_db(loc);
    ASSERT_NE(db, nullptr);
    std::string fresh_handle_get = probe_raw(db, "sessions/s1/events/k2");
    database_destroy(db);

    std::printf("[  PROBE  ] same-handle get  = %s\n", same_handle_get.c_str());
    std::printf("[  PROBE  ] same-handle scan = %s\n", same_handle_scan.c_str());
    std::printf("[  PROBE  ] fresh-handle get  = %s\n", fresh_handle_get.c_str());
    fflush(stdout);

    EXPECT_EQ(same_handle_get, "v2")
        << "the same handle must read its own post-reopen write";
    EXPECT_EQ(same_handle_scan, "v2,v1")
        << "the same handle's reverse scan must see both records";
    EXPECT_EQ(fresh_handle_get, "v2");

    std::filesystem::remove_all(dir);
}
