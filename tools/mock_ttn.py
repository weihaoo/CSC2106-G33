import time
import json
import random

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Please install paho-mqtt: pip install paho-mqtt")
    exit(1)

# --- CONFIGURATION ---
# Default to an open test broker so you don't need a real TTN APP ID right now
TTN_APP_ID  = "csc2106-g33-mesh"
TTN_SERVER  = "test.mosquitto.org"
TTN_PORT    = 1883
TTN_API_KEY = "" # Not needed for mosquitto

# Update this topic in main.py if testing with Mosquitto
TOPIC       = f"csc2106-g33-dashboard-test/v3/{TTN_APP_ID}@ttn/devices/mock-edge-01/up"

def generate_payload():
    return {
        "end_device_ids": {
            "device_id": "mock-edge-01",
            "application_ids": {"application_id": TTN_APP_ID}
        },
        "uplink_message": {
            "f_port": 1,
            "decoded_payload": {
                "schema_version": 2,
                "bridge_id": 1 if random.random() > 0.5 else 6,  # Simulate Edge 1 or Edge 2
                "record_count": 2,
                "readings": [
                    {
                        "src_id": 3,
                        "seq": random.randint(0, 255),
                        "sensor_type": 3,
                        "hops": 2,
                        "edge_uptime_s": random.randint(100, 5000),
                        "temperature_c": round(random.uniform(25.0, 32.0), 1),
                        "humidity_pct": round(random.uniform(60.0, 90.0), 1),
                        "sensor_ok": random.random() > 0.1 # 10% chance to simulate a bad sensor
                    },
                    {
                        "src_id": 4,
                        "seq": random.randint(0, 255),
                        "sensor_type": 3,
                        "hops": 2,
                        "edge_uptime_s": random.randint(100, 5000),
                        "temperature_c": round(random.uniform(22.0, 26.0), 1),
                        "humidity_pct": round(random.uniform(55.0, 75.0), 1),
                        "sensor_ok": random.random() > 0.1
                    }
                ]
            }
        }
    }

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"Connected to MQTT Broker!")
    else:
        print(f"Failed to connect, return code {rc}")

client = mqtt.Client()
if TTN_API_KEY:
    client.username_pw_set(f"{TTN_APP_ID}@ttn", TTN_API_KEY)
client.on_connect = on_connect

print(f"Connecting to {TTN_SERVER}:{TTN_PORT}...")
try:
    client.connect(TTN_SERVER, TTN_PORT, 60)
except Exception as e:
    print(f"Connection failed: {e}")
    exit(1)

client.loop_start()

try:
    print("Publishing mock payloads every 10 seconds. Press Ctrl+C to stop.")
    while True:
        payload = generate_payload()
        msg = json.dumps(payload)
        client.publish(TOPIC, msg)
        print(f"\nPublished to {TOPIC}:")
        print(json.dumps(payload["uplink_message"]["decoded_payload"]["readings"], indent=2))
        time.sleep(10)
except KeyboardInterrupt:
    print("\nStopping mock publisher...")
    client.loop_stop()
