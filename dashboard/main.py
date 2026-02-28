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
TTN_APP_ID  = "csc2106-g33-mesh"
TTN_API_KEY = ""
TTN_SERVER  = "test.mosquitto.org"
TOPIC       = "csc2106-g33-dashboard-test/v3/csc2106-g33-mesh@ttn/devices/mock-edge-01/up"

# ── State ──
nodes = {}  # keyed by src_id string

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
        for r in readings:
            sid = str(r.get("src_id", "?"))
            nodes[sid] = {
                "temp":   r.get("temperature_c", "?"),
                "hum":    r.get("humidity_pct", "?"),
                "hops":   r.get("hops", "?"),
                "ok":     r.get("sensor_ok", False),
                "bridge": bridge_id,
            }
        print("Updated", len(readings), "readings from bridge", bridge_id)
    except Exception as e:
        print("MQTT parse error:", e)

# ── Dashboard HTML ──
def dashboard_html():
    rows = ""
    if not nodes:
        rows = "<tr><td colspan='6' style='padding:2rem;text-align:center;color:#94a3b8;font-style:italic'>No data yet. Waiting for TTN uplinks...</td></tr>"
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

            rows += (
                "<tr>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08);font-family:monospace;font-size:1.1rem;font-weight:700'>"
                + "0x{:02X}".format(int(sid))
                + "</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'><b>" + temp_str + "</b> &deg;C</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'><b>" + hum_str  + "</b> %</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + str(d["hops"]) + "</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'>"
                + "<span style='background:#0c1a2e;color:#93c5fd;border:1px solid #1e40af;padding:3px 10px;border-radius:999px;font-size:0.75rem;font-weight:700'>Bridge " + str(d["bridge"]) + "</span>"
                + "</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'>"
                + "<span style='" + badge_style + ";padding:3px 10px;border-radius:999px;font-size:0.75rem;font-weight:700'>" + status_txt + "</span>"
                + "</td>"
                + "</tr>"
            )

    # Compact, no external CSS. All styles are inline.
    page = (
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='10'>"
        "<title>G33 Tunnel Dashboard</title>"
        "</head>"
        "<body style='margin:0;padding:2rem 1rem;background:#0f172a;color:#f8fafc;"
        "font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif'>"
        "<div style='max-width:1000px;margin:0 auto'>"
        "<div style='display:flex;justify-content:space-between;align-items:center;"
        "border-bottom:1px solid rgba(255,255,255,0.1);padding-bottom:1rem;margin-bottom:2rem'>"
        "<div>"
        "<h1 style='font-size:1.5rem;font-weight:700;color:#60a5fa;margin:0'>Tunnel Mesh Dashboard</h1>"
        "<div style='font-size:0.8rem;color:#94a3b8'>CSC2106 Group 33</div>"
        "</div>"
        "<span style='font-size:0.8rem;color:#94a3b8;background:rgba(255,255,255,0.05);"
        "padding:6px 14px;border-radius:999px;border:1px solid rgba(255,255,255,0.1)'>&#x25CF; Live</span>"
        "</div>"
        "<div style='background:#1e293b;border-radius:10px;overflow:hidden;border:1px solid rgba(255,255,255,0.1)'>"
        "<table style='width:100%;border-collapse:collapse;text-align:left'>"
        "<thead><tr style='background:rgba(0,0,0,0.25)'>"
        "<th style='padding:0.75rem 1.5rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Node</th>"
        "<th style='padding:0.75rem 1.5rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Temp</th>"
        "<th style='padding:0.75rem 1.5rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Humidity</th>"
        "<th style='padding:0.75rem 1.5rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Hops</th>"
        "<th style='padding:0.75rem 1.5rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Bridge</th>"
        "<th style='padding:0.75rem 1.5rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8;letter-spacing:0.05em'>Status</th>"
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
    time.sleep(0.1)  # Flush before close
    cl.close()

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
