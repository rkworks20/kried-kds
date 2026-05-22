/**
 * KDS Server — Kried Kitchen Display System
 *
 * Setup:
 *   npm install ws
 *   node server.js
 *
 * Open http://YOUR_IP:3000 on any browser device on the same WiFi.
 * ESP32 connects to ws://YOUR_IP:3000 with header: x-client-type: esp32
 *
 * Find your IP:  ipconfig getifaddr en0   (Mac)
 *                hostname -I              (Linux/Pi)
 */

const http      = require('http');
const fs        = require('fs');
const path      = require('path');
const WebSocket = require('ws');

const PORT       = process.env.PORT || 3000;  // Railway injects PORT automatically
const STATE_FILE = process.env.STATE_FILE
  || path.join(process.env.RAILWAY_VOLUME_MOUNT_PATH || __dirname, 'kds-state.json');
const HTML_FILE  = path.join(__dirname, 'kitchen-display.html');

// ── State ──
let currentState = null;

function loadState() {
  try {
    if (fs.existsSync(STATE_FILE)) {
      currentState = JSON.parse(fs.readFileSync(STATE_FILE, 'utf8'));
      console.log('✓ Restored state from disk');
    }
  } catch (e) {
    console.warn('Could not load state, starting fresh:', e.message);
    currentState = null;
  }
}

function saveState(state) {
  try {
    if (fs.existsSync(STATE_FILE))
      fs.copyFileSync(STATE_FILE, STATE_FILE + '.backup');
    fs.writeFileSync(STATE_FILE, JSON.stringify(state), 'utf8');
  } catch (e) {
    console.warn('Could not save state:', e.message);
  }
}

// Send only the oldest on-display bill to ESP32
function getDisplayBills(state) {
  if (!state || !state.billsMap) return [];
  const onDisplay = Object.values(state.billsMap)
    .filter(b => b.onDisplay)
    .sort((a, b) => (a.onDisplaySince || 0) - (b.onDisplaySince || 0));
  return onDisplay.length > 0 ? [onDisplay[0].id] : [];
}

// ── HTTP server ──
const httpServer = http.createServer((req, res) => {

  // POST /order — billing API webhook
  if (req.method === 'POST' && req.url === '/order') {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      try {
        const order = JSON.parse(body);
        console.log('📦 New order:', order.id);
        broadcastToDisplays({ type: 'new_order', payload: order });
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true }));
      } catch (e) {
        res.writeHead(400);
        res.end('Bad request');
      }
    });
    return;
  }

  // POST /batch-done/:station — physical button (ESP32 or Pi Zero GPIO)
  if (req.method === 'POST' && req.url.startsWith('/batch-done/')) {
    const station = req.url.split('/batch-done/')[1];
    console.log(`🔘 Physical button: ${station}`);
    broadcastToDisplays({ type: 'batch_done', station });
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
    return;
  }

  // GET /current-bill — ESP32 polls this every 3 seconds
  if (req.method === 'GET' && req.url === '/current-bill') {
    const bills = getDisplayBills(currentState);
    const bill  = bills.length > 0 ? bills[0] : '';
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ bill }));
    return;
  }

  // GET / — serve the KDS HTML
  if (req.method === 'GET' && (req.url === '/' || req.url === '/index.html')) {
    try {
      const html = fs.readFileSync(HTML_FILE, 'utf8');
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(html);
    } catch (e) {
      res.writeHead(404);
      res.end('kitchen-display.html not found — put it in the same folder as server.js');
    }
    return;
  }

  res.writeHead(404);
  res.end('Not found');
});

// ── WebSocket server ──
const wss = new WebSocket.Server({ server: httpServer });

// Broadcast to all browser/display clients (not ESP32)
function broadcastToDisplays(msg, excludeClient = null) {
  const data = JSON.stringify(msg);
  wss.clients.forEach(client => {
    if (client !== excludeClient &&
        client.readyState === WebSocket.OPEN &&
        client._clientType !== 'esp32') {
      client.send(data);
    }
  });
}

// Send lightweight display list to all ESP32 clients
let lastPushedBill = null;

function pushToESP32(state, { force = false } = {}) {
  const bills = getDisplayBills(state);
  const bill  = bills.length > 0 ? bills[0] : '';
  if (!force && bill === lastPushedBill) return;
  lastPushedBill = bill;
  const msg = JSON.stringify({ type: 'display', bills });
  let sent = 0;
  wss.clients.forEach(client => {
    if (client._clientType === 'esp32' && client.readyState === WebSocket.OPEN) {
      client.send(msg);
      sent++;
    }
  });
  console.log(`→ pushToESP32: bill='${bill}' sent to ${sent} esp32 client(s)`);
}

// Poll once per second so the OLED stays in sync even if no browser pushes state.
setInterval(() => pushToESP32(currentState), 1000);

wss.on('connection', (ws, req) => {
  // Identify client type via header — take first token in case the header
  // was accidentally appended multiple times by a buggy client.
  const rawType    = req.headers['x-client-type'] || 'browser';
  const clientType = String(rawType).split(',')[0].trim();
  ws._clientType = clientType;
  console.log(`✅ ${clientType} connected — ${wss.clients.size} total`);

  if (clientType === 'esp32') {
    // Send current display bills immediately on connect
    const bills = getDisplayBills(currentState);
    ws.send(JSON.stringify({ type: 'display', bills }));
  } else {
    // Send full state to browser clients
    if (currentState) {
      ws.send(JSON.stringify({ type: 'state', payload: currentState }));
    }
  }

  ws.on('message', (raw) => {
    try {
      const msg = JSON.parse(raw);

      // Browser requesting state on reconnect
      if (msg.type === 'request_state') {
        if (currentState)
          ws.send(JSON.stringify({ type: 'state', payload: currentState }));
        return;
      }

      // Browser broadcasting a state update
      if (msg.type === 'state') {
        currentState = msg.payload;
        saveState(currentState);
        broadcastToDisplays({ type: 'state', payload: currentState }, ws);
        const bills = getDisplayBills(currentState);
        console.log(`📋 State updated — bills on display: [${bills.join(', ') || 'none'}]`);
        pushToESP32(currentState);
        return;
      }

    } catch (e) {
      console.warn('Bad WS message:', e.message);
    }
  });

  ws.on('close', () => {
    console.log(`❌ ${clientType} disconnected — ${wss.clients.size} remaining`);
  });
});

// ── Start ──
loadState();
httpServer.listen(PORT, '0.0.0.0', () => {
  const { networkInterfaces } = require('os');
  const nets = networkInterfaces();
  const ips  = [];
  for (const name of Object.keys(nets))
    for (const net of nets[name])
      if (net.family === 'IPv4' && !net.internal) ips.push(net.address);

  console.log('\n🍔  KDS Server running\n');
  console.log('Browser displays:');
  console.log(`  http://localhost:${PORT}`);
  ips.forEach(ip => console.log(`  http://${ip}:${PORT}`));
  console.log('\nESP32 WebSocket:');
  ips.forEach(ip => console.log(`  ws://${ip}:${PORT}`));
  console.log('\nPhysical button endpoints:');
  ips.forEach(ip => {
    console.log(`  POST http://${ip}:${PORT}/batch-done/burger`);
    console.log(`  POST http://${ip}:${PORT}/batch-done/fries`);
  });
  console.log('\nWaiting for connections...\n');
});
