/**
 * LoRa Receiver — WiFi AP Dashboard
 * Receives LoRa packets and serves them to browsers connected to the AP.
 * Display removed; all output via Serial + web interface.
 * Companion to the LoRa-WiFi beacon transmitter.
 */

#define HELTEC_POWER_BUTTON
#include <heltec_unofficial.h>
#include <WiFi.h>
#include <WebServer.h>

// ── LoRa settings (must match transmitter) ───────────────────────────────────
#define FREQUENCY           905.2
#define BANDWIDTH           250.0
#define SPREADING_FACTOR    9

// ── WiFi AP settings ─────────────────────────────────────────────────────────
#define AP_SSID   "LoRa-Monitor"
#define AP_PASS   "lora1234"        // min 8 chars; set "" for open network
#define AP_IP     IPAddress(192, 168, 4, 1)
#define AP_GW     IPAddress(192, 168, 4, 1)
#define AP_SUBNET IPAddress(255, 255, 255, 0)

// ── Globals ───────────────────────────────────────────────────────────────────
WebServer server(80);

String  rxdata;
volatile bool rxFlag = false;
int     packetCount  = 0;

// Ring buffer of last N packets for new clients
struct Packet {
  int    num;
  String data;
  float  rssi;
  float  snr;
};
#define MAX_HISTORY 20
Packet  history[MAX_HISTORY];
int     histHead = 0;   // index of oldest entry
int     histSize = 0;   // how many entries are valid

// SSE client list (single-client simplification — works fine for a monitor page)
WiFiClient sseClient;
bool       sseConnected = false;

// ── ISR ───────────────────────────────────────────────────────────────────────
void rx() { rxFlag = true; }

// ── HTML page (stored in program memory) ─────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"RAW(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LoRa Monitor</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Orbitron:wght@600&display=swap');

  :root {
    --bg:      #050a0e;
    --panel:   #0a1520;
    --border:  #0f3a4a;
    --accent:  #00e5ff;
    --accent2: #00ff88;
    --warn:    #ff4d4d;
    --text:    #c8dde8;
    --dim:     #4a7080;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'Share Tech Mono', monospace;
    min-height: 100vh;
    padding: 24px 16px;
  }

  /* Subtle scanline texture */
  body::before {
    content: '';
    position: fixed; inset: 0;
    background: repeating-linear-gradient(
      0deg,
      transparent,
      transparent 2px,
      rgba(0,229,255,.015) 2px,
      rgba(0,229,255,.015) 4px
    );
    pointer-events: none;
    z-index: 999;
  }

  header {
    display: flex;
    align-items: center;
    gap: 16px;
    margin-bottom: 28px;
    border-bottom: 1px solid var(--border);
    padding-bottom: 16px;
  }

  header h1 {
    font-family: 'Orbitron', sans-serif;
    font-size: clamp(1rem, 4vw, 1.5rem);
    color: var(--accent);
    letter-spacing: .12em;
    text-shadow: 0 0 18px rgba(0,229,255,.45);
  }

  .dot {
    width: 10px; height: 10px;
    border-radius: 50%;
    background: var(--dim);
    flex-shrink: 0;
    transition: background .3s, box-shadow .3s;
  }
  .dot.live {
    background: var(--accent2);
    box-shadow: 0 0 8px var(--accent2);
    animation: pulse 1.4s ease-in-out infinite;
  }
  @keyframes pulse {
    0%,100% { opacity: 1; }
    50%      { opacity: .4; }
  }

  .stats-row {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
    gap: 12px;
    margin-bottom: 24px;
  }

  .stat-card {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 14px 16px;
  }
  .stat-card .label {
    font-size: .68rem;
    color: var(--dim);
    letter-spacing: .12em;
    text-transform: uppercase;
    margin-bottom: 6px;
  }
  .stat-card .value {
    font-size: 1.35rem;
    color: var(--accent);
  }

  #log {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .packet {
    background: var(--panel);
    border: 1px solid var(--border);
    border-left: 3px solid var(--accent);
    border-radius: 0 6px 6px 0;
    padding: 12px 16px;
    animation: slideIn .25s ease;
  }
  .packet.error {
    border-left-color: var(--warn);
  }
  @keyframes slideIn {
    from { opacity: 0; transform: translateY(-6px); }
    to   { opacity: 1; transform: translateY(0); }
  }

  .packet-header {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    margin-bottom: 8px;
  }
  .packet-num {
    font-family: 'Orbitron', sans-serif;
    font-size: .78rem;
    color: var(--accent);
    letter-spacing: .08em;
  }
  .packet-time {
    font-size: .7rem;
    color: var(--dim);
  }

  .packet-data {
    font-size: .92rem;
    color: #e8f4f8;
    word-break: break-all;
    margin-bottom: 8px;
  }

  .packet-meta {
    display: flex;
    gap: 20px;
    font-size: .75rem;
    color: var(--dim);
  }
  .packet-meta span { color: var(--accent2); }

  #status-bar {
    position: fixed;
    bottom: 0; left: 0; right: 0;
    background: #040810;
    border-top: 1px solid var(--border);
    padding: 6px 16px;
    font-size: .72rem;
    color: var(--dim);
    display: flex;
    justify-content: space-between;
  }
