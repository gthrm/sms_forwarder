#pragma once
// Web panel page, served at "/". Talks to the /api/* endpoints in sms_forwarder.ino.

static const char WEB_PAGE[] = R"html(<!doctype html>
<html lang="ru">
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
  <section class="card"><h2>Модем</h2><dl class="kv" id="modem"></dl></section>
  <section class="card"><h2>WiFi и плата</h2><dl class="kv" id="sys"></dl></section>
</div>

<section class="card"><h2>Правила пересылки</h2>
  <div id="rules"></div>
  <form class="rule-form" id="ruleForm">
    <select name="type">
      <option value="STARTS_WITH">Начинается с</option>
      <option value="CONTAINS">Содержит</option>
      <option value="FROM">Отправитель</option>
    </select>
    <input name="pattern" placeholder="[work]:" required autocomplete="off">
    <input name="chat" placeholder="ID чата (-100…)" required autocomplete="off">
    <button>Добавить</button>
  </form>
  <div class="out" id="ruleOut" style="margin-top:8px"></div>
</section>

<section class="card"><h2>Сообщения</h2><div class="history" id="history"></div></section>

<div class="grid" style="margin-top:16px">
  <section class="card"><h2>Отправить SMS</h2>
    <form id="smsForm">
      <input name="to" placeholder="Номер в международном формате, с +" inputmode="tel" required>
      <textarea name="text" placeholder="Текст" required></textarea>
      <div class="row"><button>Отправить</button><span class="muted" id="smsCount"></span></div>
      <div class="out" id="smsOut"></div>
    </form>
  </section>
  <section class="card"><h2>USSD</h2>
    <form id="ussdForm">
      <input name="code" placeholder="*100#" inputmode="tel" required>
      <div class="row"><button>Отправить</button><button type="button" class="secondary" id="ussdCancel">Закрыть меню</button></div>
      <div class="out" id="ussdOut"></div>
    </form>
  </section>
</div>

<div class="grid">
  <section class="card"><h2>Telegram</h2>
    <dl class="kv" id="tg"></dl>
    <form id="cfgForm" style="margin-top:12px">
      <input name="token" placeholder="Новый токен бота (пусто — не менять)" autocomplete="off">
      <input name="chat" placeholder="Новый ID канала по умолчанию (пусто — не менять)" autocomplete="off">
      <div class="row"><button>Сохранить</button><button type="button" class="secondary" id="testBtn">Тестовое сообщение</button></div>
      <div class="out" id="cfgOut"></div>
    </form>
  </section>
  <section class="card"><h2>Плата</h2>
    <div class="row"><button class="secondary" id="rebootBtn">Перезагрузить</button><button class="danger" id="resetBtn">Сброс до заводских</button></div>
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
  return (d ? d + ' д ' : '') + (h ? h + ' ч ' : '') + m + ' мин';
}
function signal(csq) {
  if (csq < 0 || csq === 99) return dot('bad') + 'нет';
  const [w, c] = csq < 10 ? ['слабый', 'bad'] : csq < 15 ? ['средний', 'warn'] : csq < 20 ? ['хороший', 'ok'] : ['отличный', 'ok'];
  return `${dot(c)}${w} (${csq}/31)`;
}
const STATUS = {sent: ['переслано', 'ok'], fallback: ['в канал по умолчанию', 'warn'], pending: ['ждёт отправки', 'warn'], failed: ['ошибка', 'bad']};
const RULE = {STARTS_WITH: 'Начинается с', CONTAINS: 'Содержит', FROM: 'Отправитель'};
let lastRules = '';

