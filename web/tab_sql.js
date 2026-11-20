'use strict';
// ====================================================================== SQL console (DBMS: query layer)
const SAMPLES = [
  ['Point lookup (uses the B+ tree)', 'SELECT * FROM students WHERE id = 42'],
  ['Range scan (leaf chain)', 'SELECT * FROM students WHERE id >= 100 AND id < 110'],
  ['Full scan (no index on marks)', 'SELECT * FROM students WHERE marks > 95'],
  ['Aggregate', "SELECT COUNT(*) FROM students WHERE dept = 'CSE'"],
  ['Top 5 by marks', 'SELECT name, marks FROM students ORDER BY marks DESC LIMIT 5'],
  ['Update one row', 'UPDATE students SET marks = 100 WHERE id = 7'],
  ['Delete a range', 'DELETE FROM students WHERE id > 90 AND id <= 95'],
  ['Show the plan only', 'EXPLAIN SELECT * FROM students WHERE id = 5'],
  ['Create a table', 'CREATE TABLE notes (id INT PRIMARY KEY, body TEXT)'],
  ['Insert rows', "INSERT INTO notes VALUES (1,'hello'),(2,'world')"],
];

function splitSql(text) {
  text = text.split('\n').filter(l => !l.trim().startsWith('--')).join(' ');
  const out = [];
  let cur = '', q = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === "'") {
      if (q && text[i + 1] === "'") { cur += "''"; i++; continue; }
      q = !q; cur += c; continue;
    }
    if (c === ';' && !q) { if (cur.trim()) out.push(cur.trim()); cur = ''; continue; }
    cur += c;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

async function execSql(sql) {
  beginAction();
  const r = await api('/api/sql', {sql});
  endAction(r);
  return r;
}

function renderSqlOut(summaries) {
  const last = summaries[summaries.length - 1].r;
  let h = '';
  if (summaries.length > 1) {
    h += '<div class="muted" style="margin-bottom:6px">' + summaries.length + ' statements:</div>' + summaries.map(s =>
      '<div class="hint"><span class="' + (s.r.ok ? 'ok' : 'bad') + '">' + (s.r.ok ? 'ok' : 'error') + '</span> <code>' + esc(s.sql.slice(0, 90)) + '</code> - ' + esc(s.r.ok ? s.r.message : s.r.error) + '</div>').join('') + '<hr style="border:0;border-top:1px solid #27345f">';
  }
  if (!last.ok) h += '<div class="err"><b>Error:</b> ' + esc(last.error) + '</div>';
  else h += '<div><b class="ok">' + esc(last.kind) + '</b> &nbsp;' + esc(last.message) + '</div>';
  if (last.plan) h += '<div class="plan"><b>Query plan:</b> ' + esc(last.plan) + '</div>';
  if (last.ok) h += resultTable(last);
  h += '<h3 style="margin-top:12px">What this cost inside the engine</h3>' + ioChips(last);
  h += '<div style="margin-top:8px">' + eventSummary(last.events) + (last.events_truncated ? ' <span class="muted">(log truncated)</span>' : '') +
    ' <a href="#" id="go-pool" style="color:#8fb0ff">watch it in the Buffer Pool tab</a></div>';
  $('#sql-out').innerHTML = h;
  const go = $('#go-pool');
  if (go) go.onclick = e => { e.preventDefault(); showTab('pool'); };
}

async function runSql(text, explain) {
  const stmts = splitSql(text);
  if (!stmts.length) { toast('Type a statement first'); return; }
  const summaries = [];
  for (const s of stmts) {
    const q = explain && !/^explain/i.test(s) ? 'EXPLAIN ' + s : s;
    const r = await execSql(q);
    summaries.push({sql: q, r});
    S.history.unshift({sql: q, ok: r.ok});
    if (!r.ok) break;
  }
  S.history = S.history.slice(0, 40);
  renderSqlOut(summaries);
  renderHistory();
  await refreshStatus();
  await refreshTables();
}

function renderHistory() {
  const el = $('#sql-history');
  if (!S.history.length) { el.textContent = 'nothing yet'; return; }
  el.innerHTML = S.history.slice(0, 14).map((h, i) => '<div class="h ' + (h.ok ? '' : 'bad') + '" data-i="' + i + '" title="click to reuse">' + esc(h.sql) + '</div>').join('');
  $$('.h', el).forEach(d => d.onclick = () => { $('#sql-input').value = S.history[+d.dataset.i].sql; });
}

function renderTableList() {
  const el = $('#sql-tables');
  if (!S.tables.length) { el.innerHTML = '<div class="muted">No tables yet. Click <b>+100 students</b> to create demo data, or write CREATE TABLE.</div>'; return; }
  el.className = 'tablelist';
  el.innerHTML = S.tables.map(t => '<div class="t" data-t="' + esc(t.name) + '"><b>' + esc(t.name) + '</b><div>' +
    t.columns.map(c => esc(c.name) + ' ' + c.type + (c.pk ? ' PK' : '')).join(', ') + '</div><div>' + t.heap_pages + ' heap page(s)' +
    (t.root_page >= 0 ? ' &middot; B+ tree height ' + t.index_height + ' (' + t.index_nodes + ' pages)' : ' &middot; no index') + '</div></div>').join('');
  $$('.t', el).forEach(d => d.onclick = () => { $('#sql-input').value = 'SELECT * FROM ' + d.dataset.t + ' LIMIT 20'; runSql($('#sql-input').value); });
}

async function seed(n) {
  beginAction();
  const r = await api('/api/seed', {n});
  endAction(r);
  if (!r.ok) { toast(r.error, 'bad'); return; }
  toast(r.message, 'good');
  renderSqlOut([{sql: 'seed ' + n, r}]);
  await refreshStatus();
  await refreshTables();
}

function initSql() {
  $('#sql-samples').innerHTML = SAMPLES.map((s, i) => '<button class="chip" data-i="' + i + '" title="' + esc(s[0]) + '">' + esc(s[1]) + '</button>').join('');
  $$('#sql-samples .chip').forEach(b => b.onclick = () => { $('#sql-input').value = SAMPLES[+b.dataset.i][1]; });
  $('#btn-run').onclick = () => runSql($('#sql-input').value);
  $('#btn-explain').onclick = () => runSql($('#sql-input').value, true);
  $('#sql-input').addEventListener('keydown', e => { if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); runSql($('#sql-input').value); } });
  $('#btn-begin').onclick = () => runSql('BEGIN');
  $('#btn-commit').onclick = () => runSql('COMMIT');
  $('#btn-rollback').onclick = () => runSql('ROLLBACK');
  $('#btn-seed100').onclick = () => seed(100);
  $('#btn-seed1000').onclick = () => seed(1000);
  S.hooks.tables = renderTableList;
}
