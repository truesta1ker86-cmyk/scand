/**
 * Приёмник вебхуков с SSE (Server-Sent Events).
 * Запуск: node hooks_sse.js
 * Открыть: http://127.0.0.1:9000/
 *
 * Зависимостей нет — только стандартный http.
 */

const http = require('http');

const PORT = 9000;
const MAX_ENTRIES = 500;

// ---------------------------------------------------------------------------
// Хранилище последних хуков
// ---------------------------------------------------------------------------
const entries = [];
let nextId = 1;

// ---------------------------------------------------------------------------
// Подписчики SSE (открытые EventSource-соединения)
// ---------------------------------------------------------------------------
const subscribers = new Set();

// Отправить событие конкретному клиенту
function sendEvent(res, eventName, data) {
  if (eventName) res.write(`event: ${eventName}\n`);
  res.write(`data: ${JSON.stringify(data)}\n\n`);
}

// Разослать событие всем подписчикам
function broadcast(eventName, data) {
  for (const res of subscribers) {
    try {
      sendEvent(res, eventName, data);
    } catch (e) {
      subscribers.delete(res);
    }
  }
}

// ---------------------------------------------------------------------------
// HTML-страница с EventSource
// ---------------------------------------------------------------------------
const HTML_PAGE = `<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<title>Webhook Receiver (SSE)</title>
<style>
  * { box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace;
    background: #1e1e2e; color: #cdd6f4; margin: 0; padding: 20px;
  }
  h1 { margin: 0 0 16px; font-size: 22px; color: #89b4fa; }
  .toolbar {
    display: flex; gap: 12px; align-items: center;
    margin-bottom: 16px; flex-wrap: wrap;
  }
  .toolbar button {
    background: #313244; color: #cdd6f4; border: 1px solid #45475a;
    padding: 6px 14px; border-radius: 6px; cursor: pointer; font-size: 13px;
  }
  .toolbar button:hover { background: #45475a; }
  .status {
    padding: 4px 10px; border-radius: 6px; font-size: 12px;
    background: #a6e3a1; color: #1e1e2e;
  }
  .status.disconnected { background: #f38ba8; }
  .status.connecting   { background: #f9e2af; }
  .count { font-size: 13px; color: #9399b2; }
  table {
    width: 100%; border-collapse: collapse;
    background: #181825; border-radius: 8px; overflow: hidden;
  }
  th, td {
    padding: 10px 12px; text-align: left; font-size: 13px;
    border-bottom: 1px solid #313244; vertical-align: top;
  }
  th { background: #313244; color: #89b4fa; font-weight: 600; }
  tr:hover td { background: #1e1e2e; }
  tr.new td { animation: flash 1s ease-out; }
  @keyframes flash {
    from { background: #45475a; }
    to   { background: transparent; }
  }
  .type { color: #f38ba8; font-weight: 600; }
  .source-ozon   { color: #a6e3a1; }
  .source-onec   { color: #fab387; }
  .source-custom { color: #cba6f7; }
  pre {
    margin: 0; font-size: 12px; color: #9399b2;
    white-space: pre-wrap; word-break: break-all; max-width: 600px;
  }
  .empty {
    text-align: center; padding: 40px; color: #6c7086; font-size: 14px;
  }
  .id { color: #6c7086; font-size: 11px; }
  .time { color: #6c7086; font-size: 12px; }
</style>
</head>
<body>
  <h1>📡 Webhook Receiver <span style="font-size:13px;color:#6c7086">(SSE)</span></h1>
  <div class="toolbar">
    <button onclick="clearAll()">🗑 Очистить</button>
    <button onclick="location.reload()">🔄 Перезагрузить</button>
    <span class="status connecting" id="status">Connecting...</span>
    <span class="count" id="count">0</span>
  </div>

  <table>
    <thead>
      <tr>
        <th style="width:60px">ID</th>
        <th style="width:170px">Время (UTC)</th>
        <th style="width:100px">Источник</th>
        <th style="width:220px">Тип</th>
        <th>Тело</th>
      </tr>
    </thead>
    <tbody id="tbody">
      <tr><td colspan="5" class="empty">Ожидание вебхуков...</td></tr>
    </tbody>
  </table>

<script>
  const tbody  = document.getElementById('tbody');
  const status = document.getElementById('status');
  const count  = document.getElementById('count');

  let items = [];

  function escapeHtml(s) {
    return String(s || '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
  }

  function render() {
    count.textContent = items.length + ' записей';
    if (!items.length) {
      tbody.innerHTML = '<tr><td colspan="5" class="empty">Ожидание вебхуков...</td></tr>';
      return;
    }
    tbody.innerHTML = items.map(it => {
      const srcClass = 'source-' + (it.source || 'custom');
      return \`<tr data-id="\${it.id}">
        <td class="id">#\${it.id}</td>
        <td class="time">\${it.received_at}</td>
        <td class="\${srcClass}">\${it.source || '-'}</td>
        <td class="type">\${escapeHtml(it.message_type) || '-'}</td>
        <td><pre>\${escapeHtml(it.body)}</pre></td>
      </tr>\`;
    }).join('');
  }

  function addItem(entry) {
    items.unshift(entry);
    if (items.length > 500) items.pop();
    render();
    const row = tbody.querySelector(\`tr[data-id="\${entry.id}"]\`);
    if (row) row.classList.add('new');
  }

  function setStatus(text, cls) {
    status.textContent = text;
    status.className = 'status ' + (cls || '');
  }

  // -------------------------------------------------------------------------
  // EventSource — поток от сервера к браузеру
  // -------------------------------------------------------------------------
  const eventSource = new EventSource("/events/subscribe");

  eventSource.onopen = function() {
    setStatus('Live', '');
  };

  eventSource.onmessage = function(event) {
    // Безымянные события (если сервер шлёт просто "data:")
    console.log("onmessage:", event.data);
  };

  eventSource.addEventListener("init", function(event) {
    const data = JSON.parse(event.data);
    items = data.items || [];
    render();
  });

  eventSource.addEventListener("new", function(event) {
    const entry = JSON.parse(event.data);
    console.log("Новое сообщение", entry);
    addItem(entry);
  });

  eventSource.addEventListener("cleared", function() {
    items = [];
    render();
  });

  eventSource.onerror = function() {
    if (eventSource.readyState === EventSource.CONNECTING) {
      setStatus('Reconnecting...', 'connecting');
    } else {
      setStatus('Disconnected', 'disconnected');
    }
  };

  async function clearAll() {
    if (!confirm('Очистить все записи?')) return;
    await fetch('/api/hooks', { method: 'DELETE' });
  }
</script>
</body>
</html>`;

