/****************************************************
 * web_page.h  -  the dashboard that the ESP32 serves
 * (lives inside the ESP32, so it works with NO internet)
 ****************************************************/
#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Reminder (Edge)</title>
<style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:0 auto;padding:16px;
     background:#f5f7fa;color:#222}
h1{font-size:1.35em} h2{font-size:1.1em;margin-top:0}
.card{background:#fff;border-radius:12px;padding:16px;margin-bottom:16px;
      box-shadow:0 1px 4px rgba(0,0,0,.08)}
input,select,textarea,button{font-size:1em;padding:8px;margin:4px 0;border-radius:8px;
      border:1px solid #ccc;width:100%;box-sizing:border-box;font-family:inherit}
button{background:#1a73e8;color:#fff;border:none;cursor:pointer}
button.small{width:auto;padding:5px 10px;margin-left:4px}
button.ok{background:#188038} button.del{background:#d93025}
.row{display:flex;justify-content:space-between;align-items:center;
     padding:8px 0;border-bottom:1px solid #eee}
.row:last-child{border-bottom:none}
.is-done{text-decoration:line-through;color:#888}
.badge{border-radius:6px;padding:2px 8px;font-size:.78em;margin-left:6px;
       background:#e8f0fe;color:#1a73e8}
.badge.done{background:#e6f4ea;color:#188038}
.badge.missed{background:#fce8e6;color:#d93025}
#banner{background:#fef7e0;border-radius:8px;padding:8px 12px;margin-bottom:12px;display:none}
small{color:#666}
</style>
</head>
<body>
<h1>&#9200; Smart Reminder &mdash; Edge Mode</h1>
<p><small>Everything is stored inside the ESP32. Nothing is sent to the internet.</small></p>
<div id="banner"></div>

<div class="card">
  <h2>Add reminder</h2>
  <input id="title" maxlength="39" placeholder="e.g. VLSI class / Pay electricity bill">
  <select id="type">
    <option>Class</option><option>Exam</option><option>Assignment</option>
    <option>Meeting</option><option>Birthday</option><option>Bill</option>
    <option>Other</option>
  </select>
  <input id="start" type="datetime-local">
  <select id="repeat">
    <option value="0">Does not repeat</option>
    <option value="1">Repeat every day</option>
    <option value="2">Repeat every week (timetable)</option>
  </select>
  <button onclick="addOne()">Add reminder</button>
  <p id="msg"></p>
</div>

<div class="card">
  <h2>Bulk import (timetable)</h2>
  <small>One per line: <b>YYYY-MM-DD HH:MM, Title, Type, weekly</b>
  (last two parts optional; use daily / weekly / once)</small>
  <textarea id="bulk" rows="5"
    placeholder="2026-10-12 10:00, Sensors &amp; Actuators [H1], Class, weekly"></textarea>
  <button onclick="addBulk()">Import lines</button>
  <p id="bulkMsg"></p>
</div>

<div class="card">
  <h2>Reminders</h2>
  <div id="list">Loading...</div>
  <button class="del" onclick="clearFinished()">Clear finished / missed</button>
  <p><small id="info"></small></p>
</div>

<script>
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"']/g, c =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const STATUS = ['pending', 'done', 'missed'];
const REPEAT = ['', ' | daily', ' | weekly'];

async function api(path, data) {
  const opt = data ? { method: 'POST', body: new URLSearchParams(data) } : {};
  const res = await fetch(path, opt);
  return res.json();
}

/* Tell the ESP32 what time it is (taken from this phone/laptop) */
async function syncTime() {
  try { await api('/api/settime', { t: Math.floor(Date.now() / 1000) }); } catch (e) {}
}

async function load() {
  try {
    const d = await api('/api/list');
    const b = $('banner');
    b.style.display = d.timeSet ? 'none' : 'block';
    b.textContent = 'Clock not set yet - reload this page to sync time.';
    $('info').textContent = d.items.length + ' / ' + d.max + ' reminders stored | ' +
                            d.freeKB + ' KB flash free';
    $('list').innerHTML = d.items.length ? d.items.map(it => {
      const when = new Date(it.start * 1000).toLocaleString();
      const cls = it.st === 0 ? '' : 'is-done';
      const doneBtn = it.st === 0
        ? '<button class="small ok" onclick="act(\'done\',' + it.id + ')">Done</button>' : '';
      return '<div class="row"><div><span class="' + cls + '">' + esc(it.title) + '</span>' +
        '<span class="badge ' + STATUS[it.st] + '">' + esc(it.type) + ' | ' + STATUS[it.st] +
        REPEAT[it.rep] + '</span><br><small>' + when + '</small></div><div>' + doneBtn +
        '<button class="small del" onclick="act(\'delete\',' + it.id + ')">&#10005;</button>' +
        '</div></div>';
    }).join('') : '<small>Nothing scheduled.</small>';
  } catch (e) {
    $('list').textContent = 'Cannot reach the ESP32 - are you on the SmartReminder Wi-Fi?';
  }
}

async function act(name, id) { await api('/api/' + name, { id: id }); load(); }

async function addOne() {
  const title = $('title').value.trim();
  const s = $('start').value;
  if (!title || !s) { $('msg').textContent = 'Please fill the title and the time.'; return; }
  const r = await api('/api/add', {
    title: title, type: $('type').value,
    start: Math.floor(new Date(s).getTime() / 1000), repeat: $('repeat').value });
  $('msg').textContent = r.ok ? 'Added \u2714' : 'Error: ' + r.err;
  if (r.ok) $('title').value = '';
  load();
}

async function addBulk() {
  const lines = $('bulk').value.split('\n').map(l => l.trim()).filter(Boolean);
  let ok = 0;
  for (const line of lines) {
    const p = line.split(',').map(x => x.trim());
    const ms = new Date(p[0].replace(' ', 'T')).getTime();
    if (p.length < 2 || isNaN(ms)) continue;
    const rep = { daily: 1, weekly: 2 }[(p[3] || '').toLowerCase()] || 0;
    const r = await api('/api/add', {
      title: p[1], type: p[2] || 'Class', start: Math.floor(ms / 1000), repeat: rep });
    if (r.ok) ok++;
  }
  $('bulkMsg').textContent = 'Imported ' + ok + ' of ' + lines.length + ' lines.';
  load();
}

async function clearFinished() { await api('/api/clear', { x: 1 }); load(); }

syncTime().then(load);
setInterval(load, 15000);
</script>
</body>
</html>
)HTML";