async function post(url, data) {
  try {
    const r = await fetch(url, {method: 'POST', body: new URLSearchParams(data || {})});
    return await r.json();
  } catch (e) {
    return {ok: false, error: 'нет связи с платой'};
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
      ['Модем', m.ready ? dot('ok') + 'работает' : dot('bad') + 'не запущен'],
      ['SIM', dot(yes(m.sim)) + (m.sim ? 'готова' : 'нет')],
      ['Сеть', dot(yes(m.registered)) + (m.registered ? 'зарегистрирован' : 'нет регистрации')],
      ['Оператор', esc(m.operator || '—')],
      ['Сигнал', signal(m.csq)],
      ['SMS на симке', m.simTotal ? `${m.simUsed} из ${m.simTotal}` : '—'],
      ['Время сети', esc(m.clock || '—')],
    ]);
    kv($('#sys'), [
      ['WiFi', dot(yes(s.wifi.connected)) + esc(s.wifi.ssid)],
      ['Уровень WiFi', s.wifi.rssi + ' dBm'],
      ['Адрес', `${esc(s.wifi.host)}<br><span class="muted">${esc(s.wifi.ip)}</span>`],
      ['Работает', dur(s.uptime)],
      ['Причина запуска', esc(s.resetReason)],
      ['Переслано / неудачных попыток', `${s.stats.forwarded} / ${s.stats.failed}`],
      ['Свободная память', Math.round(s.heap / 1024) + ' КБ'],
      ['Последняя ошибка', s.lastError ? `<span class="bad">${esc(s.lastError)}</span><br><span class="muted">${dur(s.lastErrorAgo)} назад</span>` : '—'],
    ]);
    kv($('#tg'), [['Бот', esc(s.tg.token)], ['Канал по умолчанию', esc(s.tg.chat)]]);
    const rulesKey = JSON.stringify(s.rules);
    if (rulesKey !== lastRules) {  // don't re-render while nothing changed, so buttons don't flicker
      lastRules = rulesKey;
      const n = s.rules.length;
      $('#rules').innerHTML = (n
        ? `<table><tr><th>#</th><th>Условие</th><th>Значение</th><th>Чат</th><th></th></tr>${s.rules.map((r, i) =>
            `<tr><td class="muted">${i + 1}</td><td>${RULE[r.type]}</td><td><code>${esc(r.pattern)}</code></td><td>${esc(r.chat)}</td>
             <td class="actions"><button class="secondary" data-move="up" data-i="${i}" ${i ? '' : 'disabled'} title="Выше">↑</button><button class="secondary" data-move="down" data-i="${i}" ${i < n - 1 ? '' : 'disabled'} title="Ниже">↓</button><button class="secondary" data-del="${i}" title="Удалить">✕</button></td></tr>`).join('')}</table>`
        : '<p class="muted" style="margin:0">Правил пока нет — всё уходит в канал по умолчанию.</p>') +
        `<p class="muted">Проверяются сверху вниз, срабатывает первое подходящее. Остальное — в канал по умолчанию (${esc(s.tg.chat)}).
         Пример: правило «Начинается с» <code>[work]:</code> отправит SMS «[work]: привет» в свой чат как «привет».</p>`;
    }
    $('#history').innerHTML = h.length ? h.map(x => {
      const [label, cls] = STATUS[x.status] || [x.status, ''];
      const where = x.out ? 'исходящее' : (x.chat ? '→ ' + esc(x.chat) : '');
      return `<div class="msg"><div class="meta"><b>${x.out ? '↑ ' : '↓ '}${esc(x.peer)}</b>
        <span>${esc(x.time)}</span><span>${where}</span><span class="badge ${cls}">${label}</span></div>
        <div class="text">${esc(x.text)}</div></div>`;
    }).join('') : '<p class="muted">Пока пусто. Здесь будут последние 50 SMS с момента включения платы.</p>';
    $('#updated').textContent = 'обновлено ' + new Date().toLocaleTimeString();
  } catch (e) {
    $('#updated').textContent = 'нет связи с платой';
  }
}

