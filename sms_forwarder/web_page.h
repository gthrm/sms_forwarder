#pragma once
// Web panel page, served at "/". Talks to the /api/* endpoints in sms_forwarder.ino.

static const char WEB_PAGE[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SMS → Telegram</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--text:#1d1f23;--muted:#6b7280;--line:#e5e7eb;--accent:#2563eb;--ok:#16a34a;--warn:#d97706;--bad:#dc2626;--field:#f9fafb}
@media (prefers-color-scheme:dark){:root{--bg:#111316;--card:#1b1e23;--text:#e6e8eb;--muted:#9aa1ab;--line:#2b2f36;--accent:#60a5fa;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171;--field:#15181c}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 -apple-system,system-ui,"Segoe UI",Roboto,sans-serif}
main{max-width:980px;margin:0 auto;padding:16px}
header{display:flex;align-items:baseline;justify-content:space-between;gap:12px;margin:4px 0 16px}
h1{font-size:20px;margin:0}
h2{font-size:13px;margin:0 0 12px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.05em}
.grid{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}
section.card+section.card,.grid+section.card,section.card+.grid{margin-top:16px}
.kv{display:grid;grid-template-columns:auto 1fr;gap:6px 12px;margin:0}
.kv dt{color:var(--muted)}.kv dd{margin:0;text-align:right;overflow-wrap:anywhere}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.muted{color:var(--muted)}
.msg{border-top:1px solid var(--line);padding:10px 0}.msg:first-child{border-top:0;padding-top:0}
.meta{display:flex;flex-wrap:wrap;align-items:center;gap:4px 12px;font-size:13px;color:var(--muted)}
.meta b{color:var(--text);font-weight:600}
.text{white-space:pre-wrap;overflow-wrap:anywhere;margin-top:4px}
.badge{font-size:12px;padding:0 8px;border-radius:999px;border:1px solid currentColor}
form{display:grid;gap:8px}
input,textarea,select{width:100%;padding:9px 10px;border:1px solid var(--line);border-radius:8px;background:var(--field);color:var(--text);font:inherit}
textarea{min-height:90px;resize:vertical}
button{padding:8px 14px;border:0;border-radius:8px;background:var(--accent);color:#fff;font:inherit;cursor:pointer}
button:disabled{opacity:.6;cursor:wait}
button.secondary{background:transparent;color:var(--text);border:1px solid var(--line)}
button.danger{background:var(--bad)}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.out{white-space:pre-wrap;overflow-wrap:anywhere;font-size:14px}
.out:empty{display:none}
table{width:100%;border-collapse:collapse;font-size:14px}
td,th{text-align:left;padding:6px 4px;border-top:1px solid var(--line);overflow-wrap:anywhere}
th{color:var(--muted);font-weight:500;border-top:0}
.history{max-height:560px;overflow:auto}
.rule-form{display:grid;grid-template-columns:minmax(140px,auto) 1fr 1fr auto;gap:8px;margin-top:12px}
@media (max-width:640px){.rule-form{grid-template-columns:1fr}}
td.actions{white-space:nowrap;text-align:right}
td.actions button{padding:2px 8px;margin-left:4px}
code{background:var(--field);border:1px solid var(--line);border-radius:4px;padding:0 4px}
</style>
</head>
<body>
<main>
<header><h1>SMS → Telegram</h1><span class="muted" id="updated"></span></header>

<div class="grid">
  <section class="card"><h2>Modem</h2><dl class="kv" id="modem"></dl></section>
  <section class="card"><h2>WiFi and board</h2><dl class="kv" id="sys"></dl></section>
</div>

<section class="card"><h2>Forwarding rules</h2>
  <div id="rules"></div>
  <form class="rule-form" id="ruleForm">
    <select name="type">
      <option value="STARTS_WITH">Starts with</option>
      <option value="CONTAINS">Contains</option>
      <option value="FROM">Sender</option>
    </select>
    <input name="pattern" placeholder="[work]:" required autocomplete="off">
    <input name="chat" placeholder="Chat ID (-100…)" required autocomplete="off">
    <button>Add</button>
  </form>
  <div class="out" id="ruleOut" style="margin-top:8px"></div>
</section>

<section class="card"><h2>Messages</h2><div class="history" id="history"></div></section>

<div class="grid" style="margin-top:16px">
  <section class="card"><h2>Send SMS</h2>
    <form id="smsForm">
      <input name="to" placeholder="Number in international format, with +" inputmode="tel" required>
      <textarea name="text" placeholder="Text" required></textarea>
      <div class="row"><button>Send</button><span class="muted" id="smsCount"></span></div>
      <div class="out" id="smsOut"></div>
    </form>
  </section>
  <section class="card"><h2>USSD</h2>
    <form id="ussdForm">
      <input name="code" placeholder="*100#" inputmode="tel" required>
      <div class="row"><button>Send</button><button type="button" class="secondary" id="ussdCancel">Close menu</button></div>
      <div class="out" id="ussdOut"></div>
    </form>
  </section>
</div>

<div class="grid">
  <section class="card"><h2>Telegram</h2>
    <dl class="kv" id="tg"></dl>
    <form id="cfgForm" style="margin-top:12px">
      <input name="token" placeholder="New bot token (leave empty to keep)" autocomplete="off">
      <input name="chat" placeholder="New default chat ID (leave empty to keep)" autocomplete="off">
      <div class="row"><button>Save</button><button type="button" class="secondary" id="testBtn">Test message</button></div>
      <div class="out" id="cfgOut"></div>
    </form>
  </section>
  <section class="card"><h2>Board</h2>
    <div class="row"><button class="secondary" id="rebootBtn">Reboot</button><button class="danger" id="resetBtn">Factory reset</button></div>
    <div class="out" id="sysOut" style="margin-top:8px"></div>
  </section>
</div>
</main>

<script>
const $ = s => document.querySelector(s);
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const kv = (el, rows) => el.innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
const dot = state => `<span class="dot" style="background:var(--${state})"></span>`;
const yes = b => b ? 'ok' : 'bad';
function dur(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + 'd ' : '') + (h ? h + 'h ' : '') + m + 'm';
}
function signal(csq) {
  if (csq < 0 || csq === 99) return dot('bad') + 'none';
  const [w, c] = csq < 10 ? ['weak', 'bad'] : csq < 15 ? ['fair', 'warn'] : csq < 20 ? ['good', 'ok'] : ['excellent', 'ok'];
  return `${dot(c)}${w} (${csq}/31)`;
}
const STATUS = {sent: ['forwarded', 'ok'], fallback: ['sent to default chat', 'warn'], pending: ['waiting to send', 'warn'], failed: ['failed', 'bad']};
const RULE = {STARTS_WITH: 'Starts with', CONTAINS: 'Contains', FROM: 'Sender'};
let lastRules = '';

async function post(url, data) {
  try {
    const r = await fetch(url, {method: 'POST', body: new URLSearchParams(data || {})});
    return await r.json();
  } catch (e) {
    return {ok: false, error: 'cannot reach the board'};
  }
}

async function busy(btn, out, fn) {
  btn.disabled = true;
  try { await fn(); } finally { btn.disabled = false; }
}

async function refresh() {
  try {
    const [s, h] = await Promise.all([fetch('/api/status').then(r => r.json()), fetch('/api/history').then(r => r.json())]);
    const m = s.modem;
    kv($('#modem'), [
      ['Modem', m.ready ? dot('ok') + 'running' : dot('bad') + 'not started'],
      ['SIM', dot(yes(m.sim)) + (m.sim ? 'ready' : 'missing')],
      ['Network', dot(yes(m.registered)) + (m.registered ? 'registered' : 'not registered')],
      ['Operator', esc(m.operator || '—')],
      ['Signal', signal(m.csq)],
      ['SMS on SIM', m.simTotal ? `${m.simUsed} of ${m.simTotal}` : '—'],
      ['Network time', esc(m.clock || '—')],
    ]);
    kv($('#sys'), [
      ['WiFi', dot(yes(s.wifi.connected)) + esc(s.wifi.ssid)],
      ['WiFi signal', s.wifi.rssi + ' dBm'],
      ['Address', `${esc(s.wifi.host)}<br><span class="muted">${esc(s.wifi.ip)}</span>`],
      ['Uptime', dur(s.uptime)],
      ['Last reset reason', esc(s.resetReason)],
      ['Forwarded / failed attempts', `${s.stats.forwarded} / ${s.stats.failed}`],
      ['Free memory', Math.round(s.heap / 1024) + ' KB'],
      ['Last error', s.lastError ? `<span class="bad">${esc(s.lastError)}</span><br><span class="muted">${dur(s.lastErrorAgo)} ago</span>` : '—'],
    ]);
    kv($('#tg'), [['Bot', esc(s.tg.token)], ['Default chat', esc(s.tg.chat)]]);
    const rulesKey = JSON.stringify(s.rules);
    if (rulesKey !== lastRules) {  // don't re-render while nothing changed, so buttons don't flicker
      lastRules = rulesKey;
      const n = s.rules.length;
      $('#rules').innerHTML = (n
        ? `<table><tr><th>#</th><th>Condition</th><th>Value</th><th>Chat</th><th></th></tr>${s.rules.map((r, i) =>
            `<tr><td class="muted">${i + 1}</td><td>${RULE[r.type]}</td><td><code>${esc(r.pattern)}</code></td><td>${esc(r.chat)}</td>
             <td class="actions"><button class="secondary" data-move="up" data-i="${i}" ${i ? '' : 'disabled'} title="Move up">↑</button><button class="secondary" data-move="down" data-i="${i}" ${i < n - 1 ? '' : 'disabled'} title="Move down">↓</button><button class="secondary" data-del="${i}" title="Delete">✕</button></td></tr>`).join('')}</table>`
        : '<p class="muted" style="margin:0">No rules yet — everything goes to the default chat.</p>') +
        `<p class="muted">Checked top to bottom, the first match wins. Everything else goes to the default chat (${esc(s.tg.chat)}).
         Example: a “Starts with” rule <code>[work]:</code> sends the SMS “[work]: hello” to its chat as “hello”.</p>`;
    }
    $('#history').innerHTML = h.length ? h.map(x => {
      const [label, cls] = STATUS[x.status] || [x.status, ''];
      const where = x.out ? 'outgoing' : (x.chat ? '→ ' + esc(x.chat) : '');
      return `<div class="msg"><div class="meta"><b>${x.out ? '↑ ' : '↓ '}${esc(x.peer)}</b>
        <span>${esc(x.time)}</span><span>${where}</span><span class="badge ${cls}">${label}</span></div>
        <div class="text">${esc(x.text)}</div></div>`;
    }).join('') : '<p class="muted">Nothing yet. The last 50 SMS since the board started will show up here.</p>';
    $('#updated').textContent = 'updated ' + new Date().toLocaleTimeString();
  } catch (e) {
    $('#updated').textContent = 'cannot reach the board';
  }
}

const ruleForm = $('#ruleForm');
const PLACEHOLDER = {STARTS_WITH: '[work]:', CONTAINS: 'code', FROM: 'sender name or number'};
ruleForm.type.addEventListener('change', () => ruleForm.pattern.placeholder = PLACEHOLDER[ruleForm.type.value]);
ruleForm.addEventListener('submit', e => {
  e.preventDefault();
  busy(ruleForm.querySelector('button'), null, async () => {
    $('#ruleOut').textContent = 'Checking the chat — sending a test message there…';
    const j = await post('/api/rules/add', {type: ruleForm.type.value, pattern: ruleForm.pattern.value, chat: ruleForm.chat.value});
    $('#ruleOut').innerHTML = j.ok ? '<span class="ok">Rule added</span>' : `<span class="bad">Not added: ${esc(j.error)}</span>`;
    if (j.ok) { ruleForm.pattern.value = ''; ruleForm.chat.value = ''; refresh(); }
  });
});
$('#rules').addEventListener('click', async e => {
  const b = e.target.closest('button');
  if (!b) return;
  let j;
  if (b.dataset.del !== undefined) {
    if (!confirm('Delete this rule?')) return;
    j = await post('/api/rules/delete', {index: b.dataset.del});
  } else if (b.dataset.move) {
    j = await post('/api/rules/move', {index: b.dataset.i, dir: b.dataset.move});
  } else return;
  $('#ruleOut').innerHTML = j.ok ? '' : `<span class="bad">${esc(j.error)}</span>`;
  refresh();
});

const smsForm = $('#smsForm');
smsForm.text.addEventListener('input', () => {
  const n = smsForm.text.value.length;
  $('#smsCount').textContent = n ? `${n} chars, ${n <= 70 ? 1 : Math.ceil(n / 67)} SMS` : '';
});
smsForm.addEventListener('submit', e => {
  e.preventDefault();
  const btn = smsForm.querySelector('button');
  busy(btn, null, async () => {
    $('#smsOut').textContent = 'Sending…';
    const j = await post('/api/sms', {to: smsForm.to.value, text: smsForm.text.value});
    $('#smsOut').innerHTML = j.ok ? `<span class="ok">Sent (${j.parts} SMS)</span>` : `<span class="bad">Error: ${esc(j.error)}</span>`;
    if (j.ok) { smsForm.text.value = ''; $('#smsCount').textContent = ''; refresh(); }
  });
});

const ussdForm = $('#ussdForm');
ussdForm.addEventListener('submit', e => {
  e.preventDefault();
  busy(ussdForm.querySelector('button'), null, async () => {
    $('#ussdOut').textContent = 'Waiting for the operator…';
    const j = await post('/api/ussd', {code: ussdForm.code.value});
    $('#ussdOut').innerHTML = j.ok
      ? esc(j.text) + (j.session ? '\n\n<span class="muted">The operator expects a reply — type it in the field and press “Send”.</span>' : '')
      : `<span class="bad">Error: ${esc(j.error)}</span>`;
    if (j.ok && j.session) { ussdForm.code.value = ''; ussdForm.code.focus(); }
  });
});
$('#ussdCancel').addEventListener('click', async () => {
  const j = await post('/api/ussd-cancel');
  $('#ussdOut').textContent = j.ok ? 'Menu closed' : 'No open menu';
});

const cfgForm = $('#cfgForm');
cfgForm.addEventListener('submit', e => {
  e.preventDefault();
  busy(cfgForm.querySelector('button'), null, async () => {
    $('#cfgOut').textContent = 'Checking…';
    const j = await post('/api/config', {token: cfgForm.token.value, chat: cfgForm.chat.value});
    $('#cfgOut').innerHTML = j.ok ? '<span class="ok">Saved, test message sent</span>' : `<span class="bad">Not saved: ${esc(j.error)}</span>`;
    if (j.ok) { cfgForm.reset(); refresh(); }
  });
});
$('#testBtn').addEventListener('click', e => busy(e.target, null, async () => {
  $('#cfgOut').textContent = 'Sending…';
  const j = await post('/api/test');
  $('#cfgOut').innerHTML = j.ok ? '<span class="ok">Sent, check the chat</span>' : `<span class="bad">Error: ${esc(j.error)}</span>`;
}));

$('#rebootBtn').addEventListener('click', async () => {
  if (!confirm('Reboot the board?')) return;
  await post('/api/reboot');
  $('#sysOut').textContent = 'Rebooting, the page will reload in about a minute…';
  setTimeout(() => location.reload(), 60000);
});
$('#resetBtn').addEventListener('click', async () => {
  if (!confirm('Erase WiFi, bot token, default chat and rules? The board will open its setup hotspot and this page will stop working.')) return;
  await post('/api/factory-reset');
  $('#sysOut').textContent = 'Settings erased. Connect to the board’s hotspot to set it up again.';
});

refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>
)html";
