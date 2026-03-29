// ============================================================================
// SENSOR ROUTING -- Parent selection logic for sensor nodes
// CSC2106 Group 33 -- LoRa Mesh Network
//
// Sensor-specific parent selection: single best parent with hysteresis.
// Shared scoring and beacon processing live in mesh_common.h.
// ============================================================================

#ifndef SENSOR_ROUTING_H
#define SENSOR_ROUTING_H

#include <Arduino.h>
#include "../shared/mesh_protocol.h"

// ============================================================================
// EXTERN DECLARATIONS (defined in sensor_node.ino)
// ============================================================================

extern ParentInfo candidates[];
extern uint8_t    current_parent_idx;
extern uint8_t    my_rank;

// ============================================================================
// FORWARD DECLARATIONS (defined in sensor_node.ino)
// ============================================================================

extern void broadcast_beacon();

// ============================================================================
// HAS VALID PARENT
// ============================================================================

inline bool has_valid_parent() {
    return (current_parent_idx != 0xFF && candidates[current_parent_idx].valid);
}

// ============================================================================
// GET PARENT ID
// ============================================================================

inline uint8_t get_parent_id() {
    if (!has_valid_parent()) return 0xFF;
    return candidates[current_parent_idx].node_id;
}

// ============================================================================
// UPDATE PARENT SELECTION
// Scores all valid, non-stale candidates and switches parent if better found.
// Hysteresis: only switch if new score >= PARENT_SWITCH_HYSTERESIS better.
// Also updates my_rank to parent.rank + 1.
// Sends a rapid beacon if parent changes (self-organizing).
// ============================================================================

inline void update_parent_selection() {
    uint8_t old_parent_id = get_parent_id();
    int     best_score = -1;
    uint8_t best_idx   = 0xFF;

    // Count valid candidates for debug header
    int valid_count = 0;
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (candidates[i].valid) {
            uint32_t age = millis() - candidates[i].last_seen_ms;
            if (age <= PARENT_TIMEOUT_MS) valid_count++;
        }
    }

    if (valid_count > 0) {
        Serial.println("─────────────────────────────────────────────");
        Serial.print("EVAL  | Comparing ");
        Serial.print(valid_count);
        Serial.println(" candidate(s):");
    }

    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (!candidates[i].valid) continue;

        // Skip stale candidates (self-healing)
        uint32_t age = millis() - candidates[i].last_seen_ms;
        if (age > PARENT_TIMEOUT_MS) continue;

        // Pass debug=true and node_id to show score breakdown
        int s = score_parent(candidates[i].rank,
                             candidates[i].rssi,
                             candidates[i].queue_pct,
                             candidates[i].parent_health,
                             true,   // debug output enabled
                             candidates[i].node_id);
        if (s > best_score) {
            best_score = s;
            best_idx   = i;
        }
    }

    if (best_idx == 0xFF) return;  // no valid candidates

    if (current_parent_idx == 0xFF) {
        // No current parent -- take the best one immediately
        current_parent_idx = best_idx;
        my_rank = candidates[current_parent_idx].rank + 1;
        Serial.print("PARENT | Selected 0x");
        Serial.print(candidates[current_parent_idx].node_id, HEX);
        Serial.print(" (score=");
        Serial.print(best_score);
        Serial.print(", my_rank=");
        Serial.print(my_rank);
        Serial.println(")");
        Serial.println("─────────────────────────────────────────────");
    } else {
        // We have a current parent -- only switch if score improvement >= hysteresis
        int current_score = score_parent(candidates[current_parent_idx].rank,
                                         candidates[current_parent_idx].rssi,
                                         candidates[current_parent_idx].queue_pct,
                                         candidates[current_parent_idx].parent_health);
        int score_diff = best_score - current_score;

        if (best_score >= current_score + PARENT_SWITCH_HYSTERESIS
            && best_idx != current_parent_idx) {
            Serial.print("PARENT | Switching from 0x");
            Serial.print(candidates[current_parent_idx].node_id, HEX);
            Serial.print(" (score=");
            Serial.print(current_score);
            Serial.print(") to 0x");
            Serial.print(candidates[best_idx].node_id, HEX);
            Serial.print(" (score=");
            Serial.print(best_score);
            Serial.print(", diff=+");
            Serial.print(score_diff);
            Serial.print(" >= hysteresis=");
            Serial.print(PARENT_SWITCH_HYSTERESIS);
            Serial.println(")");
            current_parent_idx = best_idx;
            my_rank = candidates[current_parent_idx].rank + 1;
            Serial.println("─────────────────────────────────────────────");
        } else if (best_idx != current_parent_idx && score_diff > 0) {
            // Better candidate exists but doesn't meet hysteresis threshold
            Serial.print("PARENT | Keeping 0x");
            Serial.print(candidates[current_parent_idx].node_id, HEX);
            Serial.print(" (best=0x");
            Serial.print(candidates[best_idx].node_id, HEX);
            Serial.print(" diff=+");
            Serial.print(score_diff);
            Serial.print(" < hysteresis=");
            Serial.print(PARENT_SWITCH_HYSTERESIS);
            Serial.println(")");
            Serial.println("─────────────────────────────────────────────");
        }
    }

    // Rapid beacon if parent changed (self-organizing)
    uint8_t new_parent_id = get_parent_id();
    if (new_parent_id != old_parent_id && new_parent_id != 0xFF) {
        Serial.println("PARENT | Topology changed, sending rapid beacon");
        broadcast_beacon();
    }
}

// ============================================================================
// CHECK PARENT TIMEOUT
// Expires stale candidates (> PARENT_TIMEOUT_MS), clears current_parent_idx
// if parent is stale, and calls update_parent_selection().
// ============================================================================

inline void check_parent_timeout() {
    // Clean up all stale candidates (self-healing)
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (!candidates[i].valid) continue;
        uint32_t age = millis() - candidates[i].last_seen_ms;
        if (age > PARENT_TIMEOUT_MS) {
            Serial.print("PARENT | Expired stale candidate 0x");
            Serial.println(candidates[i].node_id, HEX);
            candidates[i].valid = false;
            // If this was our current parent, clear it
            if (i == current_parent_idx) {
                current_parent_idx = 0xFF;
                my_rank = RANK_SENSOR;  // reset rank
            }
        }
    }

    // If we lost our current parent, try to find a new one
    if (current_parent_idx == 0xFF) {
        update_parent_selection();
        if (current_parent_idx == 0xFF) {
            // Still no parent -- will keep waiting for beacons
            static uint32_t last_orphan_log = 0;
            if (millis() - last_orphan_log > 10000UL) {
                last_orphan_log = millis();
                Serial.println("PARENT | No parent available. Waiting for beacons...");
            }
        }
    }
}

// ============================================================================
// ON BEACON UPDATED (callback for mesh_common.h's process_beacon)
// ============================================================================

inline void on_beacon_updated() {
    update_parent_selection();
}

#endif // SENSOR_ROUTING_H
