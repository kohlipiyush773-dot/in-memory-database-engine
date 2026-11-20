# 12-minute demo plan (who says what)

Start with `run.bat`. Keep the browser full-screen. Suggested split between the four members is at the end.

**0:00 Concept Map tab (1 min)** - "PageDB is a database storage engine that handles data bigger than RAM. It reuses the
operating system's virtual-memory idea inside a DBMS." Point at the layer diagram: SQL -> transactions/WAL -> heap + B+ tree ->
buffer pool -> replacement policies -> disk file.

**1:00 SQL Console (2 min)**
1. Click **+1000 students** twice (2,000 rows = about 15 heap pages; the table and its B+ tree are built through the 16-frame buffer pool).
2. Run `SELECT * FROM students WHERE id = 42` -> show the plan "Index Scan" and the chips (a handful of page accesses).
3. Run `SELECT * FROM students WHERE marks = 77` -> "Seq Scan", many more pages, misses and evictions.
4. Say: the database is far larger than the 16-frame pool, but every query works.

**3:00 Buffer Pool tab (2 min)**
1. Press **Replay last statement**: hits flash green, misses red, evictions orange. Explain miss = page fault, dirty = changed
   in RAM only.
2. Set **8 frames**, Apply, run the scan again, replay: lots of evictions. Switch the policy to **CLOCK** and show the R bits and hand.
3. Show the page table (valid / dirty / reference / pin) - "these are the same bits as in an OS page table".

**5:00 Storage & Index tab (1.5 min)** - B+ tree drawing; **Trace index lookup** for key 42 -> a few index pages instead of reading all ~15 heap pages.
Click a heap page: slot directory, decoded rows. Show the file map: "page N is at byte N x 4096".

**6:30 Page Replacement Lab (1.5 min)** - click **Textbook string** (compare all 4): FIFO 15, LRU 12, CLOCK 14, OPT 9 faults.
Click **Belady's anomaly string** then **Belady's anomaly**: FIFO 9 faults with 3 frames but 10 with 4. LRU never does that.

**8:00 Transactions & Recovery tab (2.5 min)**
1. Demo 2 (**Crash -> REDO**), **Run all**: committed data only existed in the log and dirty pages; after the crash recovery redoes it.
2. Demo 3 (**Crash -> UNDO**), **Run all**: uncommitted data had reached the disk (steal); recovery undoes it. Show the recovery card
   and the log with durable / not-durable markers. Mention the write-ahead rule.

**10:30 Benchmark tab (1 min)** - Run. Zipfian: OPT > LRU > CLOCK > FIFO. Sequential: LRU/FIFO/CLOCK 0% (sequential flooding),
OPT still hits. Show the effective access time with an HDD. Optionally run **index vs scan** (cold vs warm cache).

**11:30 Close (30 s)** - limitations (single user, lazy B+ tree delete) and future work (locking/MVCC, logging the index,
2Q / LRU-K scan-resistant policies, multithreading).

## Suggested split
1. Storage + buffer pool (`disk_manager`, `slotted_page`, `buffer_pool_manager`, Buffer Pool + Storage tabs)
2. OS part: replacement policies, OS Lab, Benchmark (`replacers`, `simulator`)
3. DBMS query layer: SQL parser, planner, B+ tree (`sql`, `executor`, `btree`)
4. Transactions, WAL, recovery + server/UI (`log_manager`, `database.cpp`, Transactions tab, `server.py`)

Everybody should be able to explain the architecture slide and the life of a query (Concept Map tab).
