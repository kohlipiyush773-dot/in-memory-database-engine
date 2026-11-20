'use strict';
// ====================================================================== Benchmark (hit ratio, disk I/O, effective access time)
const B_SIZES = [8, 16, 32, 64, 128, 256, 512];
const B_WL = [['seq', 'sequential (cyclic scan)'], ['random', 'uniform random'], ['zipf', 'Zipfian (skewed)'], ['hotcold', 'hot/cold 80/20']];
const B_POL = ['FIFO', 'LRU', 'CLOCK', 'OPT'];
let BENCH = null;

function checks(id, items, checked, name) {
  $(id).innerHTML = items.map(it => {
    const v = Array.isArray(it) ? it[0] : it, l = Array.isArray(it) ? it[1] : it;
    return '<label><input type="checkbox" name="' + name + '" value="' + v + '"' + (checked.indexOf(v) >= 0 ? ' checked' : '') + '> ' + esc(l) + '</label>';
  }).join('');
}
const picked = name => $$('input[name=' + name + ']:checked').map(i => i.value);

async function runBench() {
  const sizes = picked('bsz').map(Number), wl = picked('bwl'), pol = picked('bpol');
  if (!sizes.length || !wl.length || !pol.length) { toast('Select at least one size, workload and policy', 'bad'); return; }
  $('#b-status').innerHTML = '<span class="spin"></span>running ' + sizes.length * wl.length * pol.length + ' simulations on the real buffer pool...';
  $('#btn-bench').disabled = true;
  const r = await api('/api/bench', {pages: +$('#b-pages').value, ops: +$('#b-ops').value, write_pct: +$('#b-wp').value, sizes, workloads: wl, policies: pol});
  $('#btn-bench').disabled = false;
  if (!r.ok) { $('#b-status').textContent = ''; toast(r.error, 'bad'); return; }
  BENCH = r; S.lastBench = r;
  $('#b-status').textContent = r.results.length + ' runs finished';
  $('#btn-csv').disabled = false;
  drawBench();
}

function drawBench() {
  const r = BENCH.results;
  const wls = [...new Set(r.map(x => x.w))], pols = [...new Set(r.map(x => x.p))], sizes = [...new Set(r.map(x => x.f))].sort((a, b) => a - b);
  const wname = Object.fromEntries(B_WL);
  $('#bench-charts').innerHTML = wls.map(w => '<div class="card" id="bc-' + w + '"></div>').join('');
  wls.forEach(w => {
    const series = pols.map(p => ({name: p, color: POLICY_COLORS[p], dash: p === 'OPT', ys: sizes.map(s => { const x = r.find(y => y.w === w && y.p === p && y.f === s); return x ? x.hit_ratio : 0; })}));
    $('#bc-' + w).innerHTML = lineChart({title: 'Hit ratio - ' + (wname[w] || w), xs: sizes, series, yMax: 1, yFmt: v => Math.round(v * 100) + '%', xTitle: 'buffer pool size (frames) - database has ' + BENCH.config.pages + ' pages'});
  });
  // insight text
  const mid = sizes[Math.min(3, sizes.length - 1)];
  let ins = '<div class="card"><h3>What the numbers say (at ' + mid + ' frames)</h3>';
  wls.forEach(w => {
    const rows = r.filter(x => x.w === w && x.f === mid).sort((a, b) => b.hit_ratio - a.hit_ratio);
    ins += '<div><b>' + esc(wname[w] || w) + ':</b> ' + rows.map(x => x.p + ' ' + pct(x.hit_ratio)).join(' &gt; ') + '</div>';
  });
  ins += '<div class="hint">OPT is the unreachable optimum (it sees the future). Sequential scans larger than the pool give LRU, FIFO and Clock 0% hits, while skewed (Zipfian) access rewards recency-aware policies. Disk reads = misses; disk writes = dirty pages evicted.</div></div>';
  $('#bench-eat').innerHTML = ins + eatCard(wls, sizes, wname);
  const sel = $('#eat-wl');
  if (sel) { ['eat-wl', 'eat-size', 'eat-disk', 'eat-mem'].forEach(id => $('#' + id).onchange = drawEat); drawEat(); }
  let t = '<div class="card"><h3>Raw results</h3><div class="scroll"><table><tr><th>workload</th><th>policy</th><th>frames</th><th>hit ratio</th><th>hits</th><th>misses</th><th>evictions</th><th>disk reads</th><th>disk writes</th><th>time (ms)</th></tr>' +
    r.map(x => '<tr><td>' + x.w + '</td><td><span style="color:' + POLICY_COLORS[x.p] + '">' + x.p + '</span></td><td class="num">' + x.f + '</td><td class="num">' + pct(x.hit_ratio) + '</td><td class="num">' + x.hits + '</td><td class="num">' + x.misses + '</td><td class="num">' + x.evictions + '</td><td class="num">' + x.reads + '</td><td class="num">' + x.writes + '</td><td class="num">' + x.ms.toFixed(1) + '</td></tr>').join('') + '</table></div></div>';
  $('#bench-table').innerHTML = t;
}

function eatCard(wls, sizes, wname) {
  return '<div class="card"><h3>Effective access time (the OS formula)</h3><div class="btnrow wrap" style="margin-top:0">' +
    '<label>workload <select id="eat-wl">' + wls.map(w => '<option value="' + w + '">' + esc(wname[w] || w) + '</option>').join('') + '</select></label>' +
    '<label>frames <select id="eat-size">' + sizes.map(s => '<option>' + s + '</option>').join('') + '</select></label>' +
    '<label>backing store <select id="eat-disk"><option value="100">SSD (100 us)</option><option value="8000">hard disk (8 ms)</option><option value="20">NVMe (20 us)</option></select></label>' +
    '<label>memory access (ns) <input id="eat-mem" type="number" value="100" style="width:80px"></label></div>' +
    '<div class="hint">EAT = (1 - p) x memory access time + p x page-fault service time, where p is the miss ratio measured above.</div><div id="eat-out"></div></div>';
}

