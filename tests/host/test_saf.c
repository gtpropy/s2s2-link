/* Host tests of the store-and-forward memory and of hop-limited relaying between several nodes. */
#include <stdio.h>
#include <string.h>
#include "digipeater.h"
#include "saf.h"
#include "tnc2.h"

static int fails, checks;
#define CHK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint16_t frame(const char *t, uint8_t *b) { return Digi_FromTnc2(t, b, DIGI_MAX_BODY); }

static SafConfig_t cfg(uint8_t maxf, uint32_t ivl_s, uint32_t ttl_s)
{
    SafConfig_t c = { maxf, ivl_s * 1000UL, ttl_s * 1000UL };
    return c;
}

/* a node: digipeater engine + store */
typedef struct { Digi_t d; Saf_t s; char name[8]; } Node;
static void node_init(Node *n, const char *call)
{
    DigiConfig_t c; Digi_DefaultConfig(&c, call, 1);
    c.rate_per_min = 0; c.src_min_interval_ms = 0;
    Digi_Init(&n->d, &c);
    SafConfig_t sc = cfg(3, 60, 3600); Saf_Init(&n->s, &sc);
    strcpy(n->name, call);
}
/* a node hears a frame: repeat decision, store the copy it would send */
static DigiResult_t hear(Node *n, const uint8_t *b, uint16_t l, uint32_t now, uint8_t *out, uint16_t *ol)
{
    DigiResult_t r = Digi_Process(&n->d, b, l, now, out, DIGI_MAX_BODY, ol);
    if (r == DIGI_REPEAT) Saf_Store(&n->s, out, *ol, Digi_HopsLeft(out, *ol), now, 1000);
    return r;
}
static void txt(const uint8_t *b, uint16_t l, char *o) { DigiFrame_t f; Digi_ParseFrame(b, l, &f); Digi_FormatFrame(&f, o, 200); }

