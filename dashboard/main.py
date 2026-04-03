import network, time, json
import socket
from umqtt.simple import MQTTClient

# WiFi Configuration
WIFI_SSID = "premierlin"
WIFI_PASS = "12345678"

# TTN Configuration
TTN_APP_ID = "sit-csc2106-g33"
TTN_API_KEY = "NNSXS.NRPKTDG3KGMLAO2KY37ZYTMJDKEN3CJMKZLCA7A.V7MH2LTQZSABZ6RZHOCQXSJBKZXLMK5U7BK7NBBLGED2DBNXRVFA"
TTN_SERVER = "au1.cloud.thethings.network"

# Wildcard '+' subscribes to all edge devices.
TOPIC_TEST = "csc2106-g33-dashboard-test/v3/sit-csc2106-g33@ttn/devices/+/up"
TOPIC_TTN = "v3/" + TTN_APP_ID + "@ttn/devices/+/up"
TOPIC = TOPIC_TTN if TTN_API_KEY else TOPIC_TEST

# State
nodes = {}            # keyed by src_id string
bridge_metrics = {}   # keyed by bridge_id string


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
    print("\nWiFi connection failed.")
    return None


def on_message(topic, msg):
    try:
        payload = json.loads(msg)
        decoded = payload.get("uplink_message", {}).get("decoded_payload", {})
        readings = decoded.get("readings", [])
        bridge_id = str(decoded.get("bridge_id", "?"))
        metrics = decoded.get("metrics", {})

        if metrics:
            bridge_metrics[bridge_id] = {
                "throughput_pps": metrics.get("throughput_pps", 0),
                "throughput_bps": metrics.get("throughput_bps", 0),
                "pdr_pct": metrics.get("pdr_pct", 0),
                "loss_pct": metrics.get("loss_pct", 0),
                "hop_avg": metrics.get("hop_avg", 0),
                "latency_avg_ms": metrics.get("latency_avg_ms", 0),
                "delivered_total": metrics.get("delivered_total", 0),
                "expected_total": metrics.get("expected_total", 0),
                "lost_total": metrics.get("lost_total", 0),
                "window_ms": metrics.get("window_ms", 0),
            }

        for r in readings:
            sid = str(r.get("src_id", "?"))
            nodes[sid] = {
                "temp": r.get("temperature_c", "?"),
                "hum": r.get("humidity_pct", "?"),
                "hops": r.get("hops", "?"),
                "ok": r.get("sensor_ok", False),
                "bridge": bridge_id,
            }

        print("Updated", len(readings), "readings from bridge", bridge_id)
    except Exception as e:
        print("MQTT parse error:", e)


