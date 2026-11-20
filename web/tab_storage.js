'use strict';
// ====================================================================== Storage: B+ tree, heap pages, database file
let TREE = null, DISK = null, PATH = [];

function treeLayout(nodes) {
  const byId = {};
  nodes.forEach(n => byId[n.id] = n);
  const maxLevel = Math.max.apply(null, nodes.map(n => n.level));
  const W = 96, H = 78;
  const leaves = nodes.filter(n => n.level === maxLevel);
  leaves.forEach((n, i) => n.x = i * W + W / 2);
  for (let l = maxLevel - 1; l >= 0; l--) {
    nodes.filter(n => n.level === l).forEach(n => {
      const cs = n.children.map(c => byId[c]).filter(Boolean);
      n.x = cs.length ? (cs[0].x + cs[cs.length - 1].x) / 2 : 0;
    });
  }
  nodes.forEach(n => n.y = n.level * H + 12);
  return {byId, maxLevel, W, H, width: Math.max(leaves.length, 1) * W + 10, height: (maxLevel + 1) * H + 20};
}

function keySummary(n) {
  if (!n.keys.length) return 'empty';
  if (n.keys.length <= 3) return n.keys.join(' ');
  return n.keys[0] + ' .. ' + n.keys[n.keys.length - 1] + ' (' + n.keys.length + ')';
}

function drawTree() {
  const el = $('#st-tree');
  if (!TREE || !TREE.ok) { el.innerHTML = '<div class="muted">' + esc(TREE ? TREE.error : 'no table selected') + '</div>'; return; }
  const nodes = clone(TREE.nodes);
  const L = treeLayout(nodes);
  const inPath = {};
  PATH.forEach(id => inPath[id] = true);
  let svg = '<svg width="' + L.width + '" height="' + L.height + '">';
  nodes.forEach(n => n.children.forEach(c => {
    const ch = L.byId[c];
    if (!ch) return;
    const on = inPath[n.id] && inPath[c];
    svg += '<line x1="' + n.x + '" y1="' + (n.y + 34) + '" x2="' + ch.x + '" y2="' + ch.y + '" stroke="' + (on ? '#fbbf24' : '#3a4a85') + '" stroke-width="' + (on ? 2.5 : 1.2) + '"/>';
  }));
  nodes.filter(n => n.leaf && n.next >= 0 && L.byId[n.next]).forEach(n => {
    const nx = L.byId[n.next];
    svg += '<line x1="' + (n.x + 42) + '" y1="' + (n.y + 17) + '" x2="' + (nx.x - 42) + '" y2="' + (nx.y + 17) + '" stroke="#2e9e72" stroke-dasharray="3 2"/>';
  });
  nodes.forEach(n => {
    const mem = n.frame >= 0, hot = inPath[n.id];
    const fill = n.leaf ? (mem ? '#14543f' : '#0f2b27') : (mem ? '#1b4a63' : '#10263a');
    svg += '<g><title>page ' + n.id + (n.leaf ? ' (leaf)' : ' (internal)') + ' keys: ' + n.keys.join(', ') + (mem ? ' - in frame ' + n.frame : ' - only on disk') + '</title>' +
      '<rect x="' + (n.x - 43) + '" y="' + n.y + '" width="86" height="34" rx="6" fill="' + fill + '" stroke="' + (hot ? '#fbbf24' : (mem ? '#7ee0b5' : '#35508a')) + '" stroke-width="' + (hot ? 3 : 1.4) + '"/>' +
      '<text x="' + n.x + '" y="' + (n.y + 13) + '" font-size="9.5" fill="#9fb2e8" text-anchor="middle">P' + n.id + (n.leaf ? ' leaf' : '') + '</text>' +
      '<text x="' + n.x + '" y="' + (n.y + 27) + '" font-size="10.5" fill="#fff" text-anchor="middle">' + esc(keySummary(n)) + '</text></g>';
  });
  el.innerHTML = svg + '</svg>';
}

function traceLookup() {
  if (!TREE || !TREE.ok) { toast('No index to search'); return; }
  const key = parseInt($('#st-key').value, 10);
  const byId = {};
  TREE.nodes.forEach(n => byId[n.id] = n);
  let n = byId[TREE.root];
  PATH = [];
  while (n) {
    PATH.push(n.id);
    if (n.leaf) break;
    const ci = n.keys.filter(k => key >= k).length;
    n = byId[n.children[ci]];
  }
  const leaf = byId[PATH[PATH.length - 1]];
  const idx = leaf ? leaf.keys.indexOf(key) : -1;
  const t = S.tables.find(x => x.name === $('#st-table').value);
  let h = 'Path: ' + PATH.map(id => '<b>P' + id + '</b>').join(' &rarr; ') + ' &nbsp; = <b>' + PATH.length + ' page accesses</b>. ';
  if (idx >= 0) h += 'Key ' + key + ' found, record id = (page ' + leaf.rids[idx][0] + ', slot ' + leaf.rids[idx][1] + '), so one more page read fetches the row. ';
  else h += 'Key ' + key + ' is not in the tree. ';
  if (t) h += 'A sequential scan would read <b>' + t.heap_pages + ' heap pages</b> instead.';
  $('#st-lookup-info').innerHTML = h;
  drawTree();
}

