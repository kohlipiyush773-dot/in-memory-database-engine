'use strict';
// ====================================================================== Concept map: where each OS / DBMS topic lives in the project
function initConcepts() {
  const row = (c, w, f) => '<tr><td>' + c + '</td><td>' + w + '</td><td><code>' + f + '</code></td></tr>';
  $('#tab-concepts').innerHTML =
    '<div class="card"><h3>Architecture (every layer below is implemented in this project)</h3>' +
    '<div class="layer db"><b>Web UI</b> (this page) <span>HTML/JS dashboard &rarr; Python server (standard library) &rarr; engine over a line protocol</span></div>' +
    '<div class="layer db"><b>Query layer</b> <span>SQL parser, planner (B+ tree index scan vs sequential scan, EXPLAIN), executor &mdash; <code>sql.cpp</code>, <code>executor.cpp</code></span></div>' +
    '<div class="layer db"><b>Transactions + recovery</b> <span>BEGIN / COMMIT / ROLLBACK, undo list, write-ahead log, checkpoints, ARIES-style analysis-redo-undo &mdash; <code>database.cpp</code>, <code>log_manager.cpp</code></span></div>' +
    '<div class="layer db"><b>Access methods</b> <span>heap file of slotted pages with record ids, B+ tree primary-key index &mdash; <code>slotted_page.cpp</code>, <code>btree.cpp</code></span></div>' +
    '<div class="layer both"><b>Buffer pool manager</b> <span>frames, page table, pin counts, dirty bits, demand loading, write-ahead rule &mdash; <code>buffer_pool_manager.cpp</code></span></div>' +
    '<div class="layer os"><b>Page replacement policies</b> <span>FIFO, LRU, Clock (second chance), OPT &mdash; <code>replacers.cpp</code></span></div>' +
    '<div class="layer os"><b>Disk manager</b> <span>one binary file = array of 4 KB pages, real file I/O, fsync &mdash; <code>disk_manager.cpp</code></span></div></div>' +
    '<div class="concept"><div class="card"><h3>Operating-system concepts used</h3><table><tr><th>concept</th><th>how it appears</th><th>where</th></tr>' +
    row('Paging, fixed-size pages', 'database file and RAM are cut into 4 KB pages', 'common.h, page.h') +
    row('Frames / frame table', 'array of Page objects in the buffer pool', 'buffer_pool_manager.cpp') +
    row('Page table', 'page id &rarr; frame id hash map', 'page_table_') +
    row('Demand paging, page fault', 'a page is read only when asked for; a miss = fault', 'FetchPage') +
    row('Page replacement algorithms', 'FIFO, LRU, Clock, Optimal; policy switchable at runtime', 'replacers.cpp') +
    row('Valid / reference / modify bits', 'page_id != -1, Clock ref bit, dirty bit', 'page.h, ClockReplacer') +
    row('Pinning (page locking)', 'pinned frames are never victims', 'pin_count_') +
    row('Backing store / swap', 'the .db file; page N at byte N x 4096', 'disk_manager.cpp') +
    row('System calls, file I/O, fsync', 'fread / fwrite / fflush / fsync (_commit)', 'DiskManager, LogManager') +
    row('Belady\'s anomaly, stack algorithms', 'FIFO anomaly demo, LRU/OPT never', 'OS Lab tab') +
    row('Locality, thrashing, working set', 'scan vs zipfian workloads, small pool', 'Benchmark tab') +
    row('Effective access time', 'EAT = (1-p) x memory + p x fault time', 'Benchmark tab') +
    '</table></div><div class="card"><h3>Database concepts used</h3><table><tr><th>concept</th><th>how it appears</th><th>where</th></tr>' +
    row('Page-based storage, heap file', 'tables are chains of slotted pages', 'slotted_page.cpp') +
    row('Record id (page, slot)', 'every row is addressed this way; the index stores it', 'RID') +
    row('Catalog', 'table schemas stored on page 0', 'database.cpp') +
    row('Buffer manager', 'steal / no-force, dirty tracking, flush on eviction', 'buffer_pool_manager.cpp') +
    row('B+ tree index', 'splits, leaf chain, range scan, rebuilt after a crash', 'btree.cpp') +
    row('Query planning', 'index scan vs seq scan, EXPLAIN', 'executor.cpp') +
    row('Integrity constraints', 'PRIMARY KEY uniqueness, all-or-nothing statements', 'DoInsert') +
    row('ACID: atomicity', 'ROLLBACK and crash UNDO via before-images', 'RollbackTxn, Recover') +
    row('ACID: durability', 'log forced at COMMIT (write-ahead logging)', 'CommitTxn') +
    row('ACID: consistency, isolation', 'constraints; serial single-user execution (no locks)', 'see limitations') +
    row('ARIES-style recovery', 'analysis, redo by page LSN, undo with CLRs, checkpoints', 'Recover') +
    '</table></div></div>' +
    '<div class="concept"><div class="card"><h3>Life of <code>SELECT * FROM students WHERE id = 42</code></h3><ol>' +
    '<li>The parser turns the text into a statement; the planner sees a predicate on the primary key and picks an <b>index scan</b>.</li>' +
    '<li>The B+ tree asks the buffer pool for the root page. <b>Hit</b>: it is in a frame. <b>Miss</b>: the pool takes a free frame or asks the replacer for a victim, writes the victim to disk if dirty (after forcing the log), then reads the page.</li>' +
    '<li>Each node visited is pinned, read, unpinned. At the leaf the record id (page, slot) is found.</li>' +
    '<li>The heap page is fetched the same way and the row is decoded from its slot.</li></ol>' +
    '<p class="hint">Try it in the SQL Console and then watch the events in the Buffer Pool tab.</p></div>' +
    '<div class="card"><h3>Honest limitations and future work</h3><ul>' +
    '<li>Single user: statements run one after another, so isolation is trivial. Locking (2PL) and MVCC are future work.</li>' +
    '<li>Fixed set of types (INT, TEXT), one PRIMARY KEY, no joins, no secondary indexes.</li>' +
    '<li>B+ tree deletes are lazy (no merging); the index is not logged and is rebuilt after a crash.</li>' +
    '<li>Heap inserts go to the last page; space freed by deletes is reused only inside the same page. Dropped pages are not recycled.</li>' +
    '<li>Redo/undo is physical (page, slot, bytes). Real ARIES also logs index changes and supports fuzzy checkpoints.</li>' +
    '<li>No threads: a parallel buffer manager and a background writer would be natural extensions.</li></ul></div></div>';
}