const ruleForm = $('#ruleForm');
const PLACEHOLDER = {STARTS_WITH: '[work]:', CONTAINS: 'код', FROM: 'имя или номер отправителя'};
ruleForm.type.addEventListener('change', () => ruleForm.pattern.placeholder = PLACEHOLDER[ruleForm.type.value]);
ruleForm.addEventListener('submit', e => {
  e.preventDefault();
  busy(ruleForm.querySelector('button'), null, async () => {
    $('#ruleOut').textContent = 'Проверяю чат — отправляю туда тестовое сообщение…';
    const j = await post('/api/rules/add', {type: ruleForm.type.value, pattern: ruleForm.pattern.value, chat: ruleForm.chat.value});
    $('#ruleOut').innerHTML = j.ok ? '<span class="ok">Правило добавлено</span>' : `<span class="bad">Не добавлено: ${esc(j.error)}</span>`;
    if (j.ok) { ruleForm.pattern.value = ''; ruleForm.chat.value = ''; refresh(); }
  });
});
$('#rules').addEventListener('click', async e => {
  const b = e.target.closest('button');
  if (!b) return;
  let j;
  if (b.dataset.del !== undefined) {
    if (!confirm('Удалить правило?')) return;
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
  $('#smsCount').textContent = n ? `${n} симв., ${n <= 70 ? 1 : Math.ceil(n / 67)} SMS` : '';
});
smsForm.addEventListener('submit', e => {
  e.preventDefault();
  const btn = smsForm.querySelector('button');
  busy(btn, null, async () => {
    $('#smsOut').textContent = 'Отправляю…';
    const j = await post('/api/sms', {to: smsForm.to.value, text: smsForm.text.value});
    $('#smsOut').innerHTML = j.ok ? `<span class="ok">Отправлено (${j.parts} SMS)</span>` : `<span class="bad">Ошибка: ${esc(j.error)}</span>`;
    if (j.ok) { smsForm.text.value = ''; $('#smsCount').textContent = ''; refresh(); }
  });
});

const ussdForm = $('#ussdForm');
ussdForm.addEventListener('submit', e => {
  e.preventDefault();
  busy(ussdForm.querySelector('button'), null, async () => {
    $('#ussdOut').textContent = 'Жду ответ оператора…';
    const j = await post('/api/ussd', {code: ussdForm.code.value});
    $('#ussdOut').innerHTML = j.ok
      ? esc(j.text) + (j.session ? '\n\n<span class="muted">Оператор ждёт ответа — введи его в поле и нажми «Отправить».</span>' : '')
      : `<span class="bad">Ошибка: ${esc(j.error)}</span>`;
    if (j.ok && j.session) { ussdForm.code.value = ''; ussdForm.code.focus(); }
  });
});
$('#ussdCancel').addEventListener('click', async () => {
  const j = await post('/api/ussd-cancel');
  $('#ussdOut').textContent = j.ok ? 'Меню закрыто' : 'Открытого меню нет';
});

const cfgForm = $('#cfgForm');
cfgForm.addEventListener('submit', e => {
  e.preventDefault();
  busy(cfgForm.querySelector('button'), null, async () => {
    $('#cfgOut').textContent = 'Проверяю…';
    const j = await post('/api/config', {token: cfgForm.token.value, chat: cfgForm.chat.value});
    $('#cfgOut').innerHTML = j.ok ? '<span class="ok">Сохранено, тестовое сообщение отправлено</span>' : `<span class="bad">Не сохранено: ${esc(j.error)}</span>`;
    if (j.ok) { cfgForm.reset(); refresh(); }
  });
});
$('#testBtn').addEventListener('click', e => busy(e.target, null, async () => {
  $('#cfgOut').textContent = 'Отправляю…';
  const j = await post('/api/test');
  $('#cfgOut').innerHTML = j.ok ? '<span class="ok">Отправлено, проверь канал</span>' : `<span class="bad">Ошибка: ${esc(j.error)}</span>`;
}));

$('#rebootBtn').addEventListener('click', async () => {
  if (!confirm('Перезагрузить плату?')) return;
  await post('/api/reboot');
  $('#sysOut').textContent = 'Перезагружаюсь, страница обновится сама примерно через минуту…';
  setTimeout(() => location.reload(), 60000);
});
$('#resetBtn').addEventListener('click', async () => {
  if (!confirm('Стереть WiFi, токен бота и канал? Плата поднимет точку доступа для новой настройки, эта страница перестанет открываться.')) return;
  await post('/api/factory-reset');
  $('#sysOut').textContent = 'Настройки стёрты. Подключись к точке доступа платы, чтобы настроить заново.';
});

refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>
)html";
