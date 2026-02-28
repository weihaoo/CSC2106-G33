// TTN Payload Decoder (JavaScript)
// Paste the decodeUplink function directly into TTN's Payload formatter -> Uplink -> Custom Javascript

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

    // 14 bytes per record
    for (var r = 0; r < record_count; r++) {
        if (i + 14 > bytes.length) break; // Prevent out-of-bounds error
        
        var rec = {};
        rec.src_id         = bytes[i++];
        rec.seq            = bytes[i++];
        rec.sensor_type    = bytes[i++];
        rec.hops           = bytes[i++];
        rec.edge_uptime_s  = (bytes[i] << 8) | bytes[i+1]; i += 2;
        var plen           = bytes[i++];

        // Parse DHT22 SensorPayload (7 bytes)
        var sv             = bytes[i++];  // schema_version
        var st             = bytes[i++];  // sensor_type
        var temp_raw       = (bytes[i] << 8) | bytes[i+1]; i += 2;
        var hum_raw        = (bytes[i] << 8) | bytes[i+1]; i += 2;
        var status         = bytes[i++];

        // Handle signed 16-bit temperature
        if (temp_raw > 32767) temp_raw -= 65536;

        rec.temperature_c  = temp_raw / 10.0;
        rec.humidity_pct   = hum_raw / 10.0;
        rec.sensor_ok      = (status === 0);
        
        result.readings.push(rec);
    }
    
    return { data: result };
}

// For local Node.js testing:
if (typeof module !== "undefined" && module.exports) {
    console.log("Running local TTN decoder test...");
    
    // Fake BridgeAggV1 bytes (1 record: bridge=1, src_id=3, temp=25.3, hum=65.5)
    // 25.3 = 253 = 0x00FD
    // 65.5 = 655 = 0x028F
    // Edge header: [0x02, 0x01, 0x01]
    // Record header: [0x03, 0x1A, 0x03, 0x02, 0x00, 0x78, 0x07] (src 3, seq 26, type 3, 2 hops, 120s uptime, len 7)
    // Payload: [0x01, 0x03, 0x00, 0xFD, 0x02, 0x8F, 0x00]
    
    const fakePayload = {
        bytes: [
            0x02, 0x01, 0x01, // Bridge header
            0x03, 0x1A, 0x03, 0x02, 0x00, 0x78, 0x07, // Record meta
            0x01, 0x03, 0x00, 0xFD, 0x02, 0x8F, 0x00  // DHT data
        ]
    };
    
    const res = decodeUplink(fakePayload);
    console.log("Decoded JSON:", JSON.stringify(res, null, 2));
}
