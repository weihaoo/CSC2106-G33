#!/usr/bin/env python3
"""
LoRa Mesh Metrics Dashboard for Raspberry Pi
CSC2106 Group 33

Runs a web dashboard that subscribes to TTN MQTT and displays
real-time sensor data with metrics (latency, PDR, throughput).

Usage:
    pip install paho-mqtt flask
    python main_pi.py

Then open http://<pi-ip>:5000 in your browser.
"""

import json
import time
import threading
from flask import Flask, render_template_string
import paho.mqtt.client as mqtt

# ── TTN Configuration ──
TTN_APP_ID  = "sit-csc2106-g33"
TTN_API_KEY = "NNSXS.NRPKTDG3KGMLAO2KY37ZYTMJDKEN3CJMKZLCA7A.V7MH2LTQZSABZ6RZHOCQXSJBKZXLMK5U7BK7NBBLGED2DBNXRVFA"
TTN_SERVER  = "au1.cloud.thethings.network"
TTN_PORT    = 1883

# Wildcard '+' subscribes to ALL edge devices
TOPIC_TTN = f"v3/{TTN_APP_ID}@ttn/devices/+/up"

# ── Metrics Configuration ──
THROUGHPUT_WINDOW_S = 30
MAX_LATENCY_HISTORY = 10

# ── State ──
nodes = {}  # keyed by src_id string
lock = threading.Lock()

# ── Metrics State ──
metrics = {
    "total_packets": 0,
    "total_lost": 0,
    "bytes_received": [],  # List of (timestamp, bytes) tuples
    "start_time": None
}

node_metrics = {}  # keyed by src_id

def init_node_metrics(sid):
    if sid not in node_metrics:
        node_metrics[sid] = {
            "last_seq": None,
            "latencies": [],
            "hops": [],
            "packets_received": 0,
            "packets_lost": 0
        }

def update_metrics(readings, payload_size):
    global metrics
    
    if metrics["start_time"] is None:
        metrics["start_time"] = time.time()
    
    now = time.time()
    metrics["bytes_received"].append((now, payload_size))
    
    # Prune old throughput data
    cutoff = now - THROUGHPUT_WINDOW_S
    metrics["bytes_received"] = [(t, b) for t, b in metrics["bytes_received"] if t > cutoff]
    
    for r in readings:
        sid = str(r.get("src_id", "?"))
        seq = r.get("seq", 0)
        latency_ms = r.get("latency_ms", 0)
        hops = r.get("hops", 0)
        
        init_node_metrics(sid)
        nm = node_metrics[sid]
        
        if latency_ms > 0:
            nm["latencies"].append(latency_ms)
            if len(nm["latencies"]) > MAX_LATENCY_HISTORY:
                nm["latencies"].pop(0)
        
        nm["hops"].append(hops)
        if len(nm["hops"]) > MAX_LATENCY_HISTORY:
            nm["hops"].pop(0)
        
        nm["packets_received"] += 1
        metrics["total_packets"] += 1
        
        if nm["last_seq"] is not None:
            expected_seq = (nm["last_seq"] + 1) % 256
            if seq != expected_seq:
                if seq > nm["last_seq"]:
                    gap = seq - nm["last_seq"] - 1
                else:
                    gap = (256 - nm["last_seq"]) + seq - 1
                gap = min(gap, 10)
                nm["packets_lost"] += gap
                metrics["total_lost"] += gap
        
        nm["last_seq"] = seq

def get_throughput_bps():
    if not metrics["bytes_received"]:
        return 0
    now = time.time()
    cutoff = now - THROUGHPUT_WINDOW_S
    recent = [(t, b) for t, b in metrics["bytes_received"] if t > cutoff]
    if not recent:
        return 0
    total_bytes = sum(b for _, b in recent)
    elapsed = now - recent[0][0]
    return total_bytes / elapsed if elapsed > 0 else 0

def get_overall_pdr():
    total_rx = metrics["total_packets"]
    total_lost = metrics["total_lost"]
    total_sent = total_rx + total_lost
    return (total_rx / total_sent) * 100 if total_sent > 0 else 100.0

def get_node_pdr(sid):
    if sid not in node_metrics:
        return 100.0
    nm = node_metrics[sid]
    total_rx = nm["packets_received"]
    total_lost = nm["packets_lost"]
    total_sent = total_rx + total_lost
    return (total_rx / total_sent) * 100 if total_sent > 0 else 100.0

def get_node_avg_latency(sid):
    if sid not in node_metrics or not node_metrics[sid]["latencies"]:
        return None
    lat = node_metrics[sid]["latencies"]
    return sum(lat) // len(lat)