def dashboard_html():
    rows = ""
    metrics_rows = ""

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
            hum_str = "{:.1f}".format(d["hum"]) if isinstance(d["hum"], (int, float)) else str(d["hum"])

            rows += (
                "<tr>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08);font-family:monospace;font-size:1.1rem;font-weight:700'>"
                + "0x{:02X}".format(int(sid))
                + "</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'><b>" + temp_str + "</b> &deg;C</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'><b>" + hum_str + "</b> %</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + str(d["hops"]) + "</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'>"
                + "<span style='background:#0c1a2e;color:#93c5fd;border:1px solid #1e40af;padding:3px 10px;border-radius:999px;font-size:0.75rem;font-weight:700'>Bridge "
                + str(d["bridge"])
                + "</span>"
                + "</td>"
                + "<td style='padding:1rem 1.5rem;border-bottom:1px solid rgba(255,255,255,0.08)'>"
                + "<span style='" + badge_style + ";padding:3px 10px;border-radius:999px;font-size:0.75rem;font-weight:700'>" + status_txt + "</span>"
                + "</td>"
                + "</tr>"
            )

    if not bridge_metrics:
        metrics_rows = "<tr><td colspan='8' style='padding:1rem;text-align:center;color:#94a3b8;font-style:italic'>No metrics yet. Waiting for edge metric trailer...</td></tr>"
    else:
        for bid, m in sorted(bridge_metrics.items()):
            pps = "{:.2f}".format(m["throughput_pps"]) if isinstance(m["throughput_pps"], (int, float)) else str(m["throughput_pps"])
            bps = str(m["throughput_bps"])
            pdr = "{:.2f}".format(m["pdr_pct"]) if isinstance(m["pdr_pct"], (int, float)) else str(m["pdr_pct"])
            loss = "{:.2f}".format(m["loss_pct"]) if isinstance(m["loss_pct"], (int, float)) else str(m["loss_pct"])
            hop_avg = "{:.2f}".format(m["hop_avg"]) if isinstance(m["hop_avg"], (int, float)) else str(m["hop_avg"])
            lat = str(m["latency_avg_ms"])
            totals = "{}/{}/{}".format(m["delivered_total"], m["expected_total"], m["lost_total"])
            window = str(m["window_ms"])

            metrics_rows += (
                "<tr>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08);font-weight:700'>Bridge " + str(bid) + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + pps + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + bps + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + pdr + "%</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + loss + "%</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + hop_avg + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + lat + "</td>"
                + "<td style='padding:0.75rem 1rem;border-bottom:1px solid rgba(255,255,255,0.08)'>" + totals + " (w=" + window + "ms)</td>"
                + "</tr>"
            )

    page = (
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='10'>"
        "<title>G33 Tunnel Dashboard</title>"
        "</head>"
        "<body style='margin:0;padding:2rem 1rem;background:#0f172a;color:#f8fafc;"
        "font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif'>"
        "<div style='max-width:1100px;margin:0 auto'>"
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
        "</table></div>"
        "<div style='height:1rem'></div>"
        "<div style='background:#1e293b;border-radius:10px;overflow:hidden;border:1px solid rgba(255,255,255,0.1)'>"
        "<div style='padding:0.75rem 1rem;background:rgba(0,0,0,0.25);color:#94a3b8;font-size:0.75rem;text-transform:uppercase;letter-spacing:0.05em'>"
        "Network Metrics (Latency / Throughput / PDR / Loss / Hop Avg)"
        "</div>"
        "<table style='width:100%;border-collapse:collapse;text-align:left'>"
        "<thead><tr style='background:rgba(0,0,0,0.18)'>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>Bridge</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>Throughput (pkt/s)</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>Throughput (bps)</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>PDR</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>Loss</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>Hop Avg</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>Latency Avg (ms)</th>"
        "<th style='padding:0.6rem 1rem;font-size:0.7rem;text-transform:uppercase;color:#94a3b8'>Delivered/Expected/Lost</th>"
        "</tr></thead>"
        "<tbody>" + metrics_rows + "</tbody>"
        "</table></div>"
        "</div></body></html>"
    )
    return page


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
            if e.args[0] == 11:
                time.sleep(0.05)
            else:
                print("Socket error:", e)
                break
    time.sleep(0.1)


def main():
    ip = connect_wifi()
    if not ip:
        print("WiFi failed. Rebooting in 10s...")
        time.sleep(10)
        import machine
        machine.reset()

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

    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", 80))
    server.listen(1)
    server.settimeout(0.5)
    print("====================================")
    print("Dashboard at http://{}".format(ip))
    print("====================================")

    while True:
        try:
            client.check_msg()
        except Exception as e:
            print("MQTT error, reconnecting:", e)
            try:
                client.connect()
                client.subscribe(TOPIC)
            except Exception:
                pass

        try:
            cl, addr = server.accept()
            cl.settimeout(5)
            print("HTTP from", addr)
            try:
                cl.recv(1024)
                send_response(cl, dashboard_html())
            finally:
                cl.close()
        except OSError:
            pass

        time.sleep(0.05)


if __name__ == "__main__":
    main()
