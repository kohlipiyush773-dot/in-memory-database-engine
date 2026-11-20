'use strict';
// ====================================================================== Buffer pool view (OS paging inside the DBMS)
function frameHtml(f, flash, hand) {
  let cls = 'frame';
  if (f.p < 0) cls += ' empty'; else cls += f.dirty ? ' dirty' : ' clean';
  if (f.pin > 0) cls += ' pinned';
  if (flash) cls += ' fl-' + flash;
  const bits = [];
  if (f.dirty) bits.push('<span class="bit d" title="dirty bit: changed in RAM, disk copy is stale">D</span>');
  if (f.ref >= 0 && f.p >= 0) bits.push('<span class="bit r' + (f.ref ? '' : ' off') + '" title="reference bit (Clock)">R' + f.ref + '</span>');
  if (f.pin > 0) bits.push('<span class="bit p" title="pin count: in use, cannot be evicted">PIN' + f.pin + '</span>');
  return '<div class="' + cls + '" id="fr-' + f.f + '"><div class="fh">frame ' + f.f + '</div>' +
    '<div class="fp">' + (f.p < 0 ? '-' : 'P' + f.p) + '</div><div class="ft">' + esc(f.p < 0 ? 'empty' : (f.type || '')) + '</div>' +
    '<div class="bits">' + bits.join('') + '</div>' + (hand ? '<span class="hand" title="clock hand: next frame to examine">&#9654; hand</span>' : '') + '</div>';
}

function drawFrames(frames, flashFrame, flashType, hand) {
  const g = $('#pool-grid');
  g.className = 'grid' + (frames.length > 48 ? ' compact' : '');
  g.innerHTML = frames.map(f => frameHtml(f, f.f === flashFrame ? flashType : '', hand === f.f)).join('');
}

function renderPoolStats() {
  const p = S.pool, s = p.stats;
  const tile = (l, v) => '<div class="stat"><span>' + l + '</span><b>' + v + '</b></div>';
  const durable = p.wal.end ? (p.wal.flushed / p.wal.end) * 100 : 100;
  $('#pool-stats').innerHTML =
    '<div style="display:flex;gap:14px;align-items:center"><div class="big">' + pct(s.hit_ratio) + '</div><div style="flex:1"><div class="muted">buffer hit ratio (' + s.hits + ' hits / ' + (s.hits + s.misses) + ' accesses)</div><div class="meter"><i style="width:' + (s.hit_ratio * 100) + '%"></i></div></div></div>' +
    '<div class="stats">' + tile('page faults (misses)', s.misses) + tile('evictions', s.evictions) + tile('dirty write-backs', s.dirty_evictions) +
    tile('explicit flushes', s.flushes) + tile('WAL forces', s.wal_forces) + tile('new pages', s.new_pages) +
    tile('disk reads', p.disk.reads) + tile('disk writes', p.disk.writes) + tile('pages in file', p.disk.pages) + '</div>' +
    '<div class="hint" style="margin-top:10px">Write-ahead log: ' + p.wal.flushed + ' of ' + p.wal.end + ' bytes are durable (' + p.wal.records + ' records, ' + p.wal.forces + ' forced writes)</div>' +
    '<div class="meter"><i style="width:' + durable + '%;background:linear-gradient(90deg,#fbbf24,#34d399)"></i></div>';
}

function renderPageTable() {
  const rows = S.pool.frames.filter(f => f.p >= 0).sort((a, b) => a.p - b.p);
  if (!rows.length) { $('#pool-pt').innerHTML = '<div class="muted">the pool is empty</div>'; return; }
  $('#pool-pt').innerHTML = '<table><tr><th>page</th><th>frame</th><th>type</th><th>valid</th><th>dirty (modify bit)</th><th>reference bit</th><th>pin count</th><th>page LSN</th></tr>' +
    rows.map(f => '<tr><td class="num">' + f.p + '</td><td class="num">' + f.f + '</td><td>' + esc(f.type) + '</td><td class="ok">1</td><td class="' + (f.dirty ? 'bad' : '') + '">' + (f.dirty ? '1' : '0') + '</td><td>' + (f.ref >= 0 ? f.ref : 'n/a') + '</td><td class="num">' + f.pin + '</td><td class="num">' + f.lsn + '</td></tr>').join('') + '</table>';
}