function drawEat() {
  if (!BENCH) return;
  const w = $('#eat-wl').value, f = +$('#eat-size').value, disk = +$('#eat-disk').value * 1000, mem = +$('#eat-mem').value;
  const rows = BENCH.results.filter(x => x.w === w && x.f === f);
  const items = rows.map(x => { const p = 1 - x.hit_ratio; return {p: x.p, eat: (1 - p) * mem + p * disk, hit: x.hit_ratio}; });
  const max = Math.max.apply(null, items.map(i => i.eat));
  const base = (items.find(i => i.p === 'FIFO') || items[0]).eat;
  $('#eat-out').innerHTML = barRows(items.map(i => ({label: i.p, value: i.eat, color: POLICY_COLORS[i.p], text: (i.eat / 1000).toFixed(2) + ' us  (' + pct(i.hit) + ' hits, ' + (base / i.eat).toFixed(2) + 'x vs FIFO)'})), {max});
}

function benchCsv() {
  if (!BENCH) return;
  const head = 'workload,policy,frames,ops,hits,misses,hit_ratio,evictions,disk_reads,disk_writes,ms';
  const lines = BENCH.results.map(x => [x.w, x.p, x.f, x.ops, x.hits, x.misses, x.hit_ratio, x.evictions, x.reads, x.writes, x.ms].join(','));
  downloadText('pagedb_benchmark.csv', head + '\n' + lines.join('\n') + '\n', 'text/csv');
}

// ---- index vs sequential scan, small vs large buffer pool
async function runIvs() {
  const st = $('#ivs-status');
  st.innerHTML = '<span class="spin"></span>preparing data...';
  const c = await api('/api/sql', {sql: 'SELECT COUNT(*) FROM students'});
  const have = c.ok ? c.rows[0][0] : 0;
  if (have < 2000) {
    const s = await api('/api/seed', {n: 2000 - have});
    if (!s.ok) { st.textContent = ''; toast(s.error, 'bad'); return; }
  }
  await refreshStatus();
  const orig = {policy: S.pool.policy, frames: S.pool.size};
  const q = async sql => api('/api/sql', {sql});
  const out = [];
  const setCfg = async n => api('/api/config', {policy: orig.policy, frames: n});
  await setCfg(64);
  const idx = await q('SELECT * FROM students WHERE id = 1234');
  const scan1 = await q('SELECT * FROM students WHERE marks = 77');
  const scan2 = await q('SELECT * FROM students WHERE marks = 77');
  out.push({size: 64, a: scan1, b: scan2});
  await setCfg(8);
  const s1 = await q('SELECT * FROM students WHERE marks = 77');
  const s2 = await q('SELECT * FROM students WHERE marks = 77');
  out.push({size: 8, a: s1, b: s2});
  await api('/api/config', {policy: orig.policy, frames: orig.frames});
  await refreshStatus();
  st.textContent = '';
  const touched = r => r.io.hits + r.io.misses;
  let h = '<h3 style="margin-top:12px">A. Index lookup vs full table scan (cold cache, 64 frames)</h3>' + barRows([
    {label: 'B+ tree lookup', value: touched(idx), color: '#34d399', text: touched(idx) + ' pages, ' + idx.elapsed_us + ' us'},
    {label: 'sequential scan', value: touched(scan1), color: '#f87171', text: touched(scan1) + ' pages, ' + scan1.elapsed_us + ' us'}]) +
    '<div class="plan">' + esc(idx.plan) + '</div><div class="plan">' + esc(scan1.plan) + '</div>';
  h += '<h3 style="margin-top:14px">B. Scan the same table twice: does the buffer pool help?</h3><table><tr><th>pool</th><th>1st scan: misses (disk reads)</th><th>2nd scan: hits</th><th>2nd scan: misses (disk reads)</th><th>2nd scan time</th></tr>' +
    out.map(o => '<tr><td>' + o.size + ' frames</td><td class="num">' + o.a.io.misses + ' (' + o.a.io.disk_reads + ')</td><td class="num ok">' + o.b.io.hits + '</td><td class="num ' + (o.b.io.misses ? 'bad' : 'ok') + '">' + o.b.io.misses + ' (' + o.b.io.disk_reads + ')</td><td class="num">' + o.b.elapsed_us + ' us</td></tr>').join('') + '</table>' +
    '<div class="hint">With 64 frames the whole table fits, so the second scan is served entirely from RAM. With 8 frames a scan larger than the pool evicts every page before it is needed again (sequential flooding): LRU gets no benefit and every access is a page fault. This is exactly the cache-size / working-set effect from the OS course.</div>';
  $('#ivs-out').innerHTML = h;
}

function initBench() {
  checks('#b-sizes', B_SIZES, [8, 16, 32, 64, 128, 256], 'bsz');
  checks('#b-wl', B_WL, ['seq', 'random', 'zipf', 'hotcold'], 'bwl');
  checks('#b-pol', B_POL, B_POL, 'bpol');
  $('#btn-bench').onclick = runBench;
  $('#btn-csv').onclick = benchCsv;
  $('#btn-ivs').onclick = runIvs;
}
