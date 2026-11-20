'use strict';
// ------------------------------------------------------------------ tiny helpers
const $ = (s, r) => (r || document).querySelector(s);
const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
const sleep = ms => new Promise(r => setTimeout(r, ms));
const clone = o => JSON.parse(JSON.stringify(o));
const POLICY_COLORS = {FIFO: '#f59e0b', LRU: '#38bdf8', CLOCK: '#a78bfa', OPT: '#34d399'};
const pct = v => (v * 100).toFixed(1) + '%';

async function api(path, body) {
  const opt = body === undefined ? {} : {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)};
  try {
    const r = await fetch(path, opt);
    return await r.json();
  } catch (e) {
    return {ok: false, error: 'cannot reach the server: ' + e.message};
  }
}

function toast(msg, kind) {
  const t = document.createElement('div');
  t.className = 'toast ' + (kind || '');
  t.textContent = msg;
  $('#toasts').appendChild(t);
  setTimeout(() => t.remove(), 4200);
}

// global UI state shared by all tabs
const S = {pool: null, poolBefore: null, tables: [], lastEvents: [], history: [], recovery: null, tab: 'sql', hooks: {}, lastBench: null};

// remember the pool BEFORE an action so the Buffer Pool tab can replay what the action did
function beginAction() { if (S.pool) S.poolBefore = clone(S.pool); }
function endAction(r) { if (r && r.events) S.lastEvents = r.events; }

async function refreshStatus() {
  const p = await api('/api/pool');
  if (!p.ok) return;
  S.pool = p;
  $('#hdr-policy').textContent = 'policy ' + p.policy;
  $('#hdr-frames').textContent = p.size + ' frames';
  $('#hdr-hit').textContent = 'hit ratio ' + pct(p.stats.hit_ratio);
  const w = p.wal;
  $('#hdr-wal').textContent = 'WAL ' + w.flushed + ' / ' + w.end + ' B durable';
  $('#hdr-wal').className = 'pill' + (w.flushed < w.end ? ' warn' : ' good');
  const t = $('#hdr-txn');
  if (p.txn.active) { t.textContent = 'txn ' + p.txn.id + ' open'; t.className = 'pill warn'; }
  else { t.textContent = 'no open txn'; t.className = 'pill'; }
  if (S.hooks[S.tab + ':status']) S.hooks[S.tab + ':status']();
}

async function refreshTables() {
  const r = await api('/api/tables');
  if (r.ok) S.tables = r.tables;
  if (S.hooks.tables) S.hooks.tables();
  return S.tables;
}

// ------------------------------------------------------------------ results rendering (used by several tabs)
function resultTable(r, maxRows) {
  maxRows = maxRows || 200;
  if (!r.columns || !r.columns.length) return '';
  let h = '<div class="scroll"><table><tr>' + r.columns.map(c => '<th>' + esc(c) + '</th>').join('') + '</tr>';
  r.rows.slice(0, maxRows).forEach(row => {
    h += '<tr>' + row.map(v => '<td' + (typeof v === 'number' ? ' class="num"' : '') + '>' + esc(v) + '</td>').join('') + '</tr>';
  });
  h += '</table></div>';
  if (r.row_count > Math.min(maxRows, r.rows.length)) h += '<div class="hint">showing the first ' + Math.min(maxRows, r.rows.length) + ' of ' + r.row_count + ' rows</div>';
  return h;
}

function ioChips(r) {
  const io = r.io || {};
  const chip = (l, v, cls) => '<span class="chip stat"><span>' + l + '</span><b class="' + (cls || '') + '">' + v + '</b></span>';
  return '<div class="chips">' +
    chip('buffer hits', io.hits, 'ok') + chip('misses', io.misses, io.misses ? 'bad' : '') + chip('evictions', io.evictions) +
    chip('dirty writes on eviction', io.dirty_evictions) + chip('disk reads', io.disk_reads) + chip('disk writes', io.disk_writes) +
    chip('WAL bytes', io.wal_bytes) + chip('log forces', io.wal_forces) + chip('time', (r.elapsed_us || 0) + ' us') + '</div>';
}