function renderPool() {
  if (!S.pool) return;
  $('#pool-policy').value = S.pool.policy;
  if (document.activeElement !== $('#pool-frames')) $('#pool-frames').value = S.pool.size;
  drawFrames(S.pool.frames, -1, '', S.pool.policy === 'CLOCK' ? S.pool.hand : -1);
  renderPoolStats();
  renderPageTable();
}

function applyEvent(model, e) {
  const fr = model[e.f];
  if (!fr) return;
  if (e.t === 'EVICT') { fr.p = -1; fr.dirty = false; fr.type = ''; fr.pin = 0; fr.ref = -1; }
  else if (e.t === 'MISS') { fr.p = e.p; fr.dirty = false; fr.type = 'loading'; fr.pin = 1; }
  else if (e.t === 'NEW') { fr.p = e.p; fr.dirty = true; fr.type = 'new page'; fr.pin = 1; }
  else if (e.t === 'FLUSH') fr.dirty = false;
}

let replaying = false;
async function replayLast() {
  if (replaying) return;
  const ev = S.lastEvents || [];
  if (!ev.length || !S.poolBefore) { toast('Run a query or press a button first - then replay what it did'); return; }
  replaying = true;
  const speed = +$('#replay-speed').value;
  const model = clone(S.poolBefore.frames);
  let list = ev;
  const feed = $('#pool-feed');
  feed.className = 'feed';
  feed.innerHTML = '';
  if (list.length > 100) {
    list.slice(0, list.length - 100).forEach(e => applyEvent(model, e));
    feed.innerHTML = '<div class="muted">... ' + (list.length - 100) + ' earlier events applied instantly, animating the last 100 ...</div>';
    list = list.slice(-100);
  }
  drawFrames(model, -1, '');
  for (const e of list) {
    applyEvent(model, e);
    drawFrames(model, e.f, e.t);
    const d = document.createElement('div');
    d.innerHTML = '<span class="ev ' + e.t + '">' + e.t + '</span><span>' + esc(describeEvent(e)) + '</span>';
    feed.appendChild(d);
    feed.scrollTop = feed.scrollHeight;
    await sleep(speed);
  }
  replaying = false;
  renderPool();
}

async function applyConfig() {
  const policy = $('#pool-policy').value, frames = +$('#pool-frames').value;
  const r = await api('/api/config', {policy, frames});
  if (!r.ok) { toast(r.error, 'bad'); return; }
  toast('Now using ' + r.policy + ' with ' + r.size + ' frames (the old pool was flushed first)', 'good');
  await refreshStatus();
}

async function doFlush() {
  beginAction();
  const r = await api('/api/flush', {});
  endAction(r);
  toast(r.ok ? 'All dirty pages written to disk (' + r.io.disk_writes + ' writes, ' + r.io.wal_forces + ' log force)' : r.error, r.ok ? 'good' : 'bad');
  await refreshStatus();
}

async function doCheckpoint() {
  beginAction();
  const r = await api('/api/checkpoint', {});
  endAction(r);
  toast(r.ok ? r.message : r.error, r.ok ? 'good' : 'bad');
  await refreshStatus();
}

function initPool() {
  $('#btn-apply').onclick = applyConfig;
  $('#btn-flush').onclick = doFlush;
  $('#btn-ckpt').onclick = doCheckpoint;
  $('#btn-pool-refresh').onclick = refreshStatus;
  $('#btn-replay').onclick = replayLast;
  S.hooks['pool:status'] = () => { if (!replaying) renderPool(); };
}
