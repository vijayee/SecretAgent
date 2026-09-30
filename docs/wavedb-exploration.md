# WaveDB Deep Exploration

A source-cited survey of WaveDB (Victor's embeddable C database — Hierarchical B+Trie + MVCC + WAL + subtrees/batches/graph layer), for the SecretAgent runtime's persistence slice (`remember-session-across-restarts`). Every claim links to a file inside the `WaveDB` symlink (resolving to `~/Workspace/src/github.com/vijayee/WaveDB`). Cite-as: file:line.

Repo versioning at read time: CMake 0.1.15 (`CMakeLists.txt:2`); Python binding `wavedb` 0.2.6 (`bindings/python/pyproject.toml`).

---

## 1. Repo layout

- `src/Database/` — the database layer: `database.{h,c}`, `database_subtree.{h,c}`, `database_iterator.{h,c}`, `batch.{h,c}`, `wal.{h,c}`, `wal_manager.{h,c}`, `wal_compactor.{h,c}`, `database_config.{h,c}`, `database_lru.{h,c}`, `eviction_queue.{h,c}`. Windows export list: `src/wavedb.def`.
- `src/HBTrie/` — `hbtrie.{h,c}`, `bnode.{h,c}`, `bs_array.{h,c}`, `chunk.{h,c}`, `identifier.{h,c}`, `path.{h,c}`, `mvcc.{h,c}`.
- `src/Layers/graph/` — `graph.{h,c}`, `graph_internal.h`, `graph_ops.c`, `graph_optimizer.c`, `graph_parser.c`, `graph_schema_parser.c`, `graph_set.c`, `graph_stats.c`.
- `src/Layers/graphql/` — lexer/parser/plan/resolve/schema/result (`src/Layers/graphql/graphql.h`).
- `src/Layers/vector/` — FLAT/IVF/SLSH ANN (`src/Layers/vector/vector_layer.h`).
- `src/Storage/` — `page_file.{h,c}` (page-file persistence), `bnode_cache.{h,c}`, `node_serializer.{h,c}` (V3 format), `stale_region.{h,c}`, `encryption.{h,c}`.
- `src/Workers/` — `pool.{h,c}`, `promise.{h,c}`, `work.{h,c}`, `queue.{h,c}`, `transaction_id.{h,c}`, `error.{h,c}`.
- `tests/` — gtest + plain C; canonical usage in `test_subtree.c`, `test_sync_only.c`, `test_batch.cpp`, `test_raw_api.cpp`, `test_mvcc.cpp`, `test_batch_scan_padding.cpp`, `test_graph.cpp`, `test_graph_set.c`.
- `deps/` — xxhash, libcbor, hashmap, googletest (submodules).
- `bindings/python` (cffi ABI mode, `src/wavedb/`), `bindings/dart`, `bindings/nodejs`.
- Build: CMake `wavedb` STATIC (`CMakeLists.txt:144`), `wavedb_shared` SHARED (`CMakeLists.txt:618`), SOVERSION 0. Python wheel builds `libwavedb.so` from `bindings/python/c_src` (a mirror of `src/`).
- Docs: `README.md`, `PERSISTENCE.md` (WAL + snapshot architecture), `bindings/python/README.md` + `KNOWN_ISSUES.md`, `docs/graph-db-schema-layer.md`.

## 2. C API surface (real signatures)

### Lifecycle / config — `src/Database/database.h`
- `database_create_with_config(location, config, error_code)` (:150); `database_create_encrypted(...)` (:166); `database_destroy` (:177).
- `location == NULL` ⇒ true in-memory mode (no WAL/page file/config; `database.c:848`).
- `database_config_t` (`database_config.h:77-102`): immutable `chunk_size` (default 4), `btree_node_size` (4096), `enable_persist`, `encryption`, `sync_only`; mutable `lru_memory_mb`, `bnode_cache_memory_mb`, `wal_config` (sync mode/debounce/max_file_size/max_sealed_wals), `vacuum_config`, `worker_threads`.

### Sync CRUD — `database.h:237-260, 330-341` (path-based) and raw
- `database_put_sync(db, path_t*, identifier_t*)` consumes both; `database_get_sync(db, path, **result)` → 0 found / -1 err / -2 not found; `database_delete_sync`; `database_increment_sync(db, path, delta)` (:273) — a read-modify-write counter; **not WAL-atomic as a counter op** (concurrent increments rely on shard locks in concurrent mode; in `sync_only` this path is single-threaded).
- Raw variants: `database_put_sync_raw(db, key, key_len, delimiter, value, value_len)` / `database_get_sync_raw` / `database_delete_sync_raw` (:330-341) — keys are arbitrary bytes.
- `path_t` = refcounted vector of subscript identifiers; `path_create_from_raw(key, len, delim, chunk_size)` splits on the delimiter (`src/HBTrie/path.h:57`).

### Batches / transactions — `database.h:293-365`, `batch.h:57-115`
- `batch_create`, `batch_add_put`/`batch_add_delete`, `database_write_batch_sync`; raw: `database_batch_sync_raw(db, delim, raw_op_t*, count)` (:360) with `raw_op_t {key, key_len, value, value_len, type}` (type 0 = put, 1 = delete, :313-319). Default cap: `BATCH_DEFAULT_MAX_SIZE` = 10000 (`batch.h:57`).
- Atomicity: whole batch serialized as **one `WAL_BATCH` record under one `txn_id`**, applied under the same MVCC transaction, commit point at the end (`database.c:2993-3064`). Cross-subtree atomicity is the same transaction, built from root-namespace keys — this is exactly the workload `graph_triple_expand_ops` was built for (`graph.h:69-86`).
- **Concurrent-mode caveat**: a batch larger than `wal_config.max_file_size` (default **128 KB**, `wal_manager.h:138`) is rejected with -5 (`database.c:2994-2995`); the sync_only fast path skips that check.

### Scan / stream — `database_iterator.h`
- `database_scan_start(db, start_path, end_path)` (`:69`, half-open bounds; **seek, not walk**: `database_iterator.c:291-316` replaces the old walk-from-skip with chunk-descent so cost is O(depth) not O(keys-before-start)); `database_scan_next(iter, **path, **value)` → 0 / -1 end / -2 err; `database_scan_end`; reverse forms at `:115/:130`.
- **No persistent cursor** — resume = open a new iterator with `start_path` = byte-successor of the last seen key (prefix-successor idiom in `graph_ops.c:append_successor`, same as `database_subtree_delete_prefix` uses).
- Bulk `database_scan_sync_raw` / `database_scan_range_sync_raw` (`database.c:3531/3629`) are **collect-all** (they materialize the whole range into one `raw_result_t[]`), not incremental.
- An open iterator pins `read_txn_id` and blocks vacuum (`database_iterator.c:850-873`, `database.h:395-404`).

### Delete prefix
- `database_subtree_delete_prefix(db, prefix, delim)` (`database_subtree.{h:80,c:73-100}`) = prefix range scan + per-key delete — **not a single atomic op**.

---

## 3. Subtree model

- `database_subtree_t` (`database_subtree.h:32-40`) is a **pure prefix view**, not a physical structure. `database_subtree_open(db, prefix, delim)` (:54) prefixes keys at call time and strips the prefix from scan results via `database_iterator_t.prefix_skip` (`database_subtree.h:421-433`). **Caveat from the header itself**: prefix-strip requires entries written with `path_meta` (the 2026 padding fix); legacy entries keep the prefix.
- Open/close is **O(1)** (small struct + string copy + refcount bump; `database_subtree.c:37-71`). A `database_subtree_t` holds a ref on the parent db.
- **No subtree-of-subtree** — `database_subtree_open` takes only `database_t*`; compose a single full prefix (`"sessions/<sid>/events"`).
- Subtree batch (`database_subtree_batch_sync_raw`, `database_subtree.h:366`) prefixes each op's key and submits one batch under the parent — atomic.
- Key cost model: a path component of length L is chunked into ⌈L/4⌉ trie levels with the default `chunk_size=4` (`src/HBTrie/chunk.h:18`); a `bnode_entry_t` holds one chunk at a time with inline data for ≤8-byte keys + 16-byte per-subscript meta (`src/HBTrie/bnode.h:22-106`); concurrent mode adds a per-leaf version-chain node (24-byte txn id + value + 2 pointers). No db-layer key length limit; the **graph layer** assumes keys fit in 4096-byte stack buffers during prefix scans (`graph_ops.c:107-111`), so keep lineage key component budgets comfortable there.

## 4. Graph layer mechanics

- Index layout: `/spo/<s>/<p>/<o>`, `/pos/<p>/<o>/<s>`, `/osp/<o>/<s>/<p>`, `/pso/<p>/<s>/<o>` with empty values as presence markers (`graph.c:18-41`); in subtree mode they are prefixed (`graph.c:273-277`).
- `graph_triple_expand_ops(layer, s, p, o, type, out_ops, max)` (`graph.h:71-86`, `graph.c:408-437`) computes the index op list WITHOUT writing — so lineage indices can ride in one `database_batch_sync_raw` with content ops: one atomic transaction ("event + index + state" in one batch), which is exactly the write pattern the agent's frame tree wants.
- Queries: Gremlin-style `graph_parse_execute(...)` (`graph.h:134`) and C-level `graph_query_vertex/out/in/has/intersect/...` + `graph_query_execute_sync` (`:111`). Out/In = SPO/POS prefix range scans. **No recursive/lineage primitive** — depth-N lineage = N+1 bounded scans (each cheap; no join primitive).
- Schema parsed/persisted/reloaded on open (`graph_schema_parse` → `graph_schema_load`, `graph.c:131`); unknown predicates default to indexed (no-schema default, `graph.c:885-898`); cost-based optimizer reorders Has/Intersect by PSO-scan statistics (`graph_internal.h:113-115`).
- Layer coexistence: a root-layer graph refuses to share the db with another type (error -3); subtree mode bypasses (`graph.c:87-115`).

## 5. Concurrency & lifecycle

- **Two modes**: `sync_only=1` = no MVCC chains, no locks, per-op single-threaded (lock-free `hbtrie_insert_unsafe/...`; every test in the suite is sync_only). `sync_only=0` = concurrent MVCC: tx-manager + 64-key shard spinlocks (`database.h:46`); readers are lock-free against the last-committed id (`database.c:2599-2603`).
- Reads are per-call snapshots — no stable long-lived snapshot handle (a consistent multi-get view requires holding one iterator; open cursors pin `read_txn_id` and block vacuum).
- **Durability at three levels**: (1) per-op = WAL record appended to a thread-local WAL via writev/O_APPEND (`wal_manager.c:1217-1400`) gated by `wal_sync_mode`: **IMMEDIATE** = fsync per flush, **DEBOUNCED** (default 250 ms) = buffered then fsync'd on a debounce timer — a `put_sync` return does NOT imply fsync, **ASYNC** = never fsync; (2) checkpoint = `database_snapshot(db)` (`database.c:1890-1932`: WAL flush + MVCC GC + page-file bnode CoW + superblock advance); (3) close = `database_destroy` → persist + fsync + `wal_manager_seal_and_compact(mark_compacted)`; crash recovery replays non-COMPACTED WAL files over the page-file base (`PERSISTENCE.md` "WAL Recovery", `database.c:1205-1207`).
- Vacuum (`database_vacuum` / `_vacuum_auto` / `_vacuum_status`) blocks during open cursors and writers.

## 6. Python bindings surface

cffi ABI mode with opaque structs (`bindings/python/_native_abi.py`); everything in the C layer is reachable from the ABI entry point `database_create_with_config`:
- `WaveDB` (`src/wavedb/database.py`): put/get/del sync+async, `flush`, `vacuum`, `batch_sync` → `database_batch_sync_raw`, `create_read_stream(start, end)` → collect-all `scan_range_sync_raw`, `open_subtree`, `delete_subtree`, `put_object_sync/get_object_sync`; `put_many`/`delete_many` = one atomic batch; `get_many` = asyncio.gather.
- `WaveDBConfig` (`config.py`): `chunk_size, btree_node_size, enable_persist, lru_memory_mb, lru_shards, wal_sync_mode ("debounced"|"immediate"|"none"), wal_debounce_ms, worker_threads, sync_only, in_memory` (in_memory → `location=NULL`).
- `Subtree` (`subtree.py`): sync + async put/get/del only — **no batch, scan, or increment from Python** (those exist only in the C API).
- `GraphLayer` (`graph_layer.py`): `insert_sync`, `query(…)`, `expand_triple(s,p,o, delete=)` → root-namespace ops on `database_batch_sync_raw`; `GraphQLLayer`/`VectorLayer` present; logging hooks in `wavedb.set_log_callback`.
- C-embedder notes: result handles require `identifier_get_data_copy` + `identifier_destroy` (bindings README), cffi char params are bytes, async resolves C `promise_t` over `call_soon_threadsafe`.

## 7. Gating facts — the five pre-verified answers

**(a) Cheap scan resume from a key?** YES at the C level: iterators seek (chunk-descent, O(path depth)) with half-open bounds; resume = open a new iterator at the byte-successor of the last key (the `append_successor` idiom in `graph_ops.c`, same as `database_subtree_delete_prefix`). The bulk `*_sync_raw` scan variants are collect-all (no streaming). Unverified: seek's cost still includes lazy-loading cold bnodes.

**(b) Cheap subtree open/close?** Yes — O(1) (struct + copy + refcount; `database_subtree.c:37-71`). Subtrees are virtual views, never materialized.

**(c) Cheap subtree clone/snapshot (fork)?** **No primitive exists.** `hbtrie_copy`/`hbtrie_node_copy` (`src/HBTrie/hbtrie.h:134-160`) is a deep node copy with no Database-layer caller; there's no atomic COW. Fork-seed today = scan-parent + one raw batch into the child subtree, or resolve lazily from the `meta/parent` pointer. Because the DSH lineage model makes a fork essentially "a pointer + a new subtree," this doesn't bite the architecture — it bites a hypothetical eager-copy implementation.

**(d) Batch atomicity across subtrees?** YES: root-level `database_batch_sync_raw` = one WAL_BATCH under one txn_id + one commit point (`database.c:2993-3064`); subtree batches are the same transaction prefixed. Caveats: version-chain atomicity is the concurrent-mode model; in `sync_only` it is trivially single-thread-sequenced. And the concurrent-mode batch must fit `wal_config.max_file_size` (default 128 KB) — size accordingly or raise the limit.

**(e) Append+scan dominant-mode works?** YES: append = one WAL PUT + one trie insert (~5.6 µs in-memory, ~11 µs with WAL — bindings-python README benchmarks), scan = forward or reverse range, resume via seek. Ordering is byte-lexical, so seq keys must be zero-padded (e.g. `%020llu`). Per-event overhead: a full key path plus a 33-byte WAL header (`wal_manager.c:1227-1237`).

## 8. What the persistence slice still needs that WaveDB doesn't make cheap

1. **Cheap subtree clone / fork-seed** — no primitive; nearest = subtree scan + one root `database_batch_sync_raw` into the child (O(keys-in-subtree)) or lazy resolve via `meta/parent` (zero-copy). The DSH "fork = log + pointer" lineage model sidesteps this: forks are pointers, not copies.
2. **Persistent, resumable stream scan at the raw level** — `database_scan_range_sync_raw` materializes; long sessions must drive the C iterator API directly with `database_scan_start`/`_next`/`_end` (seek-resumable).
3. **A native seq/counter type for the log** — `database_increment_sync` is a read-modify-write counter (races in sync_only, shard-locked in concurrent mode but not WAL-atomic as a counter). Per-session event seq should instead be generated under the frame's own control — e.g. the actor mailbox's own counter written inside the same batch as the event (the atomicity is then free at the root-batch commit point).
4. **Atomic delete_prefix** (session truncate = per-key deletes; WAL'd per-key, not one transaction).
5. **No retention/compaction API** (TTL/range-truncate/count-cap) — frame compaction composes its own scan+batch through the root batch (deletes are first-class `raw_op_t.type=1`).
6. **No long-lived snapshot handle** — consistent multi-get = hold one iterator (open pins `read_txn_id`, blocks vacuum).
7. **No recursive lineage query** — the graph layer is per-hop prefix scans; depth-N lineage = N+1 scans. (A "find sessions ≥ depth 2" is also an N+1 walk, not one scan.)
8. **Python-only gaps** — subtree batch/scan/increment exist only in C; irrelevant to the runtime's native embedding story, but relevant if a Pondr-side Python client wants them.
9. **No value-level secondaries** — only the graph layer indexes value structure.
10. **Key-length caveat** — graph-layer prefix scans assume ≤4096-byte stack buffers (`graph_ops.c:107-111`); keep lineage component budgets comfortably below. The raw DB has no documented limit.

## Bottom line for the plan

Sessions-as-subtrees (`open` = O(1)), events as zero-padded seq keys with raw put + seek-resumable iterator scans, lineage as a `meta/parent` pointer + SPO/POS triple op expansion riding in the **same atomic root batch** as the event write (so event + state + lineage indices land in one commit). Durability at the ack boundary is a config choice on `wal_sync_mode`: `immediate` for crash-proof-at-ack, `debounced` (default 250 ms) for throughput. Snapshot/vacuum at operator pace. The only structural gap worth tracking is the absent cheap subtree clone — avoided by the lineage-as-pointer (not copy) model this architecture uses anyway.
