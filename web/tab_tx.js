'use strict';
// ====================================================================== Transactions, WAL and crash recovery (DBMS)
let TX_DEMO = null, TX_STEP = 0;

function txLog(title, body) {
  const el = $('#tx-steps');
  if (el.classList.contains('muted')) { el.classList.remove('muted'); el.innerHTML = ''; }
  const d = document.createElement('div');
  d.className = 'st';
  d.innerHTML = '<div class="sq">' + esc(title) + '</div><div class="res">' + body + '</div>';
  el.appendChild(d);
  el.parentElement.scrollTop = el.parentElement.scrollHeight;
}

async function txSql(sql, ignoreError) {
  const r = await execSql(sql);
  let b = r.ok ? '<span class="ok">' + esc(r.message) + '</span>' : '<span class="bad">' + esc(r.error) + '</span>';
  if (r.ok && r.columns.length) b += resultTable(r, 20);
  if (!r.ok && ignoreError) return r;
  txLog('SQL> ' + sql, b);
  return r;
}

function recoveryText(rec) {
  return 'Analysis found <b>' + rec.txns + '</b> transaction(s): ' + rec.committed + ' committed, ' + rec.aborted + ' aborted, <b>' + rec.losers + ' unfinished (losers)</b>. ' +
    'Redo scanned ' + rec.log_records + ' log records and re-applied <b>' + rec.redo_applied + '</b> change(s) that were missing on disk (skipped ' + rec.redo_skipped + ' already on disk, decided by the page LSN). ' +
    'Undo rolled back <b>' + rec.undo_applied + '</b> change(s) of the unfinished transaction(s).';
}

function renderRecovery(rec) {
  const el = $('#tx-recovery');
  if (!rec || !rec.ran) { el.className = 'muted'; el.textContent = 'no recovery has run yet - press "Crash now" or run a crash demo'; return; }
  el.className = '';
  el.innerHTML = '<div class="recgrid">' +
    '<div><h4>1. Analysis</h4>transactions <b>' + rec.txns + '</b><br>committed <b>' + rec.committed + '</b><br>losers <b>' + rec.losers + '</b></div>' +
    '<div><h4>2. Redo</h4>log records <b>' + rec.log_records + '</b><br>re-applied <b>' + rec.redo_applied + '</b><br>skipped <b>' + rec.redo_skipped + '</b></div>' +
    '<div><h4>3. Undo</h4>changes rolled back <b>' + rec.undo_applied + '</b><br>indexes rebuilt <b>' + (rec.indexes_rebuilt ? 'yes' : 'no') + '</b></div></div>' +
    '<p class="hint">' + recoveryText(rec) + '</p>';
}

async function loadWal() {
  const r = await api('/api/wal?n=40');
  const el = $('#tx-wal');
  if (!r.ok) { el.innerHTML = '<div class="err">' + esc(r.error) + '</div>'; return; }
  if (!r.records.length) { el.innerHTML = '<div class="muted">the log is empty</div>'; return; }
  el.innerHTML = '<div class="hint">' + r.total + ' records total, showing the last ' + r.records.length + '. Durable up to byte ' + r.flushed + ' of ' + r.end + '. <span class="bad">&#9675;</span> = only in memory (lost on a crash), <span class="ok">&#9679;</span> = on disk.</div>' +
    '<table><tr><th>LSN</th><th>type</th><th>txn</th><th>table</th><th>page:slot</th><th>bytes</th><th>CLR</th><th>durable</th></tr>' +
    r.records.map(x => '<tr><td class="num">' + x.lsn + '</td><td><span class="badge ' + x.type + '">' + x.type + '</span></td><td class="num">' + x.txn + '</td><td class="num">' + (x.table || '') + '</td><td class="num">' +
      (x.page >= 0 ? x.page + ':' + (x.type === 'ALLOC' ? 'prev ' : '') + x.slot : '') + '</td><td class="num">' + x.len + '</td><td>' + (x.clr ? 'undo' : '') + '</td><td>' + (x.durable ? '<span class="ok">&#9679;</span>' : '<span class="bad">&#9675;</span>') + '</td></tr>').join('') + '</table>';
}