function pageTypeClass(t) { return {'heap': 'heap', 'index-leaf': 'leaf', 'index-node': 'node', 'catalog': 'meta', 'raw': 'raw'}[t] || 'free'; }

function drawDisk() {
  if (!DISK || !DISK.ok) return;
  $('#st-disk').innerHTML = DISK.pages.map(p => '<div class="dp ' + pageTypeClass(p.type) + (p.frame >= 0 ? ' mem' : '') + (p.dirty ? ' dirty' : '') + '" data-id="' + p.id +
    '" title="page ' + p.id + ' | ' + p.type + (p.owner ? ' | ' + p.owner : '') + (p.frame >= 0 ? ' | in frame ' + p.frame : ' | disk only') + '"></div>').join('');
  $$('#st-disk .dp').forEach(d => d.onclick = () => showPage(+d.dataset.id));
  const t = $('#st-table').value;
  const heap = DISK.pages.filter(p => p.type === 'heap' && p.owner === t);
  $('#st-heap').innerHTML = heap.length ? heap.map(p => {
    const fill = Math.round(((4076 - p.free) / 4076) * 100);
    return '<div class="blk' + (p.frame >= 0 ? ' mem' : '') + (p.dirty ? ' dirty' : '') + '" data-id="' + p.id + '" title="page ' + p.id + ': ' + p.live + ' rows, ' + p.free + ' bytes free"><b>P' + p.id + '</b><div class="bar"><i style="width:' + fill + '%"></i></div>' + p.live + ' rows</div>';
  }).join('') : '<span class="muted">no heap pages</span>';
  $$('#st-heap .blk').forEach(d => d.onclick = () => showPage(+d.dataset.id));
}

async function showPage(id) {
  const r = await api('/api/page?id=' + id);
  const el = $('#st-page');
  if (!r.ok) { el.innerHTML = '<div class="err">' + esc(r.error) + '</div>'; return; }
  let h = '<h3 style="margin-top:12px">Page ' + r.id + ' &mdash; ' + esc(r.type) + (r.owner ? ' of ' + esc(r.owner) : '') + '</h3>' +
    '<div class="hint">byte offset in file: ' + r.id * 4096 + ' &middot; ' + (r.frame >= 0 ? 'in RAM, frame ' + r.frame + (r.dirty ? ' (dirty)' : ' (clean)') : 'on disk only') + ' &middot; page LSN ' + r.lsn + '</div>';
  if (r.type === 'heap') {
    const dir = 20 + 4 * r.num_slots, free = r.free, rec = 4096 - dir - free;
    const seg = (w, c, t) => '<div style="width:' + (w / 4096 * 100) + '%;background:' + c + '" title="' + t + '">' + (w > 300 ? t : '') + '</div>';
    h += '<div class="pagebar">' + seg(dir, '#fbbf24', 'header + slot directory ' + dir + ' B') + seg(free, '#34d399', 'free ' + free + ' B') + seg(rec, '#38bdf8', 'records ' + rec + ' B') + '</div>';
    h += '<div class="hint">slot directory grows from the front, records grow from the back (slotted page). ' + r.live + ' of ' + r.num_slots + ' slots used. Next page in chain: ' + (r.next >= 0 ? 'P' + r.next : 'none') + '</div>';
    h += '<div class="scroll" style="max-height:230px"><table><tr><th>slot</th><th>length</th><th>decoded values</th></tr>' +
      r.slots.slice(0, 60).map(s => '<tr><td class="num">' + s.slot + '</td><td class="num">' + (s.used ? s.len : '-') + '</td><td>' + (s.used ? esc((s.values || []).join(' | ')) : '<span class="muted">deleted (free slot)</span>') + '</td></tr>').join('') + '</table></div>';
  }
  h += '<div class="hint">first 96 bytes (hex): </div><div class="hex">' + r.hex.replace(/(..)/g, '$1 ') + '</div>';
  el.innerHTML = h;
}

async function loadStorage() {
  await refreshTables();
  const sel = $('#st-table'), cur = sel.value;
  sel.innerHTML = S.tables.map(t => '<option>' + esc(t.name) + '</option>').join('');
  if (cur && S.tables.some(t => t.name === cur)) sel.value = cur;
  const t = $('#st-table').value;
  PATH = [];
  TREE = t ? await api('/api/tree?table=' + encodeURIComponent(t)) : null;
  DISK = await api('/api/disk');
  drawTree();
  drawDisk();
  if (!t) $('#st-tree').innerHTML = '<div class="muted">Create a table first (SQL Console tab).</div>';
}

function initStorage() {
  $('#btn-st-refresh').onclick = loadStorage;
  $('#st-table').onchange = loadStorage;
  $('#btn-lookup').onclick = traceLookup;
}
