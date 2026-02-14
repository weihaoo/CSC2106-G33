LoRa Mesh for Deep Tunnel Infrastructure Connectivity
Project Overview
This project implements a Gradient-Based LoRa Mesh Network designed for reliable telemetry monitoring in Singapore’s deep-tunnel infrastructure, such as the Cross Island Line (CRL) and Deep Tunnel Sewerage System (DTSS). The system addresses the extreme signal attenuation caused by reinforced concrete and tunnel curvature at depths of up to 70 meters.

Key Features
Gradient Routing Protocol: A rank-based "virtual slope" where data flows from high-rank nodes (deep underground) to Rank 0 nodes (surface gateways).
Abstraction: A pseudo-relay mechanism that forwards raw LoRaWAN PHYPayloads without decoding, allowing legacy sensor nodes to participate in the mesh.
Uplink-Dominant Design: Optimized for 90-95% uplink traffic, specifically for safety-critical gas and temperature monitoring.
Self-Healing Resilience: Automatic parent re-selection based on a weighted score of Rank and RSSI if a link is lost.

Hardware Configuration
The implementation is validated using the following hardware:

Microcontroller: Cytron Maker UNO (Arduino Uno Compatible).
LoRa Shield: Cytron LoRa-RFM Shield (RFM95W).
Frequency: 923 MHz (AS923 Singapore Standard).

Protocol Specification
1. Mesh Header (Encapsulation)
To maintain version-blind forwarding, every packet is wrapped in a custom header:

struct MeshHeader {
  uint8_t senderID;    // Unique ID of the node
  uint8_t receiverID;  // ID of the parent/target node
  uint8_t packetID;    // For deduplication and tracking
  uint8_t currentRank; // Hop count from the gateway
};

2. Heuristic Path Optimization
Nodes select their upstream parent by evaluating:
$$Score = (w_1 \times \text{Rank}) + (w_2 \times \text{RSSI})$$
Where the node prioritizes the lowest rank (closest to surface) with the most stable signal strength.

Reliability Mechanisms
Hop-by-Hop ACKs: Localized acknowledgments between mesh nodes to ensure delivery around tunnel bends.
TTL (Time-to-Live): Prevents infinite packet circulation in dense tunnel sections.
Deduplication: Nodes ignore repeated packetID frames to minimize airtime.

Project Group 
Kwong Ming Wei (2400730)
Ng Wei Hao (2401794)
Adam Lee Alfianfirdaus (2400523)
Lin Junyu (2401336)
Reynard Tan Suan Wee (2403346)
