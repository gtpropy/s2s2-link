#include <string.h>

#include "tx_history.h"

void tx_history_init(tx_history_t *h)
{
    memset(h, 0, sizeof(*h));
}

void tx_history_store(tx_history_t *h, uint16_t seq_num, uint8_t type, uint8_t dst_id,
                       uint8_t flags, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len > TX_HISTORY_MAX_PAYLOAD)
    {
        return; /* too large to buffer -- see header comment */
    }

    tx_history_entry_t *e = &h->entries[h->next % TX_HISTORY_LEN];
    h->next++;

    e->used = true;
    e->seq_num = seq_num;
    e->type = type;
    e->dst_id = dst_id;
    e->flags = flags;
    e->payload_len = payload_len;
    memcpy(e->payload, payload, payload_len);
}

const tx_history_entry_t *tx_history_find(const tx_history_t *h, uint16_t seq_num)
{
    for (int i = 0; i < TX_HISTORY_LEN; i++)
    {
        if (h->entries[i].used && h->entries[i].seq_num == seq_num)
        {
            return &h->entries[i];
        }
    }
    return NULL;
}