// ------------------------------------------------------------------ charts (plain SVG, no libraries)
function lineChart(o) {
  const W = o.w || 560, H = o.h || 280, m = {l: 52, r: 14, t: 34, b: 42};
  const iw = W - m.l - m.r, ih = H - m.t - m.b, n = o.xs.length;
  const all = [];
  o.series.forEach(s => s.ys.forEach(v => all.push(v)));
  const ymax = o.yMax != null ? o.yMax : Math.max.apply(null, all.concat([1e-9])) * 1.1;
  const fmt = o.yFmt || (v => (Math.round(v * 100) / 100));
  const X = i => n > 1 ? m.l + i * iw / (n - 1) : m.l + iw / 2;
  const Y = v => m.t + ih - (v / ymax) * ih;
  let svg = '<svg viewBox="0 0 ' + W + ' ' + H + '" width="100%" style="max-width:' + W + 'px">';
  svg += '<text x="' + m.l + '" y="16" fill="#e8edff" font-size="13" font-weight="700">' + esc(o.title || '') + '</text>';
  for (let k = 0; k <= 4; k++) {
    const v = ymax * k / 4, y = Y(v);
    svg += '<line x1="' + m.l + '" x2="' + (W - m.r) + '" y1="' + y + '" y2="' + y + '" stroke="#1f2b52"/>';
    svg += '<text x="' + (m.l - 6) + '" y="' + (y + 4) + '" fill="#8d99c4" font-size="10" text-anchor="end">' + esc(fmt(v)) + '</text>';
  }
  o.xs.forEach((x, i) => {
    svg += '<text x="' + X(i) + '" y="' + (H - m.b + 15) + '" fill="#8d99c4" font-size="10" text-anchor="middle">' + esc(x) + '</text>';
  });
  if (o.xTitle) svg += '<text x="' + (m.l + iw / 2) + '" y="' + (H - 6) + '" fill="#8d99c4" font-size="10.5" text-anchor="middle">' + esc(o.xTitle) + '</text>';
  let lx = W - m.r;
  o.series.slice().reverse().forEach(s => {
    const w = s.name.length * 7 + 22;
    lx -= w;
    svg += '<rect x="' + lx + '" y="8" width="10" height="10" rx="2" fill="' + s.color + '"/><text x="' + (lx + 14) + '" y="17" fill="#c9d3ff" font-size="11">' + esc(s.name) + '</text>';
  });
  o.series.forEach(s => {
    const pts = s.ys.map((v, i) => X(i) + ',' + Y(v)).join(' ');
    svg += '<polyline points="' + pts + '" fill="none" stroke="' + s.color + '" stroke-width="2.2"' + (s.dash ? ' stroke-dasharray="5 4"' : '') + '/>';
    s.ys.forEach((v, i) => {
      svg += '<circle cx="' + X(i) + '" cy="' + Y(v) + '" r="3.3" fill="' + s.color + '"><title>' + esc(s.name + ' @ ' + o.xs[i] + ': ' + fmt(v)) + '</title></circle>';
    });
  });
  return svg + '</svg>';
}

function barRows(items, o) {
  o = o || {};
  const max = o.max != null ? o.max : Math.max.apply(null, items.map(i => i.value).concat([1e-9]));
  return '<div class="bars">' + items.map(i =>
    '<div class="row"><span>' + esc(i.label) + '</span><div class="track"><i style="width:' + Math.max(1, (i.value / max) * 100) + '%;background:' + (i.color || '#6c8cff') + '"></i></div><span class="muted">' + esc(i.text != null ? i.text : i.value) + '</span></div>'
  ).join('') + '</div>';
}

function describeEvent(e) {
  switch (e.t) {
    case 'HIT': return 'page ' + e.p + ' already in frame ' + e.f + ' - no disk access';
    case 'MISS': return 'page fault: page ' + e.p + ' read from disk into frame ' + e.f + (e.o >= 0 ? ' (replaced page ' + e.o + ')' : ' (free frame)');
    case 'NEW': return 'new page ' + e.p + ' created in frame ' + e.f + (e.o >= 0 ? ' (replaced page ' + e.o + ')' : '');
    case 'EVICT': return 'page ' + e.p + ' evicted from frame ' + e.f + (e.d ? ' - it was DIRTY, written to disk first' : ' - it was clean, simply dropped');
    case 'FLUSH': return 'page ' + e.p + ' (frame ' + e.f + ') written to disk';
    case 'WAL': return 'write-ahead rule: log forced to disk before page ' + e.p + ' may be written';
  }
  return e.t;
}

function eventSummary(events) {
  const c = {};
  (events || []).forEach(e => { c[e.t] = (c[e.t] || 0) + 1; });
  const keys = ['HIT', 'MISS', 'EVICT', 'NEW', 'FLUSH', 'WAL'].filter(k => c[k]);
  if (!keys.length) return '<span class="muted">no buffer activity</span>';
  return keys.map(k => '<span class="ev ' + k + '">' + k + ' x' + c[k] + '</span>').join(' ');
}

function downloadText(name, text, mime) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], {type: mime || 'text/plain'}));
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
}
