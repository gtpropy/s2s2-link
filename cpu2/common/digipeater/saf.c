#include "saf.h"

#include <string.h>

static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }     /* safe across the 32-bit ms wrap */

static void gone(Saf_t *s, uint16_t id)
{
    if (s->nGone < sizeof(s->gone) / sizeof(s->gone[0])) s->gone[s->nGone++] = id;
}

static void drop(Saf_t *s, int i)
{
    gone(s, s->slot[i].id);
    s->slot[i].used = false;
    s->slot[i].dirty = false;
}

void Saf_Init(Saf_t *s, const SafConfig_t *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    if (s->cfg.max_forwards == 0) s->cfg.max_forwards = 1;
    s->nextId = 1;
}

uint8_t Saf_Count(const Saf_t *s)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < SAF_SLOTS; i++) if (s->slot[i].used) n++;
    return n;
}

static int free_slot(Saf_t *s)
{
    for (uint8_t i = 0; i < SAF_SLOTS; i++) if (!s->slot[i].used) return i;
    /* full: evict the message that has been sent most often, then the oldest */
    int v = 0;
    for (uint8_t i = 1; i < SAF_SLOTS; i++) {
        const SafSlot_t *a = &s->slot[i], *b = &s->slot[v];
        if (a->fwd_count > b->fwd_count || (a->fwd_count == b->fwd_count && (int32_t)(b->t_store_ms - a->t_store_ms) > 0)) v = i;
    }
    drop(s, v);
    s->stats.evicted++;
    return v;
}

int Saf_Store(Saf_t *s, const uint8_t *body, uint16_t len, uint8_t hops_left, uint32_t now_ms, uint32_t first_delay_ms)
{
    if (body == NULL || len < 16U || len > DIGI_MAX_BODY) return -2;
    for (uint8_t i = 0; i < SAF_SLOTS; i++)
        if (s->slot[i].used && s->slot[i].len == len && memcmp(s->slot[i].body, body, len) == 0) { s->stats.duplicates++; return -1; }
    int i = free_slot(s);
    SafSlot_t *m = &s->slot[i];
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->dirty = true;
    m->id = s->nextId++;
    if (s->nextId == 0) s->nextId = 1;
    m->len = (uint8_t)len;
    m->hops_left = hops_left;
    m->t_store_ms = now_ms;
    m->expire_ms = now_ms + s->cfg.ttl_ms;
    m->next_fwd_ms = now_ms + first_delay_ms;
    memcpy(m->body, body, len);
    s->stats.stored++;
    return i;
}

int Saf_NextDue(Saf_t *s, uint32_t now_ms)
{
    int best = -1;
    for (uint8_t i = 0; i < SAF_SLOTS; i++) {
        const SafSlot_t *m = &s->slot[i];
        if (!m->used || !reached(now_ms, m->next_fwd_ms) || reached(now_ms, m->expire_ms)) continue;
        if (best < 0) { best = i; continue; }
        const SafSlot_t *b = &s->slot[best];
        bool better = (m->hops_left > b->hops_left) ||
                      (m->hops_left == b->hops_left && m->fwd_count < b->fwd_count) ||
                      (m->hops_left == b->hops_left && m->fwd_count == b->fwd_count && (int32_t)(b->t_store_ms - m->t_store_ms) > 0);
        if (better) best = i;
    }
    return best;
}

void Saf_Forwarded(Saf_t *s, int i, uint32_t now_ms)
{
    if (i < 0 || i >= (int)SAF_SLOTS || !s->slot[i].used) return;
    SafSlot_t *m = &s->slot[i];
    m->fwd_count++;
    s->stats.forwarded++;
    if (m->fwd_count >= s->cfg.max_forwards) { drop(s, i); return; }
    m->next_fwd_ms = now_ms + s->cfg.forward_interval_ms;
    m->dirty = true;
}

void Saf_Expire(Saf_t *s, uint32_t now_ms)
{
    for (uint8_t i = 0; i < SAF_SLOTS; i++)
        if (s->slot[i].used && reached(now_ms, s->slot[i].expire_ms)) { drop(s, i); s->stats.expired++; }
}

bool Saf_Restore(Saf_t *s, uint16_t id, const uint8_t *body, uint16_t len, uint8_t hops_left, uint8_t fwd_count,
                 uint32_t ttl_left_ms, uint32_t now_ms)
{
    if (body == NULL || len < 16U || len > DIGI_MAX_BODY || id == 0) return false;
    for (uint8_t i = 0; i < SAF_SLOTS; i++) if (s->slot[i].used && s->slot[i].id == id) return false;   /* already there */
    int k = -1;
    for (uint8_t i = 0; i < SAF_SLOTS; i++) if (!s->slot[i].used) { k = i; break; }
    if (k < 0) return false;
    SafSlot_t *m = &s->slot[k];
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->id = id;
    m->len = (uint8_t)len;
    m->hops_left = hops_left;
    m->fwd_count = fwd_count;
    m->t_store_ms = now_ms;
    m->expire_ms = now_ms + ttl_left_ms;
    m->next_fwd_ms = now_ms + s->cfg.forward_interval_ms;
    memcpy(m->body, body, len);
    if (id >= s->nextId) s->nextId = (uint16_t)(id + 1U);
    if (s->nextId == 0) s->nextId = 1;
    s->stats.restored++;
    return true;
}

int Saf_NextDirty(const Saf_t *s)
{
    for (uint8_t i = 0; i < SAF_SLOTS; i++) if (s->slot[i].used && s->slot[i].dirty) return i;
    return -1;
}

void Saf_ClearDirty(Saf_t *s, int i) { if (i >= 0 && i < (int)SAF_SLOTS) s->slot[i].dirty = false; }

bool Saf_PopGone(Saf_t *s, uint16_t *id)
{
    if (s->nGone == 0) return false;
    *id = s->gone[0];
    memmove(&s->gone[0], &s->gone[1], (size_t)(s->nGone - 1U) * sizeof(s->gone[0]));
    s->nGone--;
    return true;
}

uint32_t Saf_TtlLeftMs(const Saf_t *s, int i, uint32_t now_ms)
{
    if (i < 0 || i >= (int)SAF_SLOTS || !s->slot[i].used) return 0;
    int32_t d = (int32_t)(s->slot[i].expire_ms - now_ms);
    return d > 0 ? (uint32_t)d : 0;
}
