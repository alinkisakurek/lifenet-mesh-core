#include "store_forward.h"
#include "debug.h"

static StoredMessage queue[QUEUE_MAX_MESSAGES];

void sf_init() {
    for (int i = 0; i < QUEUE_MAX_MESSAGES; ++i) {
        queue[i].in_use = false;
    }
}

bool sf_enqueue(const Packet& p, uint32_t now_ms) {
    int target_idx = -1;
    int oldest_idx = -1;
    uint32_t max_age = 0; // Used to track max age

    // Find a free slot, and simultaneously track the absolute oldest entry
    for (int i = 0; i < QUEUE_MAX_MESSAGES; ++i) {
        if (!queue[i].in_use) {
            target_idx = i;
            break; // Found a free slot, no need to evict
        } else {
            // Age is calculated using unsigned subtraction which naturally 
            // handles the millis() overflow safely for diffs < 49 days.
            uint32_t age = now_ms - queue[i].enqueue_time;
            if (oldest_idx == -1 || age > max_age) {
                max_age = age;
                oldest_idx = i;
            }
        }
    }

    if (target_idx == -1) {
        // Queue is completely full. We must drop the oldest entry (strict FIFO).
        if (oldest_idx != -1) {
            target_idx = oldest_idx;
        } else {
            // Should theoretically never happen if QUEUE_MAX_MESSAGES > 0
            return false;
        }
    }

    queue[target_idx].packet = p;
    queue[target_idx].enqueue_time = now_ms;
    queue[target_idx].retry_count = 0;
    queue[target_idx].in_use = true;

    return true; // We successfully queued it (even if we dropped an older one)
}

void sf_process(uint32_t now_ms, StoreForwardSendFn send_fn) {
    if (!send_fn) return;

    // To process strictly in FIFO order (oldest first), we sort the active indices.
    int active_indices[QUEUE_MAX_MESSAGES];
    int active_count = 0;

    for (int i = 0; i < QUEUE_MAX_MESSAGES; ++i) {
        if (queue[i].in_use) {
            active_indices[active_count++] = i;
        }
    }

    // Simple bubble sort to order active_indices by age (descending)
    for (int i = 0; i < active_count - 1; ++i) {
        for (int j = 0; j < active_count - i - 1; ++j) {
            uint32_t age_j = now_ms - queue[active_indices[j]].enqueue_time;
            uint32_t age_next = now_ms - queue[active_indices[j + 1]].enqueue_time;
            
            if (age_j < age_next) {
                // Next is older, swap so oldest moves to the front
                int temp = active_indices[j];
                active_indices[j] = active_indices[j + 1];
                active_indices[j + 1] = temp;
            }
        }
    }

    // Process from oldest to newest
    for (int k = 0; k < active_count; ++k) {
        int i = active_indices[k];
        if (!queue[i].in_use) continue; // Safety check

        // Check for absolute expiration
        if ((int32_t)(now_ms - queue[i].enqueue_time) >= QUEUE_MAX_AGE_MS) {
            queue[i].in_use = false;
            continue;
        }

        // Attempt forwarding via the callback
        if (send_fn(queue[i].packet)) {
            // Forwarding succeeded (e.g. got ACK)
            queue[i].in_use = false;
        } else {
            // Forwarding failed
            queue[i].retry_count++;
            if (queue[i].retry_count > QUEUE_MAX_RETRIES) {
                // Exceeded max retries, drop message
                queue[i].in_use = false;
            }
        }
    }
}

void sf_debug_dump(uint32_t now_ms) {
#if DEBUG_ENABLED
    DBG_PRINTLN("=== Store-and-Forward Queue ===");
    for (int i = 0; i < QUEUE_MAX_MESSAGES; ++i) {
        if (queue[i].in_use) {
            uint32_t age_ms = now_ms - queue[i].enqueue_time;
            DBG_PRINTF("idx: %d | dst: 0x%04X | msg_id: %u | retries: %d | age: %u ms\n",
                       i, queue[i].packet.dst_addr, queue[i].packet.msg_id, queue[i].retry_count, age_ms);
        }
    }
#endif
}
