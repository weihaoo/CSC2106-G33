// ============================================================================
// RELAY ROUTING — DAG Parent Set Management
// CSC2106 Group 33 — LoRa Mesh Network
//
// Relay-specific parent management:
//   - ParentSetEntry struct and parent_set[] array
//   - update_parent_set() — score candidates, build active set, compute rank
//   - select_parent_for_packet() — weighted random selection
//   - is_stale() / check_parent_staleness() — self-healing
//   - on_beacon_updated() — callback for mesh_common.h
//
// All functions are inline. Include after shared headers and global definitions.
// ============================================================================

#ifndef RELAY_ROUTING_H
#define RELAY_ROUTING_H

#include <Arduino.h>

// ============================================================================
// PARENT SET ENTRY (active parents selected from candidates)
// ============================================================================

struct ParentSetEntry {
    uint8_t  node_id;
    int      score;
    uint32_t last_seen_ms;
    bool     active;
};

// ============================================================================
// EXTERN DECLARATIONS
// Defined in relay_node.ino
// ============================================================================

extern ParentSetEntry parent_set[];
extern uint8_t        active_parent_count;
extern ParentInfo     candidates[];
extern uint8_t        my_rank;

// Forward declarations for functions defined elsewhere
extern void broadcast_beacon();

// ============================================================================
// IS STALE
// ============================================================================

inline bool is_stale(uint32_t last_seen) {
    return (millis() - last_seen) > PARENT_TIMEOUT_MS;
}

// ============================================================================
// UPDATE PARENT SET (DAG)
// Rebuilds the active parent set from candidates.
// Invalidates stale candidates so their slots can be reused.
// Sends a rapid beacon if the parent count changed (self-organizing).
// ============================================================================

inline void update_parent_set() {
    uint8_t old_parent_count = active_parent_count;
    active_parent_count = 0;

    // Invalidate stale candidates so their slots can be reused
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (candidates[i].valid && is_stale(candidates[i].last_seen_ms)) {
            char msg[64];
            sprintf(msg, "Expired stale candidate %s", node_name(candidates[i].node_id));
            LOG_PARENT(msg);
            candidates[i].valid = false;
        }
    }

    // Count valid candidates for debug
    int valid_count = 0;
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (candidates[i].valid) valid_count++;
    }

    if (valid_count > 0) {
        Serial.println("─────────────────────────────────────────────");
        Serial.print("EVAL  | Scoring ");
        Serial.print(valid_count);
        Serial.print(" candidate(s) (threshold=");
        Serial.print(PARENT_THRESHOLD_SCORE);
        Serial.println("):");
    }

    // Score valid candidates and build active parent set
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (!candidates[i].valid) continue;

        // Pass debug=true and node_id to show score breakdown
        int score = score_parent(candidates[i].rank,
                                  candidates[i].rssi,
                                  candidates[i].queue_pct,
                                  candidates[i].parent_health,
                                  true,
                                  candidates[i].node_id);

        if (score >= PARENT_THRESHOLD_SCORE) {
            if (active_parent_count < MAX_ACTIVE_PARENTS) {
                parent_set[active_parent_count].node_id = candidates[i].node_id;
                parent_set[active_parent_count].score = score;
                parent_set[active_parent_count].last_seen_ms = candidates[i].last_seen_ms;
                parent_set[active_parent_count].active = true;
                active_parent_count++;
            }
        }
    }

    // Clear unused parent_set entries (prevent stale active flags)
    for (int i = active_parent_count; i < MAX_ACTIVE_PARENTS; i++) {
        parent_set[i].active = false;
    }

    // Update my_rank based on best ACTIVE parent only (not all candidates —
    // some may have scored below threshold but haven't timed out yet)
    if (active_parent_count > 0) {
        uint8_t best_rank = 255;
        for (int i = 0; i < active_parent_count; i++) {
            for (int j = 0; j < MAX_CANDIDATES; j++) {
                if (candidates[j].valid && candidates[j].node_id == parent_set[i].node_id) {
                    if (candidates[j].rank < best_rank) {
                        best_rank = candidates[j].rank;
                    }
                }
            }
        }
        if (best_rank != 255) my_rank = best_rank + 1;
    } else {
        my_rank = RANK_RELAY;  // Reset to default if no active parents
    }

    // Log parent set
    char msg[80];
    sprintf(msg, "Parent set updated: %d active parents (rank=%d)", active_parent_count, my_rank);
    LOG_PARENT(msg);

    for (int i = 0; i < active_parent_count; i++) {
        sprintf(msg, "  [%d] %s (score: %d)",
                i+1, node_name(parent_set[i].node_id), parent_set[i].score);
        log_detail(msg);
    }

    if (valid_count > 0) {
        Serial.println("─────────────────────────────────────────────");
    }

    // Rapid beacon on topology change — tell downstream nodes immediately
    if (active_parent_count != old_parent_count) {
        sprintf(msg, "Topology changed: %d→%d parents, sending rapid beacon",
                old_parent_count, active_parent_count);
        LOG_PARENT(msg);
        broadcast_beacon();
    }
}

// ============================================================================
// SELECT PARENT FOR PACKET (WEIGHTED RANDOM)
// ============================================================================

inline uint8_t select_parent_for_packet() {
    if (active_parent_count == 0) return 0xFF;
    if (active_parent_count == 1) return parent_set[0].node_id;

    // Weighted random selection based on score
    int total_score = 0;
    for (int i = 0; i < active_parent_count; i++) {
        total_score += parent_set[i].score;
    }

    if (total_score == 0) return parent_set[0].node_id;

    int r = random(0, total_score);
    int cumulative = 0;
    for (int i = 0; i < active_parent_count; i++) {
        cumulative += parent_set[i].score;
        if (r < cumulative) {
            return parent_set[i].node_id;
        }
    }

    return parent_set[active_parent_count - 1].node_id;
}

// ============================================================================
// CHECK PARENT STALENESS (Self-Healing)
// Called periodically from loop() to detect dead parents.
// If all parents are lost, resets rank and logs for visibility.
// ============================================================================

inline void check_parent_staleness() {
    bool had_parents = (active_parent_count > 0);

    // Rebuild parent set (will invalidate stale candidates)
    update_parent_set();

    // If we just lost all parents, log it prominently
    if (had_parents && active_parent_count == 0) {
        LOG_WARN("All parents lost! Waiting for beacons...");
        my_rank = RANK_RELAY;  // Reset rank
    }
}

// ============================================================================
// ON BEACON UPDATED (callback for mesh_common.h)
// ============================================================================

inline void on_beacon_updated() {
    update_parent_set();
}

#endif // RELAY_ROUTING_H