# ── MQTT Callbacks ──
def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"Connected to TTN MQTT broker")
        client.subscribe(TOPIC_TTN)
        print(f"Subscribed to: {TOPIC_TTN}")
    else:
        print(f"MQTT connection failed with code {rc}")

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        decoded = payload.get("uplink_message", {}).get("decoded_payload", {})
        readings = decoded.get("readings", [])
        bridge_id = decoded.get("bridge_id", "?")
        
        payload_size = len(readings) * 20
        
        with lock:
            update_metrics(readings, payload_size)
            
            for r in readings:
                sid = str(r.get("src_id", "?"))
                nodes[sid] = {
                    "temp": r.get("temperature_c", "?"),
                    "hum": r.get("humidity_pct", "?"),
                    "hops": r.get("hops", "?"),
                    "latency_ms": r.get("latency_ms", 0),
                    "seq": r.get("seq", 0),
                    "ok": r.get("sensor_ok", False),
                    "bridge": bridge_id,
                }
        
        print(f"Updated {len(readings)} readings from bridge {bridge_id}")
    except Exception as e:
        print(f"MQTT parse error: {e}")

# ── Flask App ──
app = Flask(__name__)

DASHBOARD_HTML = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta http-equiv="refresh" content="5">
    <title>G33 Mesh Metrics Dashboard</title>
    <style>
        * { box-sizing: border-box; }
        body {
            margin: 0; padding: 2rem 1rem;
            background: #0f172a; color: #f8fafc;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
        }
        .container { max-width: 1200px; margin: 0 auto; }
        .header {
            display: flex; justify-content: space-between; align-items: center;
            border-bottom: 1px solid rgba(255,255,255,0.1);
            padding-bottom: 1rem; margin-bottom: 2rem;
        }
        h1 { font-size: 1.5rem; font-weight: 700; color: #60a5fa; margin: 0; }
        .subtitle { font-size: 0.8rem; color: #94a3b8; }
        .live-badge {
            font-size: 0.8rem; color: #94a3b8;
            background: rgba(255,255,255,0.05);
            padding: 6px 14px; border-radius: 999px;
            border: 1px solid rgba(255,255,255,0.1);
        }
        .summary {
            display: grid; grid-template-columns: repeat(4, 1fr);
            gap: 1rem; margin-bottom: 2rem;
        }
        @media (max-width: 768px) {
            .summary { grid-template-columns: repeat(2, 1fr); }
        }
        .stat-card {
            background: #1e293b; padding: 1rem; border-radius: 8px;
            border: 1px solid rgba(255,255,255,0.1);
        }
        .stat-label {
            font-size: 0.7rem; color: #94a3b8;
            text-transform: uppercase; letter-spacing: 0.05em;
        }
        .stat-value { font-size: 1.5rem; font-weight: 700; }
        .stat-unit { font-size: 0.8rem; }
        .table-container {
            background: #1e293b; border-radius: 10px; overflow: hidden;
            border: 1px solid rgba(255,255,255,0.1);
        }
        table { width: 100%; border-collapse: collapse; text-align: left; }
        th {
            padding: 0.6rem 1rem; font-size: 0.65rem;
            text-transform: uppercase; color: #94a3b8;
            letter-spacing: 0.05em; background: rgba(0,0,0,0.25);
        }
        td {
            padding: 0.75rem 1rem;
            border-bottom: 1px solid rgba(255,255,255,0.08);
        }
        .node-id { font-family: monospace; font-size: 1rem; font-weight: 700; }
        .badge {
            padding: 3px 8px; border-radius: 999px;
            font-size: 0.7rem; font-weight: 700;
        }
        .badge-ok { background: #16a34a; color: #fff; border: 1px solid #15803d; }
        .badge-err { background: #dc2626; color: #fff; border: 1px solid #b91c1c; }
        .badge-bridge {
            background: #0c1a2e; color: #93c5fd;
            border: 1px solid #1e40af; padding: 3px 10px;
        }
        .green { color: #22c55e; }
        .yellow { color: #eab308; }
        .red { color: #ef4444; }
        .blue { color: #60a5fa; }
        .empty-msg {
            padding: 2rem; text-align: center;
            color: #94a3b8; font-style: italic;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <div>
                <h1>LoRa Mesh Metrics Dashboard</h1>
                <div class="subtitle">CSC2106 Group 33 — Latency Branch</div>
            </div>
            <span class="live-badge">● Live</span>
        </div>
        
        <div class="summary">
            <div class="stat-card">
                <div class="stat-label">Throughput</div>
                <div class="stat-value blue">{{ "%.1f"|format(throughput) }} <span class="stat-unit">B/s</span></div>
            </div>
            <div class="stat-card">
                <div class="stat-label">PDR</div>
                <div class="stat-value {{ pdr_class }}">{{ "%.1f"|format(pdr) }}%</div>
            </div>
            <div class="stat-card">
                <div class="stat-label">Packets RX</div>
                <div class="stat-value green">{{ total_rx }}</div>
            </div>
            <div class="stat-card">
                <div class="stat-label">Packets Lost</div>
                <div class="stat-value red">{{ total_lost }}</div>
            </div>
        </div>
        
        <div class="table-container">
            <table>
                <thead>
                    <tr>
                        <th>Node</th>
                        <th>Temp</th>
                        <th>Humidity</th>
                        <th>Hops</th>
                        <th>Latency</th>
                        <th>PDR</th>
                        <th>Bridge</th>
                        <th>Status</th>
                    </tr>
                </thead>
                <tbody>
                    {% if not nodes %}
                    <tr>
                        <td colspan="8" class="empty-msg">No data yet. Waiting for TTN uplinks...</td>
                    </tr>
                    {% else %}
                    {% for sid, d in nodes|dictsort %}
                    <tr>
                        <td class="node-id">0x{{ "%02X"|format(sid|int) }}</td>
                        <td><b>{{ "%.1f"|format(d.temp) if d.temp is number else d.temp }}</b> °C</td>
                        <td><b>{{ "%.1f"|format(d.hum) if d.hum is number else d.hum }}</b> %</td>
                        <td>{{ d.hops }}</td>
                        <td class="blue">{{ d.latency_ms if d.latency_ms else "-" }} ms</td>
                        <td class="{{ d.pdr_class }}">{{ "%.0f"|format(d.pdr) }}%</td>
                        <td><span class="badge badge-bridge">Bridge {{ d.bridge }}</span></td>
                        <td>
                            <span class="badge {{ 'badge-ok' if d.ok else 'badge-err' }}">
                                {{ "OK" if d.ok else "ERR" }}
                            </span>
                        </td>
                    </tr>
                    {% endfor %}
                    {% endif %}
                </tbody>
            </table>
        </div>
    </div>
</body>
</html>
"""

@app.route("/")
def dashboard():
    with lock:
        throughput = get_throughput_bps()
        pdr = get_overall_pdr()
        total_rx = metrics["total_packets"]
        total_lost = metrics["total_lost"]
        
        # Add per-node PDR to nodes dict for template
        nodes_with_pdr = {}
        for sid, d in nodes.items():
            node_pdr = get_node_pdr(sid)
            pdr_class = "green" if node_pdr >= 95 else ("yellow" if node_pdr >= 80 else "red")
            nodes_with_pdr[sid] = {**d, "pdr": node_pdr, "pdr_class": pdr_class}
    
    pdr_class = "green" if pdr >= 95 else ("yellow" if pdr >= 80 else "red")
    
    return render_template_string(
        DASHBOARD_HTML,
        throughput=throughput,
        pdr=pdr,
        pdr_class=pdr_class,
        total_rx=total_rx,
        total_lost=total_lost,
        nodes=nodes_with_pdr
    )

@app.route("/api/metrics")
def api_metrics():
    """JSON API endpoint for metrics."""
    with lock:
        return {
            "throughput_bps": get_throughput_bps(),
            "pdr_overall": get_overall_pdr(),
            "total_packets": metrics["total_packets"],
            "total_lost": metrics["total_lost"],
            "nodes": {
                sid: {
                    **d,
                    "pdr": get_node_pdr(sid),
                    "avg_latency_ms": get_node_avg_latency(sid)
                }
                for sid, d in nodes.items()
            }
        }

def mqtt_thread():
    """Run MQTT client in background thread."""
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pi-g33-dashboard")
    client.username_pw_set(f"{TTN_APP_ID}@ttn", TTN_API_KEY)
    client.on_connect = on_connect
    client.on_message = on_message
    
    print(f"Connecting to MQTT: {TTN_SERVER}:{TTN_PORT}")
    
    while True:
        try:
            client.connect(TTN_SERVER, TTN_PORT, keepalive=60)
            client.loop_forever()
        except Exception as e:
            print(f"MQTT error: {e}, reconnecting in 5s...")
            time.sleep(5)

def main():
    # Start MQTT in background
    mqtt_t = threading.Thread(target=mqtt_thread, daemon=True)
    mqtt_t.start()
    
    print("=" * 50)
    print("Dashboard starting at http://0.0.0.0:5000")
    print("=" * 50)
    
    # Run Flask (use 0.0.0.0 to allow external access)
    app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)

if __name__ == "__main__":
    main()
