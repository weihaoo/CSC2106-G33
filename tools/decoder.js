// TTN Payload Decoder (JavaScript)
// Paste the decodeUplink function directly into TTN's Payload formatter -> Uplink -> Custom Javascript
//
// BridgeAggV1 format (with latency):
//   Header (3 bytes): schema_version, bridge_id, record_count
//   Each record (8 bytes + opaque_payload):
//     [0] src_id, [1] seq, [2] hops, [3-4] edge_uptime_s, [5-6] latency_ms, [7] opaque_len, [8+] payload

function decodeUplink(input) {
    var bytes = input.bytes;
    var i = 0;
    var result = {};

    // Check minimum length
    if (bytes.length < 3) return { errors: ["Payload too short"] };

    result.schema_version = bytes[i++];
    result.bridge_id = bytes[i++];
    var record_count = bytes[i++];
    result.record_count = record_count;
    result.readings = [];

    // Variable-length records (8 bytes header + opaque payload)
    for (var r = 0; r < record_count; r++) {
        if (i + 8 > bytes.length) break; // Minimum 8 bytes per record header

        var rec = {};
        rec.src_id         = bytes[i++];
        rec.seq            = bytes[i++];
        rec.hops           = bytes[i++];
        rec.edge_uptime_s  = (bytes[i] << 8) | bytes[i+1]; i += 2;
        rec.latency_ms     = (bytes[i] << 8) | bytes[i+1]; i += 2;  // New: latency field
        var plen           = bytes[i++];

        if (i + plen > bytes.length) break; // Prevent out-of-bounds

        // Parse DHT22 SensorPayload (7+ bytes: schema, type, temp, hum, status, [timestamp])
        if (plen >= 7) {
            var sv         = bytes[i];      // schema_version
            var st         = bytes[i+1];    // sensor_type
            var temp_raw   = (bytes[i+2] << 8) | bytes[i+3];
            var hum_raw    = (bytes[i+4] << 8) | bytes[i+5];
            var status     = bytes[i+6];

            // Handle signed 16-bit temperature
            if (temp_raw > 32767) temp_raw -= 65536;

            rec.temperature_c  = temp_raw / 10.0;
            rec.humidity_pct   = hum_raw / 10.0;
            rec.sensor_ok      = (status === 0);
        }

        i += plen; // Skip the full payload (handles any payload length)
        result.readings.push(rec);
    }
    
    return { data: result };
}

// For local Node.js testing:
if (typeof module !== "undefined" && module.exports) {
    console.log("Running local TTN decoder test...");
    
    // Updated test payload with latency field
    // Header: [0x02, 0x01, 0x01] = schema v2, bridge 1, 1 record
    // Record: [src=0x03, seq=0x1A, hops=0x02, uptime_hi=0x00, uptime_lo=0x78,
    //          latency_hi=0x00, latency_lo=0x7B (123ms), plen=0x07]
    // Payload: [schema=0x01, type=0x03, temp=25.3, hum=65.5, status=0x00]

    const fakePayload = {
        bytes: [
            0x02, 0x01, 0x01,                                    // Bridge header
            0x03, 0x1A, 0x02, 0x00, 0x78, 0x00, 0x7B, 0x07,      // Record meta with latency
            0x01, 0x03, 0x00, 0xFD, 0x02, 0x8F, 0x00             // DHT payload
        ]
    };
    
    const res = decodeUplink(fakePayload);
    console.log("Decoded JSON:", JSON.stringify(res, null, 2));
}
