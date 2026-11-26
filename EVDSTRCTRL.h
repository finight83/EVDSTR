static const char EVDSTRCTRL_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EVDSTR</title>
<style>
  :root{--bg:#050507;--panel:#111116;--panel2:#1a1a21;--line:#32323c;
        --text:#f6f6f8;--muted:#a1a1ad;--accent:#9d7bff;--accenttext:#140b2c;
        --good:#37dba0;--warn:#ffc857;--bad:#ff5d5d}
  *{box-sizing:border-box}
  body{margin:0;font:14px/1.45 system-ui,sans-serif;color:var(--text);
       background:radial-gradient(circle at top right,#22124a,transparent 28rem),var(--bg)}
  header{display:flex;flex-wrap:wrap;gap:12px;align-items:center;padding:18px 22px;
         border-bottom:1px solid var(--line)}
  h1{font-size:1.35rem;margin:0;letter-spacing:-.03em}
  .pill{padding:3px 10px;border:1px solid var(--line);border-radius:999px;
        color:var(--muted);font-size:.8rem}
  .pill.ok{color:var(--good);border-color:#1f5b45}
  .pill.bad{color:var(--bad);border-color:#5b1f1f}
  .pill.warn{color:var(--warn);border-color:#5b4a1f}
  main{padding:22px}
  .grid{display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px}
  .card h3{margin:0 0 10px;font-size:.85rem;text-transform:uppercase;
           letter-spacing:.08em;color:var(--muted)}
  .kv{display:flex;justify-content:space-between;gap:10px;padding:3px 0}
  .kv span:last-child{font-variant-numeric:tabular-nums}
  input[type=range]{width:100%}
  input[type=text],select{width:100%;padding:8px;margin-top:4px;border-radius:9px;
    border:1px solid #343440;background:#09090c;color:var(--text)}
  select{padding:6px;margin-top:0}
  button.act{margin-top:10px;background:var(--accent);color:var(--accenttext);border:0;
             padding:9px 16px;border-radius:10px;font-weight:800;cursor:pointer}
  button.act.ghost{background:var(--panel2);color:var(--text);border:1px solid var(--line)}
  table{width:100%;border-collapse:collapse;font-size:.85rem}
  th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line)}
  th{color:var(--muted);font-weight:600;font-size:.75rem;text-transform:uppercase}
  pre{background:#09090c;border:1px solid var(--line);border-radius:10px;padding:12px;
      max-height:300px;overflow:auto;font-size:.78rem}
  .muted{color:var(--muted)}
  .row{display:flex;gap:10px;align-items:center}
  .row>*{flex:1 1 auto}
  #err{display:none;margin:0 22px 14px;padding:10px 12px;border-radius:10px;
       border:1px solid #5b1f1f;background:#1a0d0d;color:#ffb9b9;font-size:.85rem}
  #err.on{display:block}

    /* ==================================================================
       CLEAN THEME (palette pass) - matches the PC/server dashboards:
       deep indigo base, hairline surfaces, pill controls.
       ================================================================== */
    :root {
      --bg: #07070f;
      --panel: rgba(255,255,255,0.055);
      --line: rgba(255,255,255,0.10);
      --text: #f4f5fb;
      --muted: #b3b9d1;
      --accent: #9db6ff;
      --accenttext: #0b0a16;
      --good: #4ade80;
      --hair: rgba(255,255,255,0.10);
      --hair-strong: rgba(255,255,255,0.16);
    }
    body { background-color: #07070f; color: var(--text); }
    .card {
      background: var(--panel) !important;
      border: 1px solid var(--line) !important;
      border-radius: 18px !important;
      box-shadow: none !important;
    }
    .pill {
      background: rgba(255,255,255,0.06) !important;
      border: 1px solid var(--line) !important;
      border-radius: 999px !important;
    }
    button, .btn, .tab {
      border-radius: 999px !important;
      border: 1px solid var(--line) !important;
      background: rgba(255,255,255,0.06) !important;
      color: var(--text) !important;
      font-weight: 550 !important;
    }
    button:hover, .btn:hover, .tab:hover { background: rgba(255,255,255,0.12) !important; }
    button.active, .tab.active, .btn.active {
      background: rgba(255,255,255,0.16) !important; color: #fff !important;
    }
    input, select, textarea {
      background: rgba(0,0,0,0.25) !important;
      border: 1px solid var(--line) !important;
      border-radius: 10px !important;
      color: var(--text) !important;
    }

</style>
</head>
<body>
<header>
  <h1>EVDSTR</h1>
  <span class="pill" id="p-role">streamer</span>
  <span class="pill" id="p-ip">-</span>
  <span class="pill" id="p-subs">0 players</span>
  <span class="pill" id="p-disc">-</span>
  <span class="pill" id="p-link">link ?</span>
  <span class="pill" id="p-ver">-</span>
</header>

<div id="err"></div>

<main>
  <div class="grid">
    <div class="card"><h3>Stream</h3><div id="stream"></div></div>
    <div class="card"><h3>Pin link from EVDCTR</h3><div id="link"></div></div>
    <div class="card"><h3>Controller</h3>
      <p class="muted">The encoder and the input sources live on EVDCTR. Point
         this board at it so its dashboard relays to the right place.</p>
      <input id="ctrhost" type="text" placeholder="192.168.1.40">
      <button class="act" id="pair">Pair</button>
      <div id="pairstat" class="muted" style="margin-top:8px"></div>
    </div>
    <div class="card"><h3>Master volume</h3>
      <input id="vol" type="range" min="0" max="100" step="1" value="100">
      <div class="row"><span class="muted" id="volv">100</span>
        <button class="act ghost" id="mute">Mute</button></div>
      <h3 style="margin-top:14px">Buffer latency</h3>
      <input id="buf" type="range" min="20" max="600" step="10">
      <div class="muted" id="bufv"></div>
      <button class="act" id="bufapply">Apply to players</button>
    </div>
  </div>

  <div class="grid" style="margin-top:14px">
    <div class="card"><h3>Players</h3><table id="players">
      <thead><tr><th>Name</th><th>IP</th><th>Ver</th><th>Latency</th><th>Queue</th><th>Room</th><th>Compat</th></tr></thead>
      <tbody></tbody></table></div>
    <div class="card"><h3>Zones</h3>
      <div class="row"><input id="zonename" type="text" placeholder="New room name">
        <button class="act" id="zoneadd" style="flex:0 0 auto;margin-top:0">Add room</button></div>
      <pre id="zones" style="margin-top:10px">-</pre></div>
  </div>

  <div class="grid" style="margin-top:14px">
    <div class="card" style="grid-column:1/-1"><h3>Servers (servers decide)</h3>
      <div id="srv-role" class="muted" style="margin-bottom:8px">role …</div>
      <div id="serverList" style="display:flex;flex-direction:column;gap:6px"></div>
      <div class="row" style="margin-top:10px">
        <span class="muted" style="flex:0 0 auto">Priority (1 = highest)</span>
        <input id="srv-priority" type="number" min="1" max="99" style="flex:0 0 auto;width:80px">
        <button class="act" id="srv-apply" style="flex:0 0 auto;margin-top:0">Apply</button>
        <span class="muted" id="srv-state" style="flex:0 0 auto"></span>
      </div>
    </div>
  </div>
</main>

<script>
const $ = s => document.querySelector(s);
async function j(url, opt) {
  const r = await fetch(url, opt);
  const t = await r.text();
  try { return JSON.parse(t); } catch (e) { return { raw: t }; }
}
const post = (url, body) => j(url, { method: 'POST',
  headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) });
const kv = o => Object.entries(o)
  .map(([k, v]) => `<div class="kv"><span>${k}</span><span>${v ?? '-'}</span></div>`).join('');
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

// Every command used to fire and forget: a refused pairing or a board that
// restarted mid-request left the page unchanged and the operator guessing.
let errTimer = null;
function err(e) {
  const box = $('#err');
  if (!box) { console.warn(e); return; }
  box.textContent = (e && e.message) ? e.message : String(e);
  box.className = 'on';
  clearTimeout(errTimer);
  errTimer = setTimeout(() => { box.className = ''; }, 8000);
}
const txt = (url, body) => post(url, body)
  .then(r => { if (r && r.ok === false) err(r.error || 'request failed'); })
  .catch(err);

async function tick() {
  const s = await j('/api/state');
  if (!s || s.raw) {
    $('#p-link').textContent = 'no answer';
    $('#p-link').className = 'pill bad';
    return;
  }
  $('#p-ip').textContent = s.ip + ' (' + s.rssi + ' dBm)';
  $('#p-ver').textContent = 'STR ' + s.version;
  const n = typeof s.subscribers === 'number' ? s.subscribers : 0;
  $('#p-subs').textContent = n + ' player' + (n === 1 ? '' : 's');
  // 400 ms, the same threshold audioLinkTask uses (LINK_STALE_DROP_MS) and the
  // same one /api/state reports. A dashboard that disagrees with the audio task
  // about whether the link is alive is worse than no indicator.
  const linkOk = s.linkGapMs < 400;
  $('#p-link').textContent = linkOk ? 'link ok' : 'link idle ' + s.linkGapMs + ' ms';
  $('#p-link').className = 'pill ' + (linkOk ? 'ok' : 'bad');

  const kb = n => (typeof n === 'number' ? (n / 1000) + ' kHz' : '-');
  $('#stream').innerHTML = kv({
    Subscribers: s.subscribers,
    'Queue': (s.queueDepth ?? '-') + ' / ' + (s.queueTarget ?? '-'),
    'Pacer late': (s.pacerLateMs ?? '-') + ' ms',
    'Frames sent': s.framesSent,
    'Frames dropped': s.framesDropped,
    Encoder: kb(s.opusSampleRate),
    Buffer: (s.bufferMs ?? '-') + ' ms',
    Mode: s.mode
  });
  $('#link').innerHTML = kv({
    'Frames in': s.linkIn,
    'Last gap': s.linkGapMs + ' ms',
    Controller: s.ctr || ('searching\u2026 ' + (s.discoveryProbes ?? 0) + ' probes sent')
  });
  // A range under the finger must not be written back, and a board that has
  // not published a field yet must not blank the control with undefined.
  const focusId = document.activeElement ? document.activeElement.id : '';
  const idle = (sel, id) => { const el = $(sel); return el && !el.matches(':active') && focusId !== id; };
  if (idle('#vol', 'vol') && typeof s.volume === 'number') {
    $('#vol').value = s.volume; $('#volv').textContent = s.volume;
  }
  if (idle('#buf', 'buf') && typeof s.bufferMs === 'number') {
    $('#buf').value = s.bufferMs; $('#bufv').textContent = s.bufferMs + ' ms';
  }
  if (focusId !== 'ctrhost') $('#ctrhost').value = s.ctr || '';

  const z = await j('/api/zones').catch(() => null);
  const zones = Array.isArray(z) ? z : [];
  if (Array.isArray(z)) $('#zones').textContent = JSON.stringify(z, null, 1);

  const p = await j('/api/players').catch(() => null);
  if (Array.isArray(p)) {
    const opts = zones.map(zz => `<option value="${zz.id}">${zz.name}</option>`).join('');
    $('#players tbody').innerHTML = p.map(x => {
      return `<tr><td>${x.name}</td><td>${x.ip}</td><td>${x.version || '-'}</td>` +
        `<td>${x.latencyMs ?? '-'}</td><td>${x.queueDepth ?? '-'}</td>` +
        `<td><select class="roomsel" data-ip="${x.ip}">` +
          `<option value="">\u2014 Discovered (muted)</option>${opts}</select></td>` +
        `<td>${x.compatible ? 'ok' : (x.reason || 'no')}</td></tr>`;
    }).join('') || '<tr><td colspan="7" class="muted">no players yet</td></tr>';
    // Reflect the current assignment without firing change events.
    document.querySelectorAll('select.roomsel').forEach(sel => {
      const row = p.find(x => x.ip === sel.dataset.ip);
      sel.value = (row && row.zone) ? row.zone : '';
    });
    const disc = p.filter(x => !x.assigned).length;
    const pd = $('#p-disc');
    if (pd) { pd.textContent = disc + ' discovered'; pd.className = 'pill ' + (disc ? 'warn' : 'ok'); }
  }
  if (s.servers) renderServers(s);
}

// ---- Servers (P3: servers decide) -------------------------------------------
let serversData = [];
function renderServers(s) {
  serversData = s.servers || [];
  const isLeader = s.peerRole === 'leader';
  const leader = serversData.find(x => x.id === s.leaderId);
  const roleEl = $('#srv-role');
  if (roleEl) {
    roleEl.innerHTML = '<span class="pill ' + (isLeader ? 'ok' : 'warn') + '">' +
      (isLeader ? 'LEADER' : 'standby') + '</span> leader: ' +
      esc(leader ? (leader.self ? 'this server' : (leader.name || leader.id)) : '?');
  }
  const prioEl = $('#srv-priority');
  if (prioEl && document.activeElement !== prioEl) prioEl.value = s.priority;
  const list = $('#serverList');
  if (!list) return;
  list.innerHTML = serversData.map((sv, i) => {
    const down = sv.alive === false && !sv.self;
    const badge = sv.id === s.leaderId ? '<span class="pill ok">leader</span>'
                : (down ? '<span class="pill">down</span>' : '');
    return '<div class="srv-row" draggable="true" data-id="' + esc(sv.id) + '"' +
      ' ondragstart="srvDragStart(event)" ondragover="srvDragOver(event)"' +
      ' ondragleave="srvDragLeave(event)" ondrop="srvDrop(event)" ondragend="srvDragEnd(event)"' +
      ' style="display:flex;align-items:center;gap:10px;padding:7px 10px;border-radius:9px;' +
      'background:rgba(255,255,255,.03);border:1px solid #32323c;cursor:grab;' +
      (down ? 'opacity:.6;' : '') + '">' +
      '<span class="muted" style="width:16px;text-align:right">' + (i + 1) + '</span>' +
      '<strong>' + esc(sv.name || sv.id) + '</strong>' +
      (sv.self ? '<span class="pill">this</span>' : '') +
      '<span class="muted" style="font-size:.8rem">' + esc(sv.ip) + ' \u00b7 prio ' + sv.priority + '</span>' +
      badge + '</div>';
  }).join('') || '<p class="muted">no servers</p>';
}
let srvDragId = null;
window.srvDragStart = e => { srvDragId = e.currentTarget.dataset.id; e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', srvDragId); };
window.srvDragOver = e => { e.preventDefault(); e.currentTarget.style.borderColor = 'var(--accent)'; };
window.srvDragLeave = e => { e.currentTarget.style.borderColor = '#32323c'; };
window.srvDragEnd = e => { e.currentTarget.style.borderColor = '#32323c'; };
window.srvDrop = e => {
  e.preventDefault(); e.currentTarget.style.borderColor = '#32323c';
  const targetId = e.currentTarget.dataset.id;
  const srcId = srvDragId || e.dataTransfer.getData('text/plain');
  if (!srcId || srcId === targetId) return;
  const order = serversData.map(x => x.id);
  const from = order.indexOf(srcId), to = order.indexOf(targetId);
  if (from < 0 || to < 0) return;
  order.splice(from, 1); order.splice(to, 0, srcId);
  $('#srv-state').textContent = 'applying\u2026';
  post('/api/servers/order', { order }).then(r => {
    $('#srv-state').textContent = (r && r.ok) ? 'order applied' : 'failed';
    tick();
  }).catch(e => err(e));
};
$('#srv-apply').onclick = () => {
  const v = parseInt($('#srv-priority').value, 10);
  if (!v) return;
  post('/api/servers/priority', { priority: v }).then(r => {
    $('#srv-state').textContent = (r && r.ok) ? ('priority ' + r.priority) : 'failed';
    tick();
  }).catch(e => err(e));
};

$('#vol').oninput = e => $('#volv').textContent = e.target.value;
$('#vol').onchange = e => txt('/api/config', { masterVolume: +e.target.value });
$('#mute').onclick = async () => {
  try {
    const s = await j('/api/state');
    if (s.raw) { err('board did not answer'); return; }
    txt('/api/config', { masterMuted: !s.muted });
  } catch (e) { err(e); }
};
$('#buf').oninput = e => $('#bufv').textContent = e.target.value + ' ms';
$('#bufapply').onclick = () => txt('/api/config', { audioBufferMs: +$('#buf').value });
$('#pair').onclick = async () => {
  const host = $('#ctrhost').value.trim();
  if (!host) { $('#pairstat').textContent = 'enter the controller address first'; return; }
  $('#pairstat').textContent = 'checking ' + host + '\u2026';
  try {
    const r = await post('/api/link/pair', { host });
    $('#pairstat').textContent = r.ok ? 'Paired with ' + r.host : (r.error || 'failed');
    if (!r.ok) err('pairing failed: ' + (r.error || 'no answer'));
  } catch (e) { $('#pairstat').textContent = 'failed'; err(e); }
};

// Discovered -> room assignment (and back). The Room dropdown is rendered per
// player in tick(); assigning posts /api/zones/move, un-assigning posts
// /api/zones/remove-node, which mutes the node again.
document.addEventListener('change', e => {
  const sel = e.target.closest ? e.target.closest('select.roomsel') : null;
  if (!sel) return;
  const ip = sel.dataset.ip, zone = sel.value;
  if (zone) txt('/api/zones/move', { ip, zone });
  else      txt('/api/zones/remove-node', { ip });
});
$('#zoneadd').onclick = () => {
  const n = ($('#zonename').value || '').trim();
  if (!n) return;
  txt('/api/zones/create', { name: n });
  $('#zonename').value = '';
};

// Start the pollers last, and catch what they return: an unhandled rejection
// (board rebooting after an OTA) used to freeze the page with nothing in the
// console to say why.
tick().catch(e => console.warn('startup', e));
setInterval(() => { tick().catch(e => console.warn('tick', e)); }, 2000);
</script>
</body>
</html>
)HTML";
