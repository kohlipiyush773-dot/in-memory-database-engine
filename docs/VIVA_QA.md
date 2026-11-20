# Viva questions and honest answers

Numbers quoted below come from the real engine (run them yourself in the Page Replacement Lab and the Benchmark tab).

## A. Operating Systems evaluator

**1. In OS terms, what is this project?**  A user-space virtual-memory manager inside a database. The database file is the
*backing store* (swap), the buffer pool is *physical memory*, pages are 4 KB like OS pages, `page_table_` maps page -> frame,
a miss is a *page fault*, and a replacement policy picks the victim.

**2. Page vs frame?**  A page is a 4 KB block of the file (`page_id`). A frame is a 4 KB slot in RAM (`frame_id`) that can hold one page at a time.

**3. Where is the page fault in your code?**  `BufferPoolManager::FetchPage`: if the page is not in `page_table_` it takes a free frame
(or evicts a victim), then `DiskManager::ReadPage` loads it (demand paging - pages are loaded only when asked for).

**4. Which replacement algorithms and how do they work?**
FIFO evicts the oldest loaded page. LRU evicts the page unused for the longest time (list + move-to-front, O(1)). Clock keeps a reference bit per
frame and a hand; bit 1 -> clear it and move on (second chance), bit 0 -> evict. OPT evicts the page whose next use is farthest away (needs the future).

**5. Why Clock and not exact LRU?**  Exact LRU needs an update on *every* access. Hardware only sets a reference bit, so real kernels (and PostgreSQL's
buffer manager) use Clock. In our measurements Clock stays within about 1-2 points of LRU (Zipfian, 64 frames: LRU 48.6%, Clock 47.2%, FIFO 43.3%).