async function txAfter() { await refreshStatus(); await refreshTables(); await loadWal(); }

async function txCrash() {
  const r = await api('/api/crash', {});
  if (!r.ok) { toast(r.error, 'bad'); return r; }
  S.recovery = r.recovery;
  let b = '<span class="bad">engine process killed - buffer pool and unflushed log tail are gone</span><br>' + recoveryText(r.recovery);
  txLog('CRASH, then restart + recovery', b);
  renderRecovery(r.recovery);
  await txAfter();
  return r;
}

const BANK_SETUP = async () => {
  await txSql('DROP TABLE bank', true);
  await txSql('CREATE TABLE bank (id INT PRIMARY KEY, owner TEXT, balance INT)');
  await txSql("INSERT INTO bank VALUES (1,'Piyush',1000),(2,'Durga',1000),(3,'Aditya',1000)");
};

const TX_DEMOS = [
  {title: '1. ROLLBACK (atomicity)', steps: [
    {t: 'Create the table', say: 'Three accounts are inserted. The INSERT statement is an auto-committed transaction: BEGIN, INSERT x3, COMMIT are appended to the write-ahead log and the log is forced to disk at COMMIT.', run: BANK_SETUP},
    {t: 'Start a transfer', say: 'BEGIN opens a transaction. The two UPDATEs write DELETE (old row image) + INSERT (new row image) records to the log before the pages change. Nothing is committed yet.', run: async () => {
      await txSql('BEGIN'); await txSql('UPDATE bank SET balance = 500 WHERE id = 1'); await txSql('UPDATE bank SET balance = 1500 WHERE id = 2'); }},
    {t: 'Read your own writes', say: 'Inside the transaction the new balances are visible. Look at the Buffer Pool and the log on the right: the changes are only in dirty pages and in the log tail.', run: async () => { await txSql('SELECT * FROM bank'); }},
    {t: 'ROLLBACK', say: 'The engine walks its undo list backwards. For every change it writes a compensation log record (CLR) and restores the old row image in the same slot, and it fixes the B+ tree index too.', run: async () => { await txSql('ROLLBACK'); }},
    {t: 'Check the result', say: 'Both balances are back to 1000: atomicity - all or nothing.', run: async () => { await txSql('SELECT * FROM bank'); }},
  ]},
  {title: '2. Crash -> REDO (durability)', steps: [
    {t: 'Commit some data', say: 'The rows are committed. COMMIT forces the log to disk, but the data pages stay dirty in the buffer pool (NO-FORCE policy) because writing them can wait.', run: BANK_SETUP},
    {t: 'Look at the buffer pool', say: 'Open the Buffer Pool tab: the bank pages are dirty (D). The disk copy of those pages is old. The log record COMMIT is durable though (black dot in the log).', run: async () => {
      await refreshStatus(); const d = S.pool.frames.filter(f => f.dirty).length;
      txLog('buffer pool', '<b>' + d + '</b> dirty frame(s) exist only in RAM right now.'); }},
    {t: 'CRASH!', say: 'The process is killed with nothing flushed. All RAM content is lost. On restart the engine sees "not shut down cleanly" and runs recovery: analysis, REDO, UNDO.', run: async () => { await txCrash(); }},
    {t: 'Check the data', say: 'The committed rows are back: REDO re-applied the logged changes to the pages whose page LSN was older than the log record.', run: async () => { await txSql('SELECT * FROM bank'); }},
  ]},
  {title: '3. Crash -> UNDO (steal)', steps: [
    {t: 'Committed baseline', say: 'Create the table and take a checkpoint so that the committed data is safely on disk.', run: async () => { await BANK_SETUP(); await txSql('CHECKPOINT'); }},
    {t: 'Uncommitted changes', say: 'A transaction changes a balance and inserts a ghost account but does NOT commit.', run: async () => {
      await txSql('BEGIN'); await txSql('UPDATE bank SET balance = 0 WHERE id = 1'); await txSql("INSERT INTO bank VALUES (99,'Ghost',1)"); }},
    {t: 'Dirty pages reach the disk (STEAL)', say: 'A buffer pool under memory pressure must evict dirty pages, even those of uncommitted transactions (STEAL). Flushing does the same. The write-ahead rule forces the log to disk first.', run: async () => {
      beginAction(); const r = await api('/api/flush', {}); endAction(r);
      txLog('flush dirty pages', r.ok ? '<span class="ok">' + r.io.disk_writes + ' page(s) written, log forced ' + r.io.wal_forces + ' time(s) before them (write-ahead rule)</span>' : esc(r.error)); }},
    {t: 'CRASH!', say: 'The uncommitted data is already on disk. Recovery must remove it: UNDO rolls back the loser transaction using the before-images stored in the log.', run: async () => { await txCrash(); }},
    {t: 'Check the data', say: 'Piyush has 1000 again and the Ghost account is gone. No committed data lost, no uncommitted data kept.', run: async () => { await txSql('SELECT * FROM bank'); }},
  ]},
];