// ---------------------------------------------------------------------------
// Определяем источник по message_type
// ---------------------------------------------------------------------------
function detectSource(msgType) {
  if (!msgType) return 'custom';
  if (msgType.startsWith('TYPE_')) return 'ozon';
  if (['SIGNAL', 'product_updated', 'product_created'].includes(msgType)) return 'onec';
  return 'custom';
}

// ---------------------------------------------------------------------------
// HTTP-сервер
// ---------------------------------------------------------------------------
const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);

  // -------------------------------------------------------------------------
  // HTML-страница
  // -------------------------------------------------------------------------
  if (req.method === 'GET' && url.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(HTML_PAGE);
    return;
  }

  // -------------------------------------------------------------------------
  // SSE-эндпоинт
  // -------------------------------------------------------------------------
  if (req.method === 'GET' && url.pathname === '/events/subscribe') {
    res.writeHead(200, {
      'Content-Type':         'text/event-stream; charset=utf-8',
      'Cache-Control':        'no-cache, no-transform',
      'Connection':           'keep-alive',
      'X-Accel-Buffering':    'no'   // для nginx: отключить буферизацию
    });

    // Начальный комментарий — «прогревает» соединение
    res.write(': connected\n\n');

    // Отправляем текущее состояние (исправлено: items: entries)
    sendEvent(res, 'init', { items: entries });

    // Регистрируем подписчика
    subscribers.add(res);
    console.log(`[SSE] Client connected (total ${subscribers.size})`);

    // Keep-alive: комментарий каждые 15 секунд, чтобы прокси не закрыл соединение
    const keepAlive = setInterval(() => {
      try {
        res.write(': keep-alive\n\n');
      } catch (e) {
        clearInterval(keepAlive);
      }
    }, 15000);

    // Очистка при отключении
    req.on('close', () => {
      clearInterval(keepAlive);
      subscribers.delete(res);
      console.log(`[SSE] Client disconnected (total ${subscribers.size})`);
    });

    return;
  }

  // -------------------------------------------------------------------------
  // API: список хуков
  // -------------------------------------------------------------------------
  if (req.method === 'GET' && url.pathname === '/api/hooks') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ items: entries }));
    return;
  }

  // -------------------------------------------------------------------------
  // API: очистка
  // -------------------------------------------------------------------------
  if (req.method === 'DELETE' && url.pathname === '/api/hooks') {
    entries.length = 0;
    broadcast('cleared', {});
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'cleared' }));
    return;
  }

  // -------------------------------------------------------------------------
  // Приём вебхуков
  // -------------------------------------------------------------------------
  if (req.method === 'POST' && url.pathname === '/hooks') {
    let raw = '';
    req.on('data', chunk => { raw += chunk; });
    req.on('end', () => {
      let payload = {};
      try { payload = JSON.parse(raw); } catch {}

      const msgType = payload.message_type || payload.event || '';
      const source  = payload.source || detectSource(msgType);

      const entry = {
        id:           nextId++,
        received_at:  new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
        source,
        message_type: msgType,
        body:         raw,
        remote_ip:    req.headers['x-forwarded-for'] || req.socket.remoteAddress || ''
      };

      entries.unshift(entry);
      if (entries.length > MAX_ENTRIES) entries.pop();

      console.log(`[HOOKS] #${entry.id} ${source} / ${msgType}`);

      // Мгновенная рассылка всем SSE-подписчикам
      broadcast('new', entry);

      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: 'ok', id: entry.id }));
    });
    return;
  }

  // -------------------------------------------------------------------------
  // 404
  // -------------------------------------------------------------------------
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'not found' }));
});

// ---------------------------------------------------------------------------
// Запуск
// ---------------------------------------------------------------------------
server.listen(PORT, '0.0.0.0', () => {
  console.log('Webhook receiver (SSE) started');
  console.log(`  HTML view:      http://127.0.0.1:${PORT}/`);
  console.log(`  POST endpoint:  http://127.0.0.1:${PORT}/hooks`);
  console.log(`  SSE subscribe:  http://127.0.0.1:${PORT}/events/subscribe`);
  console.log(`  API:            http://127.0.0.1:${PORT}/api/hooks`);
});