</style>
</head>
<body>

<header>
  <div class="dot" id="dot"></div>
  <h1>LoRa Monitor</h1>
</header>

<div class="stats-row">
  <div class="stat-card">
    <div class="label">Packets</div>
    <div class="value" id="s-count">0</div>
  </div>
  <div class="stat-card">
    <div class="label">Last RSSI</div>
    <div class="value" id="s-rssi">—</div>
  </div>
  <div class="stat-card">
    <div class="label">Last SNR</div>
    <div class="value" id="s-snr">—</div>
  </div>
  <div class="stat-card">
    <div class="label">Freq</div>
    <div class="value">905.2</div>
  </div>
</div>

<div id="log"></div>

<div id="status-bar">
  <span id="conn-status">Connecting…</span>
  <span>BW 250 kHz · SF9</span>
</div>

<script>
  const log      = document.getElementById('log');
  const dot      = document.getElementById('dot');
  const sCount   = document.getElementById('s-count');
  const sRSSI    = document.getElementById('s-rssi');
  const sSNR     = document.getElementById('s-snr');
  const connStat = document.getElementById('conn-status');

  function addPacket(num, data, rssi, snr, isError, ts) {
    const div = document.createElement('div');
    div.className = 'packet' + (isError ? ' error' : '');

    if (isError) {
      div.innerHTML = `
        <div class="packet-header">
          <span class="packet-num" style="color:var(--warn)">RX ERROR</span>
          <span class="packet-time">${ts}</span>
        </div>
        <div class="packet-data">${data}</div>`;
    } else {
      div.innerHTML = `
        <div class="packet-header">
          <span class="packet-num">PKT #${num}</span>
          <span class="packet-time">${ts}</span>
        </div>
        <div class="packet-data">${data}</div>
        <div class="packet-meta">
          RSSI <span>${rssi} dBm</span>
          &nbsp; SNR <span>${snr} dB</span>
        </div>`;
      sCount.textContent = num;
      sRSSI.textContent  = rssi;
      sSNR.textContent   = snr;
    }

    log.prepend(div);
    // keep DOM tidy
    while (log.children.length > 50) log.removeChild(log.lastChild);

    // flash dot
    dot.classList.add('live');
    setTimeout(() => dot.classList.remove('live'), 1200);
  }

  // ── SSE connection ────────────────────────────────────────────────────────
  function connect() {
    const es = new EventSource('/events');

    es.addEventListener('open', () => {
      connStat.textContent = 'Live';
      dot.classList.add('live');
    });

    es.addEventListener('packet', e => {
      const d = JSON.parse(e.data);
      addPacket(d.num, d.data, d.rssi, d.snr, false,
                new Date().toLocaleTimeString());
    });

    es.addEventListener('error_pkt', e => {
      const d = JSON.parse(e.data);
      addPacket(0, 'RX Error: ' + d.code, 0, 0, true,
                new Date().toLocaleTimeString());
    });

    es.onerror = () => {
      connStat.textContent = 'Reconnecting…';
      dot.classList.remove('live');
      es.close();
      setTimeout(connect, 3000);
    };
  }

  connect();
