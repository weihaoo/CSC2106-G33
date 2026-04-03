import network, time, json
import socket
from umqtt.simple import MQTTClient

# ── WiFi Configuration ──
WIFI_SSID = "premierlin"
WIFI_PASS = "12345678"

# ── TTN Configuration ──
# For local testing, use the public test broker with no auth.
# Change TTN_SERVER back to "as1.cloud.thethings.network" and fill in TTN_API_KEY
# when connecting to real TTN.
TTN_APP_ID  = "sit-csc2106-g33"
TTN_API_KEY = "NNSXS.NRPKTDG3KGMLAO2KY37ZYTMJDKEN3CJMKZLCA7A.V7MH2LTQZSABZ6RZHOCQXSJBKZXLMK5U7BK7NBBLGED2DBNXRVFA"
TTN_SERVER  = "au1.cloud.thethings.network"

# Wildcard '+' subscribes to ALL edge devices (edge-01, edge-06, etc.)
TOPIC_TEST  = "csc2106-g33-dashboard-test/v3/sit-csc2106-g33@ttn/devices/+/up"
TOPIC_TTN   = "v3/" + TTN_APP_ID + "@ttn/devices/+/up"
TOPIC       = TOPIC_TTN if TTN_API_KEY else TOPIC_TEST

# ── Metrics Configuration ──
THROUGHPUT_WINDOW_S = 30  # 30-second rolling window for throughput
MAX_LATENCY_HISTORY = 10  # Keep last 10 latency samples per node

# ── State ──
nodes = {}  # keyed by src_id string

# ── Metrics State ──
metrics = {
    "total_packets": 0,
    "total_lost": 0,
    "bytes_received": [],  # List of (timestamp, bytes) tuples
    "start_time": None
}

# Per-node metrics tracking
node_metrics = {}  # keyed by src_id: {last_seq, latencies, hops, packets_received, packets_lost}

def init_node_metrics(sid):
    """Initialize metrics tracking for a new node."""
    if sid not in node_metrics:
        node_metrics[sid] = {
            "last_seq": None,
            "latencies": [],      # Recent latency samples
            "hops": [],           # Recent hop counts
            "packets_received": 0,
            "packets_lost": 0
        }

def update_metrics(readings, payload_size):
    """Update metrics from a new batch of readings."""
    global metrics
    
    if metrics["start_time"] is None:
        metrics["start_time"] = time.time()
    
    now = time.time()
    
    # Track bytes for throughput (approximate: payload_size per uplink)
    metrics["bytes_received"].append((now, payload_size))
    
    # Prune old throughput data (older than window)
    cutoff = now - THROUGHPUT_WINDOW_S
    metrics["bytes_received"] = [(t, b) for t, b in metrics["bytes_received"] if t > cutoff]
    
    for r in readings:
        sid = str(r.get("src_id", "?"))
        seq = r.get("seq", 0)
        latency_ms = r.get("latency_ms", 0)
        hops = r.get("hops", 0)
        
        init_node_metrics(sid)
        nm = node_metrics[sid]
        
        # Track latency (only if valid)
        if latency_ms > 0:
            nm["latencies"].append(latency_ms)
            if len(nm["latencies"]) > MAX_LATENCY_HISTORY:
                nm["latencies"].pop(0)
        
        # Track hop count
        nm["hops"].append(hops)
        if len(nm["hops"]) > MAX_LATENCY_HISTORY:
            nm["hops"].pop(0)
        
        # Calculate PDR from sequence gaps
        nm["packets_received"] += 1
        metrics["total_packets"] += 1
        
        if nm["last_seq"] is not None:
            expected_seq = (nm["last_seq"] + 1) % 256
            if seq != expected_seq:
                # Calculate gap (handle wraparound)
                if seq > nm["last_seq"]:
                    gap = seq - nm["last_seq"] - 1
                else:
                    gap = (256 - nm["last_seq"]) + seq - 1
                
                # Cap gap at reasonable value (avoid counting huge jumps as loss)
                gap = min(gap, 10)
                nm["packets_lost"] += gap
                metrics["total_lost"] += gap
        
        nm["last_seq"] = seq

