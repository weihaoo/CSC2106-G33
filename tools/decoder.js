// TTN Payload Decoder (JavaScript)
// Paste the decodeUplink function directly into TTN's Payload formatter -> Uplink -> Custom Javascript

function decodeUplink(input) {
    var bytes = input.bytes;
    var i = 0;
    var result = {};
    var METRICS_TAG = 0xA1;
    var METRICS_VER = 0x01;

    function readU16(pos) {
        return (bytes[pos] << 8) | bytes[pos + 1];
    }

    function readU32(pos) {
        return (((bytes[pos] << 24) >>> 0)
              | (bytes[pos + 1] << 16)
              | (bytes[pos + 2] << 8)
              | bytes[pos + 3]) >>> 0;
    }

    // Check minimum length
    if (bytes.length < 3) return { errors: ["Payload too short"] };

    result.schema_version = bytes[i++];
    result.bridge_id = bytes[i++];
    var record_count = bytes[i++];
    result.record_count = record_count;
    result.readings = [];

    // Variable-length records (no sensor_type_hint field)
    for (var r = 0; r < record_count; r++) {
        if (i + 6 > bytes.length) break; // Minimum 6 bytes per record header

        var rec = {};
        rec.src_id         = bytes[i++];
        rec.seq            = bytes[i++];
        rec.hops           = bytes[i++];
        rec.edge_uptime_s  = (bytes[i] << 8) | bytes[i+1]; i += 2;
        var plen           = bytes[i++];

        if (i + plen > bytes.length) break; // Prevent out-of-bounds

        // Parse DHT22 SensorPayload (7 bytes: schema, type, temp_hi, temp_lo, hum_hi, hum_lo, status)
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

    // Optional metrics trailer appended by edge node.
    // Format (32 bytes):
    //   [0]   tag=0xA1
    //   [1]   version=0x01
    //   [2-5] window_ms
    //   [6-7] throughput_pps_x100
    //   [8-11] throughput_bps
    //   [12-13] pdr_x100
    //   [14-15] loss_x100
    //   [16-19] delivered_total
    //   [20-23] expected_total
    //   [24-27] lost_total
    //   [28-29] hop_avg_x100
    //   [30-31] latency_avg_ms
    if (i + 32 <= bytes.length && bytes[i] === METRICS_TAG && bytes[i + 1] === METRICS_VER) {
        var m = i + 2;
        var throughput_pps_x100 = readU16(m + 4);
        var pdr_x100 = readU16(m + 10);
        var loss_x100 = readU16(m + 12);
        var hop_avg_x100 = readU16(m + 26);

        result.metrics = {
            window_ms: readU32(m + 0),
            throughput_pps: throughput_pps_x100 / 100.0,
            throughput_bps: readU32(m + 6),
            pdr_pct: pdr_x100 / 100.0,
            loss_pct: loss_x100 / 100.0,
            delivered_total: readU32(m + 14),
            expected_total: readU32(m + 18),
            lost_total: readU32(m + 22),
            hop_avg: hop_avg_x100 / 100.0,
            latency_avg_ms: readU16(m + 28)
        };
    }
    
    return { data: result };
}

// For local Node.js testing:
if (typeof module !== "undefined" && module.exports) {
    console.log("Running local TTN decoder test...");
    
    // Fake BridgeAggV1 bytes (1 record: bridge=1, src_id=3, temp=25.3, hum=65.5)
    // 25.3 = 253 = 0x00FD, 65.5 = 655 = 0x028F
    // Edge header: [0x02, 0x01, 0x01]
    // Record: [src=0x03, seq=0x1A, hops=0x02, uptime_hi=0x00, uptime_lo=0x78, plen=0x07]
    // Payload: [schema=0x01, type=0x03, temp_hi=0x00, temp_lo=0xFD, hum_hi=0x02, hum_lo=0x8F, status=0x00]

    const fakePayload = {
        bytes: [
            0x02, 0x01, 0x01,                          // Bridge header
            0x03, 0x1A, 0x02, 0x00, 0x78, 0x07,        // Record meta (no sensor_type)
            0x01, 0x03, 0x00, 0xFD, 0x02, 0x8F, 0x00,  // DHT payload
            0xA1, 0x01,                                // Metrics tag/version
            0x00, 0x00, 0x3A, 0x98,                    // window_ms = 15000
            0x00, 0x1E,                                // throughput_pps_x100 = 30 (0.30 pkt/s)
            0x00, 0x00, 0x0A, 0xF0,                    // throughput_bps = 2800
            0x27, 0x10,                                // pdr_x100 = 10000 (100.00%)
            0x00, 0x00,                                // loss_x100 = 0 (0.00%)
            0x00, 0x00, 0x00, 0x64,                    // delivered_total = 100
            0x00, 0x00, 0x00, 0x64,                    // expected_total = 100
            0x00, 0x00, 0x00, 0x00,                    // lost_total = 0
            0x00, 0xC8,                                // hop_avg_x100 = 200 (2.00)
            0x00, 0x96                                 // latency_avg_ms = 150
        ]
    };
    
    const res = decodeUplink(fakePayload);
    console.log("Decoded JSON:", JSON.stringify(res, null, 2));
}