</script>
</body>
</html>
)RAW";

// ── Web server handlers ────────────────────────────────────────────────────────

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// SSE endpoint — keeps one long-lived connection open
void handleEvents() {
  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/event-stream");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println("Access-Control-Allow-Origin: *");
  client.println();
  client.flush();

  // Send backlog of recent packets
  int start = (histSize < MAX_HISTORY) ? 0 : histHead;
  for (int i = 0; i < histSize; i++) {
    int idx = (start + i) % MAX_HISTORY;
    Packet &p = history[idx];
    String msg = "event: packet\ndata: {\"num\":" + String(p.num)
               + ",\"data\":\"" + p.data + "\""
               + ",\"rssi\":" + String(p.rssi, 2)
               + ",\"snr\":"  + String(p.snr, 2)
               + "}\n\n";
    client.print(msg);
  }
  client.flush();

  // Hand off to our SSE client slot
  if (sseConnected) sseClient.stop();
  sseClient    = client;
  sseConnected = true;
}

// Push a packet event to the SSE client (if connected)
void pushPacket(Packet &p) {
  if (!sseConnected || !sseClient.connected()) {
    sseConnected = false;
    return;
  }
  String msg = "event: packet\ndata: {\"num\":" + String(p.num)
             + ",\"data\":\"" + p.data + "\""
             + ",\"rssi\":" + String(p.rssi, 2)
             + ",\"snr\":"  + String(p.snr, 2)
             + "}\n\n";
  sseClient.print(msg);
  sseClient.flush();
}

void pushError(int code) {
  if (!sseConnected || !sseClient.connected()) {
    sseConnected = false;
    return;
  }
  String msg = "event: error_pkt\ndata: {\"code\":" + String(code) + "}\n\n";
  sseClient.print(msg);
  sseClient.flush();
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
  heltec_setup();   // initialises Serial, power button, etc. — display unused

  Serial.println("LoRa Receiver — WiFi AP mode");

  // Start WiFi access point
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_SUBNET);
  if (strlen(AP_PASS) >= 8)
    WiFi.softAP(AP_SSID, AP_PASS);
  else
    WiFi.softAP(AP_SSID);           // open if password too short / empty

  Serial.printf("AP started: %s  IP: %s\n", AP_SSID, AP_IP.toString().c_str());

  // Register routes
  server.on("/",       handleRoot);
  server.on("/events", handleEvents);
  server.begin();
  Serial.println("HTTP server running on port 80");

  // Init LoRa radio
  RADIOLIB_OR_HALT(radio.begin());
  radio.setDio1Action(rx);

  RADIOLIB_OR_HALT(radio.setFrequency(FREQUENCY));
  RADIOLIB_OR_HALT(radio.setBandwidth(BANDWIDTH));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(SPREADING_FACTOR));

  RADIOLIB_OR_HALT(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF));
  Serial.println("Radio listening…");
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
  heltec_loop();
  server.handleClient();

  if (rxFlag) {
    rxFlag = false;
    radio.readData(rxdata);

    if (_radiolib_status == RADIOLIB_ERR_NONE) {
      float rssi = radio.getRSSI();
      float snr  = radio.getSNR();

      packetCount++;

      // Store in ring buffer
      Packet p = { packetCount, rxdata, rssi, snr };
      history[histHead] = p;
      histHead = (histHead + 1) % MAX_HISTORY;
      if (histSize < MAX_HISTORY) histSize++;

      pushPacket(p);

      Serial.printf("Packet #%i: %s\n",  packetCount, rxdata.c_str());
      Serial.printf("  RSSI: %.2f dBm\n", rssi);
      Serial.printf("  SNR:  %.2f dB\n",  snr);

    } else {
      pushError(_radiolib_status);
      Serial.printf("RX Error: %i\n", _radiolib_status);
    }

    RADIOLIB_OR_HALT(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF));
  }
}