int main(void)
{
    uint8_t a[DIGI_MAX_BODY], b[DIGI_MAX_BODY], o[DIGI_MAX_BODY];
    Saf_t s; SafConfig_t c = cfg(3, 60, 3600);

    /* ---- store, schedule, forward, max forwards ---- */
    Saf_Init(&s, &c);
    uint16_t la = frame("A1ABC>APRS,N1*,WIDE2-1:one", a), lb = frame("B2BBB>APRS,N1*:two", b);
    int i1 = Saf_Store(&s, a, la, 1, 1000, 0), i2 = Saf_Store(&s, b, lb, 0, 1000, 0);
    CHK(i1 >= 0 && i2 >= 0 && i1 != i2 && Saf_Count(&s) == 2);
    CHK(Saf_Store(&s, a, la, 1, 1100, 0) == -1 && s.stats.duplicates == 1);                         /* same frame again */
    CHK(Saf_Store(&s, a, 5, 1, 1100, 0) == -2);                                                       /* too short */
    CHK(Saf_NextDue(&s, 999) == -1);                                                                  /* not before the first send time */
    CHK(Saf_NextDue(&s, 1000) == i1);                                                                 /* more hops left goes first */
    Saf_Forwarded(&s, i1, 1000);
    CHK(Saf_NextDue(&s, 1001) == i2);                                                                 /* i1 waits for its interval */
    Saf_Forwarded(&s, i2, 1001);
    CHK(Saf_NextDue(&s, 30000) == -1);
    CHK(Saf_NextDue(&s, 61000) == i1);                                                                /* 60 s later: both due, hops first */
    Saf_Forwarded(&s, i1, 61000); Saf_Forwarded(&s, i2, 61001);
    Saf_Forwarded(&s, i1, 121000);                                                                    /* third and last send of i1 */
    CHK(Saf_Count(&s) == 1);                                                                          /* i1 is gone */
    uint16_t id; CHK(Saf_PopGone(&s, &id) && id == 1 && !Saf_PopGone(&s, &id));
    Saf_Forwarded(&s, i2, 121001);
    CHK(Saf_Count(&s) == 0 && s.stats.forwarded == 6);

    /* ---- TTL, millisecond wrap ---- */
    Saf_Init(&s, &c);
    i1 = Saf_Store(&s, a, la, 1, 0xFFFFF000UL, 0);
    CHK(Saf_NextDue(&s, 0xFFFFF800UL) == i1);                                                         /* due across the wrap */
    Saf_Expire(&s, 0xFFFFF800UL); CHK(Saf_Count(&s) == 1);
    Saf_Expire(&s, (uint32_t)(0xFFFFF000UL + 3600000UL - 1)); CHK(Saf_Count(&s) == 1);
    Saf_Expire(&s, (uint32_t)(0xFFFFF000UL + 3600000UL)); CHK(Saf_Count(&s) == 0 && s.stats.expired == 1);
    i1 = Saf_Store(&s, a, la, 1, 0, 0); CHK(Saf_NextDue(&s, 3600000UL) == -1);                        /* expired messages are never due */

    /* ---- full table: evicts the most-sent / oldest ---- */
    Saf_Init(&s, &c);
    for (int k = 0; k < (int)SAF_SLOTS; k++) {
        char t[64]; snprintf(t, sizeof t, "S%d>APRS,N1*:msg %d", k, k);
        uint8_t f[DIGI_MAX_BODY]; uint16_t l = frame(t, f);
        CHK(Saf_Store(&s, f, l, 0, 1000 + k, 0) >= 0);
    }
    CHK(Saf_Count(&s) == SAF_SLOTS);
    Saf_Forwarded(&s, 4, 5000);                                                                       /* slot 4 was sent once */
    { uint8_t f[DIGI_MAX_BODY]; uint16_t l = frame("NEW>APRS,N1*:newest", f);
      int k = Saf_Store(&s, f, l, 0, 6000, 0); CHK(k == 4 && s.stats.evicted == 1); }                 /* the one sent most often is replaced */
    { uint8_t f[DIGI_MAX_BODY]; uint16_t l = frame("NEW2>APRS,N1*:newer", f);
      int k = Saf_Store(&s, f, l, 0, 7000, 0); CHK(k == 0); }                                         /* then the oldest */

    /* ---- journal bookkeeping and restore ---- */
    Saf_Init(&s, &c);
    i1 = Saf_Store(&s, a, la, 1, 1000, 0);
    CHK(Saf_NextDirty(&s) == i1); Saf_ClearDirty(&s, i1); CHK(Saf_NextDirty(&s) == -1);
    Saf_Forwarded(&s, i1, 2000); CHK(Saf_NextDirty(&s) == i1);                                        /* a send changes the record */
    { Saf_t t; Saf_Init(&t, &c);
      CHK(Saf_Restore(&t, s.slot[i1].id, s.slot[i1].body, s.slot[i1].len, s.slot[i1].hops_left, s.slot[i1].fwd_count, Saf_TtlLeftMs(&s, i1, 3000), 500));
      CHK(t.slot[0].fwd_count == 1 && t.slot[0].id == s.slot[i1].id && Saf_TtlLeftMs(&t, 0, 500) == Saf_TtlLeftMs(&s, i1, 3000));
      CHK(!Saf_Restore(&t, s.slot[i1].id, a, la, 1, 0, 1000, 500));                                   /* same id twice */
      CHK(Saf_NextDirty(&t) == -1);                                                                   /* restored = already in the journal */
      CHK(Saf_NextDue(&t, 500) == -1 && Saf_NextDue(&t, 500 + 60000) == 0);                           /* first send one interval after the restart */
      uint8_t f[DIGI_MAX_BODY]; uint16_t l = frame("N>APRS,N1*:fresh", f); int k = Saf_Store(&t, f, l, 0, 600, 0);
      CHK(k >= 0 && t.slot[k].id > t.slot[0].id); }                                                   /* new ids continue after the restored ones */

    /* ---- hopping across nodes: WIDE2-2 gives two hops, a third node must not take it ---- */
    { Node n1, n2, n3; node_init(&n1, "NODE1"); node_init(&n2, "NODE2"); node_init(&n3, "NODE3");
      char t[200]; uint16_t ol;
      uint16_t l = frame("K1ABC>APRS,WIDE2-2:hop me", a);
      CHK(hear(&n1, a, l, 1000, o, &ol) == DIGI_REPEAT); txt(o, ol, t);
      CHK(!strcmp(t, "K1ABC>APRS,NODE1-1*,WIDE2-1:hop me"));                                          /* hop 1: trace, budget 2 -> 1 */
      CHK(Saf_Count(&n1.s) == 1 && n1.s.slot[0].hops_left == 1);                                      /* stored with the remaining budget */
      memcpy(b, o, ol); uint16_t lb2 = ol;
      CHK(hear(&n2, b, lb2, 2000, o, &ol) == DIGI_REPEAT); txt(o, ol, t);
      CHK(!strcmp(t, "K1ABC>APRS,NODE1-1,NODE2-1*:hop me"));                                          /* hop 2: budget used up */
      CHK(Saf_Count(&n2.s) == 1 && n2.s.slot[0].hops_left == 0);                                      /* still carried, but cannot hop further */
      memcpy(a, o, ol); uint16_t l3 = ol;
      CHK(hear(&n3, a, l3, 3000, o, &ol) == DIGI_DROP_NO_PATH && Saf_Count(&n3.s) == 0);              /* a third node has nothing to do */
      CHK(hear(&n1, a, l3, 4000, o, &ol) == DIGI_DROP_NO_PATH);                                       /* the first node hears the second copy: nothing */
      /* a node never takes back its own repeat, even when it travels with budget left */
      uint16_t l4 = frame("K1ABC>APRS,NODE1-1*,WIDE2-1:hop me", a);
      CHK(hear(&n1, a, l4, 5000, o, &ol) == DIGI_DROP_LOOP);
      /* the stored copy of node 1 is a frame node 3 can still hop later, in another place: it is sent again from the store */
      int due = Saf_NextDue(&n1.s, 1000 + 1000);
      CHK(due == 0);
      txt(n1.s.slot[due].body, n1.s.slot[due].len, t); CHK(!strcmp(t, "K1ABC>APRS,NODE1-1*,WIDE2-1:hop me"));
      CHK(hear(&n3, n1.s.slot[due].body, n1.s.slot[due].len, 9000, o, &ol) == DIGI_REPEAT);           /* node 3 picks it up on the later send */
      txt(o, ol, t); CHK(!strcmp(t, "K1ABC>APRS,NODE1-1,NODE3-1*:hop me")); }

    printf(fails ? "saf FAILED %d of %d checks\n" : "saf OK (%d checks)\n", fails ? fails : checks, checks);
    return fails != 0;
}
