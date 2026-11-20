'use strict';
// ====================================================================== OS lab: page replacement algorithms
const OS_PRESETS = [
  {name: 'Textbook string, 3 frames', refs: '7 0 1 2 0 3 0 4 2 3 0 3 2 1 2 0 1 7 0 1', frames: 3},
  {name: "Belady's anomaly string, 3 frames", refs: '1 2 3 4 1 2 5 1 2 3 4 5', frames: 3},
  {name: 'Cyclic scan (worst case for LRU)', refs: '1 2 3 4 1 2 3 4 1 2 3 4 1 2 3 4', frames: 3},
  {name: 'Hot page + one-time scan', refs: '1 2 1 3 1 4 1 5 1 6 1 7 1 8 1 9', frames: 3},
];

const OS_INFO = [
  ['FIFO', 'Evict the page that has been in memory the longest. Trivial to implement, but ignores how often a page is used and suffers from Belady\'s anomaly.'],
  ['LRU', 'Evict the page not used for the longest time (temporal locality). Needs an update on every access, so real kernels only approximate it.'],
  ['CLOCK (second chance)', 'Frames form a circle with a reference bit. The hand clears bits as it passes; a frame with bit 0 is evicted. Cheap approximation of LRU used by real operating systems and by PostgreSQL.'],
  ['OPT (Belady)', 'Evict the page whose next use is farthest in the future. Impossible online (needs the future) but it is the lower bound the other algorithms are measured against.'],
];

function parseRefsText(s) {
  const v = s.split(/[\s,]+/).filter(Boolean).map(x => Number(x));
  if (!v.length || v.some(x => !Number.isInteger(x) || x < 0 || x > 99999)) return null;
  return v;
}

function traceTable(res) {
  const st = res.steps, nf = res.frames;
  let h = '<div class="scroll"><table class="trace"><tr><th class="rowh">reference</th>' + st.map(s => '<th>' + s.p + '</th>').join('') + '</tr>';
  for (let f = 0; f < nf; f++) {
    h += '<tr><th class="rowh">frame ' + f + '</th>';
    st.forEach((s, i) => {
      const pg = s.frames[f], prev = i > 0 ? st[i - 1].frames[f] : -1;
      const fresh = pg >= 0 && pg !== prev;
      let t = pg < 0 ? '&middot;' : String(pg);
      if (res.policy === 'CLOCK' && pg >= 0 && s.ref[f] >= 0) t += '<span class="sup">' + s.ref[f] + '</span>';
      h += '<td class="' + (fresh ? 'new' : '') + '">' + t + '</td>';
    });
    h += '</tr>';
  }
  if (res.policy === 'CLOCK') h += '<tr><th class="rowh">clock hand &rarr;</th>' + st.map(s => '<td class="muted">f' + s.hand + '</td>').join('') + '</tr>';
  h += '<tr><th class="rowh">result</th>' + st.map(s => '<td class="' + (s.hit ? 'hit' : 'fault') + '">' + (s.hit ? 'hit' : 'FAULT') + '</td>').join('') + '</tr>';
  h += '<tr><th class="rowh">evicted</th>' + st.map(s => '<td class="muted">' + (s.ev >= 0 ? s.ev : '') + '</td>').join('') + '</tr></table></div>';
  if (res.policy === 'CLOCK') h += '<div class="hint">small superscript = reference bit of the page; a hit sets it to 1, the hand clears it to 0 when it passes (second chance).</div>';
  return h;
}

function traceSummary(res) {
  const n = res.hits + res.faults;
  return '<b style="color:' + POLICY_COLORS[res.policy] + '">' + res.policy + '</b> with ' + res.frames + ' frames: <b class="bad">' + res.faults + ' page faults</b>, ' + res.hits + ' hits (hit ratio ' + pct(res.hits / n) + ')';
}

async function fetchTrace(policy, frames, refs) {
  const r = await api('/api/trace', {policy, frames, refs});
  if (!r.ok) { toast(r.error, 'bad'); return null; }
  return r.result;
}