function renderStepper() {
  const el = $('#tx-stepper');
  if (!TX_DEMO) { el.className = 'stepper muted'; el.textContent = 'Choose a demo. Each step is explained and then executed on the real engine.'; return; }
  el.className = 'stepper';
  const d = TX_DEMO, done = TX_STEP >= d.steps.length;
  let h = '<b>' + esc(d.title) + '</b><div class="prog">' + d.steps.map((s, i) => '<i class="' + (i < TX_STEP ? 'done' : (i === TX_STEP ? 'cur' : '')) + '"></i>').join('') + '</div>';
  if (done) h += '<div class="stepbox ok">Demo finished. Try the other demos or look at the log on the right.</div>';
  else h += '<div class="stepbox"><b>Step ' + (TX_STEP + 1) + ' of ' + d.steps.length + ': ' + esc(d.steps[TX_STEP].t) + '</b><p>' + esc(d.steps[TX_STEP].say) + '</p><button class="btn primary" id="btn-step">Run this step</button> <button class="btn" id="btn-stepall">Run all remaining</button></div>';
  el.innerHTML = h;
  if (!done) {
    $('#btn-step').onclick = () => runStep(false);
    $('#btn-stepall').onclick = () => runStep(true);
  }
}

async function runStep(all) {
  while (TX_DEMO && TX_STEP < TX_DEMO.steps.length) {
    const s = TX_DEMO.steps[TX_STEP];
    txLog('-- step ' + (TX_STEP + 1) + ': ' + s.t, '<span class="muted">' + esc(s.say) + '</span>');
    await s.run();
    TX_STEP++;
    await txAfter();
    renderStepper();
    if (!all) break;
  }
}

function startDemo(i) {
  TX_DEMO = TX_DEMOS[i]; TX_STEP = 0;
  $('#tx-steps').className = 'steps muted'; $('#tx-steps').textContent = '-';
  renderStepper();
}

function initTx() {
  $('#tx-demos').innerHTML = TX_DEMOS.map((d, i) => '<button class="btn accent" data-i="' + i + '">' + esc(d.title) + '</button>').join('');
  $$('#tx-demos .btn').forEach(b => b.onclick = () => startDemo(+b.dataset.i));
  $('#btn-crash').onclick = txCrash;
  $('#btn-flush2').onclick = async () => { await doFlush(); await loadWal(); };
  $('#btn-ckpt2').onclick = async () => { await doCheckpoint(); await loadWal(); };
  $('#btn-reset').onclick = async () => {
    const r = await api('/api/reset', {});
    toast(r.ok ? r.message : r.error, r.ok ? 'good' : 'bad');
    S.recovery = null; renderRecovery(null); $('#tx-steps').className = 'steps muted'; $('#tx-steps').textContent = '-';
    await txAfter();
  };
}
