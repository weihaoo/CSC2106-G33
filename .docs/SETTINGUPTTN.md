# Setting Up TTN (The Things Network)

This guide provides step-by-step instructions for **Part 4**. It covers how to configure the WisGate gateways, create the TTN Application, and register the Edge Nodes without waiting for the other node firmware to be finished.

## 1. WisGate Gateway Configuration

**Requirement:** Two RAK WisGate gateways.

1. **Hardware Power-on:** Attach the LoRa antenna **first** before applying power (12V adapter or PoE). Do not power on without the antenna attached.
2. **Access the Web UI:**
   - Connect your PC to the gateway's Wi-Fi hotspot (`RAK7268_XXXX`).
   - Open your browser and navigate to `192.168.230.1`.
   - Log in using credentials: Username `root` / Password `admin12345678!`.
3. **Configure the Packet Forwarder:**
   - Go to **LoRa Network** > **Packet Forwarder**.
   - Set protocol to `Basic Station`.
   - Server URL: `wss://as1.cloud.thethings.network:8887` (Ensure it is the Singapore cluster `as1`).
4. **Network Connection:** Connect the WisGate to your local Lab Wi-Fi under **Network** > **Wi-Fi** > **DHCP Client**.
5. **Gateway EUI:** Locate the Gateway EUI on the sticker beneath the device. **Save this EUI.**
6. *Repeat for the second gateway.*

---

## 2. TTN Console Setup

1. **Log In:** Go to the [TTN Console](https://console.cloud.thethings.network/) and select the **Asia 1 (`as1`)** cluster.

### Adding Gateways
1. Navigate to **Gateways** > **Register Gateway**.
2. Input the Gateway EUI from your physical device.
3. Select Frequency Plan: **Asia 920–923 MHz (AS923 Group 1)**.
4. Click Register. Ensure the status turns to a green **Connected**.
5. Repeat for the second gateway.

### Creating the Application
1. Navigate to **Applications** > **Add Application**.
2. Application ID: `csc2106-g33-mesh`
3. Click **Create Application**.

---

## 3. Registering Edge Nodes

You must create two end devices (one for Edge 1, one for Edge 2) within your TTN Application so Person 3 can hardcode their specific OTAA keys into the firmware.

1. In your `csc2106-g33-mesh` application, click **Register End Device**.
2. Choose **Enter end device specifics manually**.
3. **Configuration:**
   - Frequency plan: `Asia 920–923 MHz (AS923 Group 1)`
   - LoRaWAN version: `1.0.2`
   - Regional Parameters version: `RP001 Regional Parameters 1.0.2`
   - Show Advanced Activation, set Activation Mode: `Over the air activation (OTAA)`
4. **Keys:**
   - JoinEUI (AppEUI): `00 00 00 00 00 00 00 00` (All zeros).
   - DevEUI: Click **Generate**
   - AppKey: Click **Generate**
5. End Device ID: `edge-bridge-01`
6. Click **Register**.
7. **Important:** Copy the generated `DevEUI` and `AppKey` somewhere safe. Tell Person 3 that they need the `DevEUI` in **LSB format** and the `AppKey` in **MSB format** when coding their node.
8. Repeat the process for your second node (`edge-bridge-02`).

---

## 4. Setup Payload Decoder

This ensures TTN successfully formats your raw packets into JSON so the dashboard can parse it nicely.

1. Under your application, go to **Payload formatters** > **Uplink**.
2. Formatter type: **Custom JavaScript**.
3. Open the `tools/decoder.js` file from this project. Copy the `decodeUplink` function exactly as written, and paste it into the editor.
4. Click **Save changes**.

---

## 5. Enable MQTT Integration (For Dashboard)

This enables the Pico W to automatically receive the messages from TTN.

1. Go to **Integrations** > **MQTT**.
2. Click **Generate new API key**. Provide it a name (e.g., "Dashboard Key").
3. **Save the generated API key immediately** — you cannot view it again once you close the window.
4. Note your credentials down:
   - Server: `as1.cloud.thethings.network`
   - Port: `1883`
   - Username: `csc2106-g33-mesh@ttn`
   - Password: `<The API Key you just generated>`
5. Paste these credentials into the `dashboard/main.py` file!