def get_throughput_bps():
    """Calculate throughput in bytes per second over the rolling window."""
    if not metrics["bytes_received"]:
        return 0
    
    now = time.time()
    cutoff = now - THROUGHPUT_WINDOW_S
    recent = [(t, b) for t, b in metrics["bytes_received"] if t > cutoff]
    
    if not recent:
        return 0
    
    total_bytes = sum(b for _, b in recent)
    elapsed = now - recent[0][0]
    
    if elapsed <= 0:
        return 0
    
    return total_bytes / elapsed

def get_overall_pdr():
    """Calculate overall Packet Delivery Ratio."""
    total_rx = metrics["total_packets"]
    total_lost = metrics["total_lost"]
    total_sent = total_rx + total_lost
    
    if total_sent == 0:
        return 100.0
    
    return (total_rx / total_sent) * 100

def get_node_pdr(sid):
    """Calculate PDR for a specific node."""
    if sid not in node_metrics:
        return 100.0
    
    nm = node_metrics[sid]
    total_rx = nm["packets_received"]
    total_lost = nm["packets_lost"]
    total_sent = total_rx + total_lost
    
    if total_sent == 0:
        return 100.0
    
    return (total_rx / total_sent) * 100

def get_node_avg_latency(sid):
    """Get average latency for a node."""
    if sid not in node_metrics or not node_metrics[sid]["latencies"]:
        return None
    
    lat = node_metrics[sid]["latencies"]
    return sum(lat) // len(lat)

def get_node_avg_hops(sid):
    """Get average hop count for a node."""
    if sid not in node_metrics or not node_metrics[sid]["hops"]:
        return None
    
    hops = node_metrics[sid]["hops"]
    return sum(hops) / len(hops)

# ── WiFi Connection ──
def connect_wifi():
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    if not wlan.isconnected():
        print("Connecting to WiFi", WIFI_SSID, "...")
        wlan.connect(WIFI_SSID, WIFI_PASS)
        attempts = 0
        while not wlan.isconnected() and attempts < 20:
            time.sleep(0.5)
            attempts += 1
            print(".", end="")
    if wlan.isconnected():
        print("\nWiFi connected:", wlan.ifconfig())
        return wlan.ifconfig()[0]
    else:
        print("\nWiFi connection failed.")
        return None

# ── MQTT Callback ──
def on_message(topic, msg):
    try:
        payload = json.loads(msg)
        decoded = payload.get("uplink_message", {}).get("decoded_payload", {})
        readings = decoded.get("readings", [])
        bridge_id = decoded.get("bridge_id", "?")
        
        # Estimate payload size for throughput calculation (rough: ~20 bytes per reading)
        payload_size = len(readings) * 20
        
        # Update metrics with new readings
        update_metrics(readings, payload_size)
        
        for r in readings:
            sid = str(r.get("src_id", "?"))
            nodes[sid] = {
                "temp":       r.get("temperature_c", "?"),
                "hum":        r.get("humidity_pct", "?"),
                "hops":       r.get("hops", "?"),
                "latency_ms": r.get("latency_ms", 0),
                "seq":        r.get("seq", 0),
                "ok":         r.get("sensor_ok", False),
                "bridge":     bridge_id,
            }
        print("Updated", len(readings), "readings from bridge", bridge_id)
    except Exception as e:
        print("MQTT parse error:", e)

