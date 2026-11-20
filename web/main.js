'use strict';
function showTab(name) {
  S.tab = name;
  $$('#tabs button').forEach(b => b.classList.toggle('on', b.dataset.tab === name));
  $$('.tab').forEach(t => t.classList.toggle('on', t.id === 'tab-' + name));
  if (name === 'pool') renderPool();
  if (name === 'storage') loadStorage();
  if (name === 'tx') { loadWal(); renderRecovery(S.recovery); }
}

async function init() {
  initSql(); initPool(); initStorage(); initOs(); initTx(); initBench(); initConcepts();
  $$('#tabs button').forEach(b => b.onclick = () => showTab(b.dataset.tab));
  await refreshStatus();
  await refreshTables();
  const info = await api('/api/info');
  if (info.ok && info.startup && info.startup.recovery && info.startup.recovery.ran) {
    S.recovery = info.startup.recovery;
    toast('The previous session did not end cleanly - crash recovery ran at start-up (redo ' + S.recovery.redo_applied + ', undo ' + S.recovery.undo_applied + ')', 'good');
  }
  renderHistory();
}
window.addEventListener('DOMContentLoaded', init);