async function osRun() {
  const refs = parseRefsText($('#os-refs').value);
  if (!refs) { toast('Reference string: page numbers separated by spaces', 'bad'); return; }
  const res = await fetchTrace($('#os-policy').value, +$('#os-frames').value, refs);
  if (!res) return;
  $('#os-out').innerHTML = '<div class="card"><h3>Step-by-step trace</h3><div style="margin-bottom:8px">' + traceSummary(res) + '</div>' + traceTable(res) + '</div>';
}

async function osCompare() {
  const refs = parseRefsText($('#os-refs').value), frames = +$('#os-frames').value;
  if (!refs) { toast('Reference string: page numbers separated by spaces', 'bad'); return; }
  const all = await Promise.all(['FIFO', 'LRU', 'CLOCK', 'OPT'].map(p => fetchTrace(p, frames, refs)));
  if (all.some(x => !x)) return;
  const max = refs.length;
  let h = '<div class="card"><h3>Page faults, ' + refs.length + ' references, ' + frames + ' frames (lower is better)</h3>' +
    barRows(all.map(r => ({label: r.policy, value: r.faults, color: POLICY_COLORS[r.policy], text: r.faults + ' faults, ' + pct(r.hits / refs.length) + ' hits'})), {max}) + '</div>';
  h += '<div class="card">' + all.map((r, i) => '<details' + (i === 0 ? ' open' : '') + '><summary>' + traceSummary(r) + '</summary>' + traceTable(r) + '</details>').join('') + '</div>';
  $('#os-out').innerHTML = h;
}

async function osBelady() {
  const refs = parseRefsText($('#os-refs').value);
  if (!refs) { toast('Reference string: page numbers separated by spaces', 'bad'); return; }
  const r = await api('/api/belady', {refs});
  if (!r.ok) { toast(r.error, 'bad'); return; }
  const series = ['FIFO', 'LRU', 'CLOCK', 'OPT'].map(p => ({name: p, color: POLICY_COLORS[p], ys: r.series[p]}));
  let notes = '';
  series.forEach(s => {
    for (let i = 0; i + 1 < s.ys.length; i++) {
      if (s.ys[i + 1] > s.ys[i]) notes += '<div class="bad"><b>' + s.name + ' anomaly:</b> ' + s.ys[i] + ' faults with ' + (i + 1) + ' frames, but ' + s.ys[i + 1] + ' faults with ' + (i + 2) + ' frames. More memory made it worse!</div>';
    }
  });
  if (!notes) notes = '<div class="muted">No anomaly for this reference string. Try the "Belady\'s anomaly string" preset.</div>';
  $('#os-belady').innerHTML = '<div class="card"><h3>Page faults vs number of frames</h3>' +
    lineChart({title: 'Page faults for the reference string', xs: r.frames, series, yFmt: v => Math.round(v), xTitle: 'number of frames', yMax: Math.max.apply(null, [].concat.apply([], series.map(s => s.ys))) + 1}) +
    notes + '<div class="hint">LRU and OPT are <i>stack algorithms</i>: the pages in a smaller memory are always a subset of those in a bigger memory, so they can never show the anomaly. FIFO is not a stack algorithm.</div></div>';
}

function initOs() {
  $('#os-presets').innerHTML = OS_PRESETS.map((p, i) => '<button class="chip" data-i="' + i + '">' + esc(p.name) + '</button>').join('');
  $$('#os-presets .chip').forEach(b => b.onclick = () => {
    const p = OS_PRESETS[+b.dataset.i];
    $('#os-refs').value = p.refs; $('#os-frames').value = p.frames;
    osCompare();
  });
  $('#btn-trace').onclick = osRun;
  $('#btn-compare').onclick = osCompare;
  $('#btn-belady').onclick = osBelady;
  $('#os-info').innerHTML = OS_INFO.map(i => '<div class="card info"><h4 style="color:' + (POLICY_COLORS[i[0].split(' ')[0]] || '#fff') + '">' + i[0] + '</h4><p>' + i[1] + '</p></div>').join('');
}
