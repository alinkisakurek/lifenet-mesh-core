#include "store_forward.h"
#include "debug.h"

static StoredMessage queue[QUEUE_MAX_MESSAGES];

void sf_init() {
    for (int i = 0; i < QUEUE_MAX_MESSAGES; ++i) {
        queue[i].in_use = false;
    }
}

bool sf_enqueue(const Packet& p, uint32_t now_ms) {
#if DEBUG_ENABLED
    DBG_PRINTF("[SF] enqueue request dst=0x%04X msg_id=%08X at %u ms\n",
               p.dst_addr, (unsigned int)p.msg_id, now_ms);
#endif

    int target_idx = -1;
    int oldest_idx = -1;
    uint32_t max_age = 0; 

    for (int i = 0; i < QUEUE_MAX_MESSAGES; ++i) {
        if (!queue[i].in_use) {
            target_idx = i;
            break; 
        } else {
            uint32_t age = now_ms - queue[i].enqueue_time;
            if (oldest_idx == -1 || age > max_age) {
                max_age   = age;
                oldest_idx = i;
            }
        }
    }

    if (target_idx == -1) {
        if (oldest_idx != -1) {
#if DEBUG_ENABLED
            DBG_PRINTF("[SF] queue full, evicting oldest idx=%d (age=%u ms)\n",
                       oldest_idx, max_age);
#endif
            target_idx = oldest_idx;
        } else {
#if DEBUG_ENABLED
            DBG_PRINTLN("[SF] enqueue failed: no slot and no oldest_idx (BUG)");
#endif
            return false;
        }
    }

#if DEBUG_ENABLED
    DBG_PRINTF("[SF] storing at idx=%d\n", target_idx);
#endif

    queue[target_idx].packet       = p;
    queue[target_idx].enqueue_time = now_ms;
    queue[target_idx].retry_count  = 0;
    queue[target_idx].in_use       = true;
    queue[target_idx].last_attempt_time = 0; // Ilk eklendiginde sifirla

    return true; 
}

void sf_process(uint32_t now_ms, StoreForwardSendFn send_fn) {
    if (!send_fn) return;

    int active_indices[QUEUE_MAX_MESSAGES];
    int active_count = 0;

    for (int i = 0; i < QUEUE_MAX_MESSAGES; ++i) {
        if (queue[i].in_use) {
            active_indices[active_count++] = i;
        }
    }

    for (int i = 0; i < active_count - 1; ++i) {
        for (int j = 0; j < active_count - i - 1; ++j) {
            uint32_t age_j    = now_ms - queue[active_indices[j]].enqueue_time;
            uint32_t age_next = now_ms - queue[active_indices[j + 1]].enqueue_time;

            if (age_j < age_next) {
                int temp              = active_indices[j];
                active_indices[j]     = active_indices[j + 1];
                active_indices[j + 1] = temp;
            }
        }
    }

    // Sirasiyla isle (En eskiden en yeniye)
    for (int k = 0; k < active_count; ++k) {
        int i = active_indices[k];
        if (!queue[i].in_use) continue; 

        // Suresi tamamen dolmus mu kontrol et (Orn: 5 dk)
        if ((int32_t)(now_ms - queue[i].enqueue_time) >= QUEUE_MAX_AGE_MS) {
#if DEBUG_ENABLED
            DBG_PRINTF("[SF] dropping idx=%d due to age >= %u ms\n",
                       i, QUEUE_MAX_AGE_MS);
#endif
            queue[i].in_use = false;
            continue;
        }

        // --- HIZLI DENEME (RAPID-FIRE) ENGELLEYICI ---
        if (queue[i].last_attempt_time != 0 && (now_ms - queue[i].last_attempt_time < 5000)) {
            continue; // Son denemeden bu yana 5 saniye gecmediyse, bu mesaji pas gec
        }
        queue[i].last_attempt_time = now_ms; // Deneme zamanini guncelle
        // ---------------------------------------------

#if DEBUG_ENABLED
        DBG_PRINTF("[SF] retry idx=%d dst=0x%04X msg_id=%08X retries=%u\n",
                   i,
                   queue[i].packet.dst_addr,
                   (unsigned int)queue[i].packet.msg_id,
                   (unsigned int)queue[i].retry_count);
#endif

        // Ağa gondermeyi dene (Sadece bir kere calisacak)
        if (send_fn(queue[i].packet)) {
#if DEBUG_ENABLED
            DBG_PRINTF("[SF] success, removing idx=%d\n", i);
#endif
            queue[i].in_use = false;
        } else {
            // Basarisiz oldu, deneme sayisini artir
            queue[i].retry_count++;
            if (queue[i].retry_count > QUEUE_MAX_RETRIES) {
#if DEBUG_ENABLED
                DBG_PRINTF("[SF] dropping idx=%d after %u retries\n",
                           i, (unsigned int)queue[i].retry_count);
#endif
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
            DBG_PRINTF("idx: %d | dst: 0x%04X | msg_id: %08X | retries: %d | age: %u ms\n",
                       i,
                       queue[i].packet.dst_addr,
                       (unsigned int)queue[i].packet.msg_id,
                       queue[i].retry_count,
                       age_ms);
        }
    }
#endif
}