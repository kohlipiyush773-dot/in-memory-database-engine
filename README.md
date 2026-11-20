# PageDB - an in-memory database engine with OS-style page replacement

B.Tech CSE major project - *Design and Implementation of an In-Memory Database Engine with OS-Based Custom Page
Replacement for Buffer Pool Management*.

PageDB is a small but complete database engine written in C++ that can work on **more data than fits in its
memory**. It does this the way an operating system does virtual memory: data lives in a file of 4 KB pages, a
**buffer pool** keeps a few pages in RAM, a **page-replacement policy** (FIFO, LRU, Clock, Optimal) decides which
page leaves when RAM is full, and modified (*dirty*) pages are written back safely. On top of that sits a real
DBMS: SQL, a B+ tree index, transactions, a write-ahead log and crash recovery. A web dashboard shows all of it live.

```
 Browser (web/)  <--HTTP/JSON-->  server/server.py  <--stdin/stdout-->  engine/ (C++)
                                                                          SQL -> planner -> executor
                                                                          transactions + WAL + recovery
                                                                          heap files, B+ tree
                                                                          BUFFER POOL + replacers   <- OS concepts
                                                                          disk manager (binary file)
```

## 1. Quick start on Windows (MinGW g++ and Python 3 installed)

1. Extract the zip. Open the `pagedb` folder, click the address bar, type `cmd`, press Enter.
2. `build.bat`  - compiles the engine and the tests. You must see **BUILD OK**.
3. `test.bat`   - runs 1,677 automated checks. You must see **ALL TESTS PASSED**.
4. `run.bat`    - starts the app and opens **http://127.0.0.1:8080** in your browser.

Linux / macOS: `./build.sh` then `./run.sh`.  With CMake: `cmake -B build && cmake --build build`.
No browser? `shell.bat` gives an interactive SQL shell in the terminal.

Troubleshooting
* *python is not recognized* - install Python 3 from python.org and tick "Add to PATH" (or use the `py` launcher).
* *port already in use* - `run.bat --port 8081`.
* *build error* - copy the full error text; the code is checked for old compilers (g++ 6.x, `-std=gnu++14`).
* To start with an empty database press **Reset database** in the *Transactions & Recovery* tab, or delete `data/`.

## 2. What you can do in the web app

| Tab | What it shows | Subject |
|---|---|---|
| SQL Console | SQL with query plans (index scan vs sequential scan) and the buffer-pool cost of every statement | DBMS |
| Buffer Pool | live frames (clean / dirty / pinned / reference bit), page table, hit ratio, animated replay of hits, misses, evictions, flushes | OS + DBMS |
| Storage & Index | B+ tree drawing, index lookup path, heap page slots, the database file as a page map | DBMS |
| Page Replacement Lab | textbook step-by-step tables for FIFO / LRU / Clock / OPT on your own reference string, Belady's anomaly chart | OS |
| Transactions & Recovery | guided demos of ROLLBACK, crash -> REDO, crash -> UNDO, write-ahead log viewer, recovery report | DBMS |
| Benchmark | policies x pool sizes x workloads, charts, effective access time, index-vs-scan and cold-vs-warm cache experiment | OS + DBMS |
| Concept Map | where every OS and DBMS concept lives in the code, limitations | both |

## 3. Features

**OS side**: 4 KB paging, frames and frame table, page table, demand paging and page faults, FIFO / LRU / Clock
(second chance) / Optimal replacement behind one `Replacer` interface (switch at run time), valid / reference /
modify bits, pinning, backing store with real file I/O and `fsync`, Belady's anomaly, thrashing and locality
experiments, effective-access-time formula.

**DBMS side**: catalog, slotted heap pages with record ids, variable-length records, B+ tree primary-key index
(splits, leaf chain, range scan), SQL (`CREATE/DROP TABLE, INSERT, SELECT ... WHERE/ORDER BY/LIMIT/COUNT,
UPDATE, DELETE, EXPLAIN, BEGIN/COMMIT/ROLLBACK, CHECKPOINT, SHOW TABLES, DESCRIBE`), constraint checking,
steal / no-force buffer management, **write-ahead logging** (the log is forced before a dirty page may be written
and at COMMIT), ARIES-style recovery (analysis, redo by page LSN, undo with compensation records), checkpoints.

## 4. Folder layout

```
engine/include, engine/src   C++ engine (see docs/ and the Concept Map tab for a file-by-file role list)
engine/tests/test_engine.cpp 1,677 checks: every layer, crash recovery, textbook page-replacement results
server/server.py             tiny web server (Python standard library only)
web/                         the dashboard (HTML, CSS, vanilla JavaScript - no frameworks, works offline)
tools/plot_benchmarks.py     optional Matplotlib graphs from the benchmark results
results/                     sample benchmark output (hit_ratio.png, disk_io.png, bench_latest.json)
docs/VIVA_QA.md             likely questions from the OS and DBMS evaluators, with answers
docs/DEMO_SCRIPT.md          a 12-minute demo plan
data/                        the database file (pagedb.db) and its write-ahead log (pagedb.db.wal)
```

Key files: `replacers.cpp` (the 4 policies), `buffer_pool_manager.cpp` (page table, eviction, WAL rule),
`disk_manager.cpp`, `slotted_page.cpp`, `btree.cpp`, `sql.cpp`, `executor.cpp`, `log_manager.cpp`,
`database.cpp` (catalog, transactions, recovery).

## 5. Mapping to the synopsis

| Synopsis objective | Where |
|---|---|
| Page-based storage format | `slotted_page.cpp`, `disk_manager.cpp` |
| Buffer Pool Manager, configurable fixed size | `buffer_pool_manager.cpp`, Buffer Pool tab (8-512 frames) |
| LRU and Clock, compared | `replacers.cpp`, OS Lab + Benchmark tabs (FIFO and OPT added) |
| Dirty-page tracking, safe flush on eviction | `BufferPoolManager::GetFrame/WriteFrame`, tests |
| Binary file with real OS file I/O | `DiskManager` (fread/fwrite/fsync) |
| Hit ratio, latency, disk I/O across sizes and policies | Benchmark tab, `tools/plot_benchmarks.py` |
| ARIES / write-ahead logging (literature review) | `log_manager.cpp`, `Database::Recover` |

## 6. Honest limitations

Single user (no locking, so isolation is by serial execution); INT and TEXT types, one primary key per table, no
joins; B+ tree deletes are lazy and the index is rebuilt after a crash instead of being logged; heap inserts go to
the last page; no threads. These are listed in the Concept Map tab as future work.