# ── Dashboard HTML ──
def dashboard_html():
    # Calculate summary metrics
    throughput = get_throughput_bps()
    overall_pdr = get_overall_pdr()
    total_rx = metrics["total_packets"]
    total_lost = metrics["total_lost"]
    
    # Build summary panel
    summary_html = (
        "<div style='display:grid;grid-template-columns:repeat(4,1fr);gap:1rem;margin-bottom:2rem'>"
        "<div style='background:#1e293b;padding:1rem;border-radius:8px;border:1px solid rgba(255,255,255,0.1)'>"
        "<div style='font-size:0.7rem;color:#94a3b8;text-transform:uppercase'>Throughput</div>"
        "<div style='font-size:1.5rem;font-weight:700;color:#60a5fa'>{:.1f} <span style='font-size:0.8rem'>B/s</span></div>"
        "</div>"
        "<div style='background:#1e293b;padding:1rem;border-radius:8px;border:1px solid rgba(255,255,255,0.1)'>"
        "<div style='font-size:0.7rem;color:#94a3b8;text-transform:uppercase'>PDR</div>"
        "<div style='font-size:1.5rem;font-weight:700;color:{}'>{:.1f}%</div>"
        "</div>"
        "<div style='background:#1e293b;padding:1rem;border-radius:8px;border:1px solid rgba(255,255,255,0.1)'>"
        "<div style='font-size:0.7rem;color:#94a3b8;text-transform:uppercase'>Packets RX</div>"
        "<div style='font-size:1.5rem;font-weight:700;color:#22c55e'>{}</div>"
        "</div>"
        "<div style='background:#1e293b;padding:1rem;border-radius:8px;border:1px solid rgba(255,255,255,0.1)'>"
        "<div style='font-size:0.7rem;color:#94a3b8;text-transform:uppercase'>Packets Lost</div>"
        "<div style='font-size:1.5rem;font-weight:700;color:#ef4444'>{}</div>"
        "</div>"
        "</div>"
    ).format(
        throughput,
        "#22c55e" if overall_pdr >= 95 else ("#eab308" if overall_pdr >= 80 else "#ef4444"),
        overall_pdr,
        total_rx,
        total_lost
    )
    
    # Build node rows
    rows = ""
    if not nodes:
        rows = "<tr><td colspan='9' style='padding:2rem;text-align:center;color:#94a3b8;font-style:italic'>No data yet. Waiting for TTN uplinks...</td></tr>"
    else:
        for sid, d in sorted(nodes.items()):
            ok = d["ok"]
            status_txt = "OK" if ok else "ERR"
            if ok:
                badge_style = "background:#16a34a;color:#ffffff;border:1px solid #15803d"
            else:
                badge_style = "background:#dc2626;color:#ffffff;border:1px solid #b91c1c"

            temp_str = "{:.1f}".format(d["temp"]) if isinstance(d["temp"], (int, float)) else str(d["temp"])
            hum_str  = "{:.1f}".format(d["hum"])  if isinstance(d["hum"],  (int, float)) else str(d["hum"])
            
            # Get per-node metrics
            avg_latency = get_node_avg_latency(sid)
            latency_str = "{}".format(avg_latency) if avg_latency else "-"
            node_pdr = get_node_pdr(sid)
            pdr_color = "#22c55e" if node_pdr >= 95 else ("#eab308" if node_pdr >= 80 else "#ef4444")

            rows += (
                "<tr>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08);font-family:monospace;font-size:1rem;font-weight:700'>"
                + "0x{:02X}".format(int(sid))
                + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'><b>" + temp_str + "</b> &deg;C</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'><b>" + hum_str  + "</b> %</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + str(d["hops"]) + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08);color:#60a5fa'>" + latency_str + " ms</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08);color:" + pdr_color + "'>{:.0f}%</td>".format(node_pdr)
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>"
                + "<span style='background:#0c1a2e;color:#93c5fd;border:1px solid #1e40af;padding:3px 10px;border-radius:999px;font-size:0.7rem;font-weight:700'>Bridge " + str(d["bridge"]) + "</span>"
                + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>"
                + "<span style='" + badge_style + ";padding:3px 8px;border-radius:999px;font-size:0.7rem;font-weight:700'>" + status_txt + "</span>"
                + "</td>"
                + "</tr>"
            )

    # Compact, no external CSS. All styles are inline.
    page = (
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='10'>"
        "<title>G33 Mesh Metrics Dashboard</title>"
        "</head>"
        "<body style='margin:0;padding:2rem 1rem;background:#0f172a;color:#f8fafc;"
        "font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif'>"
        "<div style='max-width:1200px;margin:0 auto'>"
        "<div style='display:flex;justify-content:space-between;align-items:center;"
        "border-bottom:1px solid rgba(255,255,255,0.1);padding-bottom:1rem;margin-bottom:2rem'>"
        "<div>"
        "<h1 style='font-size:1.5rem;font-weight:700;color:#60a5fa;margin:0'>LoRa Mesh Metrics Dashboard</h1>"
        "<div style='font-size:0.8rem;color:#94a3b8'>CSC2106 Group 33 — Latency Branch</div>"
        "</div>"
        "<span style='font-size:0.8rem;color:#94a3b8;background:rgba(255,255,255,0.05);"
        "padding:6px 14px;border-radius:999px;border:1px solid rgba(255,255,255,0.1)'>&#x25CF; Live</span>"
        "</div>"
        + summary_html +
        "<div style='background:#1e293b;border-radius:10px;overflow:hidden;border:1px solid rgba(255,255,255,0.1)'>"
        "<table style='width:100%;border-collapse:collapse;text-align:left'>"
        "<thead><tr style='background:rgba(0,0,0,0.25)'>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Node</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Temp</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Humidity</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Hops</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Latency</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>PDR</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Bridge</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.65rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Status</th>"
        "</tr></thead>"
        "<tbody>" + rows + "</tbody>"
        "</table></div></div></body></html>"
    )
    return page