**6. What is Belady's anomaly? Show it.**  More frames can cause more faults. Reference string 1 2 3 4 1 2 5 1 2 3 4 5 with FIFO: 9 faults with 3 frames,
10 faults with 4 (Lab tab -> *Belady's anomaly*). LRU and OPT are *stack algorithms*: the set of pages kept with n frames is always a subset of
those kept with n+1 frames, so they cannot show it.

**7. Is OPT implementable? Why include it?**  Not online - it needs the future reference string. We compute next-use positions offline and use OPT only as a lower bound
to judge the others. Textbook string, 3 frames: FIFO 15, LRU 12, Clock 14, OPT 9 faults.

**8. What is thrashing / sequential flooding in your results?**  A cyclic scan over more pages than frames gives FIFO, LRU and Clock a **0%** hit ratio
because every page is evicted just before it is needed again (Benchmark -> sequential). OPT still hits because it keeps some pages. Remedies: scan-resistant
policies (LRU-K, 2Q, PostgreSQL's ring buffer for big scans).

**9. Locality of reference?**  Zipfian and 80/20 workloads have temporal locality, so LRU/Clock beat FIFO; the benchmark shows hit ratio rising with pool size.

**10. Effective access time?**  EAT = (1 - p) x memory time + p x fault time. Example: miss ratio p = 0.5, memory 100 ns, SSD fault 100 us -> EAT = 0.5 x 0.1 + 0.5 x 100 = 50.05 us.
The Benchmark tab computes it from measured miss ratios.

**11. Dirty / modify bit?**  `is_dirty_` says RAM differs from disk. A dirty victim is written before its frame is reused (write-back); a clean victim is simply dropped,
which saves I/O (test: "clean pages are not written").

**12. Pinning?**  `pin_count_ > 0` means somebody is using the page; the replacer never returns pinned frames (like `mlock`). If every frame is pinned `FetchPage`
returns null -> "buffer pool exhausted" (the analogue of out-of-memory).

**13. fflush vs fsync?**  `fflush` moves data from the C library buffer to the OS page cache; only `fsync` (`_commit` on Windows) forces it to the device. We fsync the
log at COMMIT and at checkpoints. Data pages are only `fflush`ed because the log already protects them.

**14. Why not just rely on the OS page cache / mmap?**  The database knows its access pattern (scan vs index lookup), needs to control *when* pages reach disk
(write-ahead logging) and must not cache twice (OS cache + buffer pool). That is why PostgreSQL and MySQL manage their own buffer pools.

**15. Why 4 KB pages?**  It matches the OS page and typical disk block, so one page I/O is one aligned unit. Bigger pages waste I/O on small rows; smaller ones raise
metadata overhead.

**16. Your page table vs the OS page table?**  Ours is a hash map (the pool is small). The OS uses multi-level hardware page tables with a TLB. Our "valid / reference / modify"
bits are software versions of the PTE bits.

**17. System calls involved?**  `fseek/fread/fwrite/fflush/fsync` map to `lseek/read/write/fsync` (or the Win32 equivalents).

**18. Differences from real virtual memory?**  Software checks instead of MMU traps; explicit `FetchPage/Unpin` calls; the replacement policy can use database knowledge.

**19. Multithreading, disk scheduling?**  Not implemented (single-threaded engine). A parallel pool would need latches on the page table and frames; a background writer and
elevator-style I/O ordering are listed as future work.

## B. DBMS evaluator

**1. How are rows stored?**  Heap file of **slotted pages**: header, slot directory growing from the front, records growing from the back. A row is addressed by a
**record id (page, slot)**. Deleted slots are reused; compaction squeezes holes.

**2. Where are the schemas?**  The **catalog** is text on page 0 (table id, name, first page, index root page, columns). It is rewritten and flushed on DDL.

**3. What does the buffer manager do for the DBMS?**  Caches pages, pin/unpin, tracks dirty pages, evicts with a policy, and enforces the write-ahead rule. Policy is *steal / no-force*:
dirty pages of uncommitted transactions may be written (steal) and committed pages need not be written at commit (no-force). Therefore recovery needs both **undo and redo**.

**4. Explain write-ahead logging.**  Every change is described in the log (INSERT with the new row image, DELETE with the old image) **before** the page changes, and the log is forced to disk
(a) at COMMIT, and (b) before a dirty page is written (`BufferPoolManager::WriteFrame` compares the page LSN with the flushed LSN). The Transactions tab shows durable vs not-yet-durable records.

**5. ACID - how?**  Atomicity: undo list and log before-images (ROLLBACK and crash UNDO). Consistency: primary-key uniqueness, type checks, all-or-nothing statements.
Durability: log forced at COMMIT. Isolation: only one user at a time, so transactions run serially. *Locking / MVCC is future work* (say this honestly).

**6. Describe crash recovery.**  *Analysis*: scan the log from the last checkpoint, classify transactions (committed, aborted, unfinished). *Redo*: repeat every logged change whose log LSN is greater
than the page LSN (idempotent). *Undo*: roll back the unfinished transactions newest-first using before-images, writing compensation records (CLRs), then log ABORT. Tested including a crash during recovery.

**7. Why does Redo compare the page LSN?**  If the page on disk already contains the change (it was flushed after the change) its LSN is >= the record's LSN, so redoing would be wrong or wasteful.

**8. Checkpoint?**  A *sharp* checkpoint: force the log, flush all dirty pages, write a CHECKPOINT record and remember its LSN on the catalog page. Recovery starts there.

**9. How does a B+ tree work here?**  Each node is a 4 KB page read through the buffer pool. Internal nodes route by separator keys; leaves hold (key, record id) and are linked for range scans. Insert splits full nodes
(leaf: copy-up, internal: move-up) and may grow a new root. Real nodes hold ~340 keys; ours hold at most 8 so that splits are visible.

**10. Why a B+ tree and not a B-tree or hash?**  Leaves chained for range queries, higher fan-out because internal nodes carry no row pointers, predictable height. A hash index cannot do ranges.

**11. How does the planner choose?**  If the WHERE clause has `pk = value` -> index scan; `<, <=, >, >=` on the key -> index range scan; otherwise sequential scan. `EXPLAIN` prints the plan.
Cost evidence: an index lookup touches a few pages; a scan of 2,000 rows touches all ~15 heap pages.

**12. Why is the index rebuilt after a crash?**  Index changes are not logged (a simplification). After an unclean shutdown the index is rebuilt from the recovered heap. ARIES would log B+ tree changes too.

**13. How is UPDATE done?**  As DELETE (old image logged) + INSERT (new image logged). The record id may change, so the index is updated. The primary key itself cannot be updated.

**14. What happens on a duplicate primary key in a multi-row INSERT?**  The statement is validated first (against the batch and the index) so nothing is modified: statements are all-or-nothing.

**15. Logical vs physical logging?**  Ours is physical-to-a-page (page, slot, bytes), which makes redo/undo simple and idempotent. Index changes would need logical logging.

**16. Normalisation, joins?**  Out of scope for the storage-engine focus. The engine has one table per query (no joins); joins and secondary indexes are future work.

**17. Concurrency control?**  None - single user. Next steps would be strict two-phase locking with a lock manager, or MVCC.

**18. What does the demo of UNDO prove?**  An uncommitted change reached the disk (steal), the process died, and after restart the change is gone and committed data is intact.

## C. General

* *What is new here?*  Both layers (OS-style paging and a DBMS) are built explicitly in one system and **measured against each other** (hit ratio, I/O, EAT) with the same code that serves SQL.
* *How was it tested?*  1,677 automated checks incl. textbook page-replacement results, Belady's anomaly, WAL rule, B+ tree with 3,000 random keys, crash-recovery scenarios, run under AddressSanitizer / UBSan.
* *What would you do next?*  Locking, logging the index, scan-resistant replacement (2Q / LRU-K), multithreaded buffer pool, joins.
