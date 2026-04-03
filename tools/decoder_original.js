// TTN Payload Decoder (JavaScript)

function decodeUplink(input) {
    var bytes = input.bytes;
    var i = 0;
    var result = {};

    // Check minimum length
    if (bytes.length < 3) return { errors: ["Payload too short"] };

    result.schema_version = bytes[i++];
    result.edge_id = bytes[i++];
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
    
    return { data: result };
}

// For local Node.js testing:
if (typeof module !== "undefined" && module.exports) {
    console.log("Running local TTN decoder test...");
    
    // Fake BridgeAggV1 bytes (1 record: edge=1, src_id=3, temp=25.3, hum=65.5)
    // 25.3 = 253 = 0x00FD, 65.5 = 655 = 0x028F
    // Edge header: [0x02, 0x01, 0x01]
    // Record: [src=0x03, seq=0x1A, hops=0x02, uptime_hi=0x00, uptime_lo=0x78, plen=0x07]
    // Payload: [schema=0x01, type=0x03, temp_hi=0x00, temp_lo=0xFD, hum_hi=0x02, hum_lo=0x8F, status=0x00]

    const fakePayload = {
        bytes: [
            0x02, 0x01, 0x01,                          // Edge header
            0x03, 0x1A, 0x02, 0x00, 0x78, 0x07,        // Record meta (no sensor_type)
            0x01, 0x03, 0x00, 0xFD, 0x02, 0x8F, 0x00   // DHT payload
        ]
    };
    
    const res = decodeUplink(fakePayload);
    console.log("Decoded JSON:", JSON.stringify(res, null, 2));
}