# ── HTTP response sender (chunked for Pico W safety) ──
def send_response(cl, html):
    cl.send(b"HTTP/1.1 200 OK\r\n")
    cl.send(b"Content-Type: text/html\r\n")
    cl.send(b"Connection: close\r\n\r\n")
    data = html.encode("utf-8")
    total = len(data)
    sent = 0
    while sent < total:
        chunk = data[sent:sent + 512]
        try:
            n = cl.send(chunk)
            if n > 0:
                sent += n
        except OSError as e:
            if e.args[0] == 11:   # EAGAIN — buffer full, wait and retry
                time.sleep(0.05)
            else:
                print("Socket error:", e)
                break
    time.sleep(0.1)  # Flush before caller closes

# ── Main Entry ──
def main():
    ip = connect_wifi()
    if not ip:
        print("WiFi failed. Rebooting in 10s...")
        time.sleep(10)
        import machine
        machine.reset()

    # Connect to MQTT (no auth for test broker)
    if TTN_API_KEY:
        client = MQTTClient("picow-g33", TTN_SERVER,
                            user=TTN_APP_ID + "@ttn", password=TTN_API_KEY, port=1883)
    else:
        client = MQTTClient("picow-g33", TTN_SERVER, port=1883)

    client.set_callback(on_message)

    try:
        print("Connecting to MQTT:", TTN_SERVER)
        client.connect()
        client.subscribe(TOPIC)
        print("Subscribed:", TOPIC)
    except Exception as e:
        print("MQTT connect failed:", e)
        time.sleep(5)
        import machine
        machine.reset()

    # HTTP server (non-blocking via settimeout)
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", 80))
    server.listen(1)
    server.settimeout(0.5)
    print("====================================")
    print("Dashboard at http://{}".format(ip))
    print("====================================")

    while True:
        # Poll MQTT
        try:
            client.check_msg()
        except Exception as e:
            print("MQTT error, reconnecting:", e)
            try:
                client.connect()
                client.subscribe(TOPIC)
            except Exception:
                pass

        # Serve HTTP
        try:
            cl, addr = server.accept()
            cl.settimeout(5)  # prevent a slow client from blocking forever
            print("HTTP from", addr)
            try:
                cl.recv(1024)           # consume the request headers
                send_response(cl, dashboard_html())
            finally:
                cl.close()              # always close the socket, even on error
        except OSError:
            pass  # accept() timed out — normal in non-blocking mode

        time.sleep(0.05)

if __name__ == "__main__":
    main()
