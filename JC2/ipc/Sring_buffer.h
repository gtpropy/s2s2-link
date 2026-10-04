#ifndef __SRING_BUFFER_H
#define __SRING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * STM32WL55 SHARED SRAM2 RING BUFFERS
 * CPU1 = Cortex-M4 (NuttX OBC), CPU2 = Cortex-M0+ (Radio)
 *
 *   0x20008300: Ring TX  (M4 -> M0+)  beacon id + sheet-exact payload
 *   0x20008500: Ring RX  (M0+ -> M4)  ground commands
 *   0x20008900: Radio event log (M0+ -> M4)
 */

/* ------------------------------------------------------------
 * Ring TX: M4 -> M0+ (beacon id + sheet-exact payload per packet)
 *
 * beacon1_ipc_s / beacon2_ipc_s mirror Beacon1Packet_t/Beacon2Packet_t
 * (Mission/config.h) field-for-field, minus head/type/chk_crc (those
 * are added on TX by satellite_app.c's Beacon1_Pack/Beacon2_Pack). CPU1
 * fills every field it has a real sensor for; anything it doesn't
 * measure, it leaves 0 -- same "honest zero, not fabricated" rule the
 * rest of the beacon pipeline already follows. __attribute__((packed))
 * because this struct crosses the M4/M0+ shared-memory boundary raw --
 * no padding, no compiler-dependent layout.
 * ------------------------------------------------------------ */
#define IPC_RING_TX_BASE      0x20008300UL
#define RING_TX_MAGIC         0x54585242UL  /* "TXRB" */
#define RING_TX_DEPTH         4U            /* holds DEPTH-1 packets */

#define TELEM_ID_B1           1U
#define TELEM_ID_B2           2U

struct beacon1_ipc_s
{
  uint16_t tim_day;
  uint8_t  tim_hour;
  uint16_t bat_v;
  uint16_t bat_i;
  uint8_t  bat_temp;
  uint8_t  raw_v;
  uint8_t  raw_i;
  uint16_t sol_tot_v;
  uint8_t  sol_tot_i;
  uint8_t  unreg_i;
  uint8_t  main_3v3_i;
  uint8_t  msn_3v3_i;
  uint8_t  v5_i;
  uint8_t  adc_3v32_i;
  uint8_t  temp1;
  uint8_t  temp2;
  uint8_t  temp3;
  uint8_t  temp4;
  uint8_t  temp5;
  uint8_t  bpb_t;
  uint8_t  ant_p_t;
  uint8_t  obc_com_t;
  uint8_t  system_stat;
  uint8_t  mcu_stat;
  uint8_t  adc_stat;
} __attribute__((packed));

struct beacon2_ipc_s
{
  uint16_t tim_day;
  uint8_t  sol_p1_v;
  uint8_t  sol_p2_v;
  uint8_t  sol_p3_v;
  uint8_t  sol_p4_v;
  uint8_t  sol_p5_v;
  uint8_t  sol_p1_i;
  uint8_t  sol_p2_i;
  uint8_t  sol_p3_i;
  uint8_t  sol_p4_i;
  uint8_t  sol_p5_i;
  uint8_t  gyro_x;
  uint8_t  gyro_y;
  uint8_t  gyro_z;
  uint8_t  mag_x;
  uint8_t  mag_y;
  uint8_t  mag_z;
  uint8_t  power_stat;
} __attribute__((packed));

_Static_assert(sizeof(struct beacon1_ipc_s) == 29, "beacon1_ipc_s size");
_Static_assert(sizeof(struct beacon2_ipc_s) == 19, "beacon2_ipc_s size");

struct tx_packet_s
{
  union
  {
    struct beacon1_ipc_s b1;
    struct beacon2_ipc_s b2;
    uint8_t               raw[29];   /* sizeof(beacon1_ipc_s), the larger */
  } payload;
  uint16_t id;                        /* beacon id: 1 = B1, 2 = B2 */
  uint16_t seq;                       /* increments on every update call */
  uint16_t crc16;                     /* CRC-16 over all bytes before it */
} __attribute__((packed));

_Static_assert(sizeof(struct tx_packet_s) == 35, "tx_packet_s size");

struct shared_ring_tx
{
  volatile uint32_t magic;
  volatile uint32_t write_index;      /* written by M4 only */
  volatile uint32_t read_index;       /* written by M0+ only */
  struct tx_packet_s slots[RING_TX_DEPTH];
};

#define SHARED_RING_TX \
  ((volatile struct shared_ring_tx *)IPC_RING_TX_BASE)

/* ------------------------------------------------------------
 * Ring RX: M0+ -> M4 (ground commands)
 * ------------------------------------------------------------ */
#define IPC_RING_RX_BASE      0x20008500UL
#define RING_RX_MAGIC         0x52585242UL  /* "RXRB" */
#define RING_RX_DEPTH         4U
#define CMD_PAYLOAD_LEN       13U

#define CMD_TYPE_HK           0x01
#define CMD_TYPE_ADCS         0x03
#define CMD_TYPE_CAMERA       0x04
#define CMD_TYPE_EPDM         0x05

struct rx_command_s
{
  uint8_t  len;
  uint8_t  cmd[CMD_PAYLOAD_LEN];
  uint16_t crc16;
};

struct shared_ring_rx
{
  volatile uint32_t magic;
  volatile uint32_t write_index;      /* written by M0+ only */
  volatile uint32_t read_index;       /* written by M4 only */
  struct rx_command_s slots[RING_RX_DEPTH];
};

#define SHARED_RING_RX \
  ((volatile struct shared_ring_rx *)IPC_RING_RX_BASE)

/* ------------------------------------------------------------
 * IPCC doorbells
 * ------------------------------------------------------------ */
#ifndef IPCC_BASE
#  define IPCC_BASE           0x58000C00UL
#endif
#define IPCC_C1SCR_OFFSET     0x0008UL
#define IPCC_C1TOC2SR_OFFSET  0x000CUL
#define IPCC_C2SCR_OFFSET     0x0018UL
#define IPCC_C2TOC1SR_OFFSET  0x001CUL

#define IPCC_C1SCR     (*(volatile uint32_t *)(IPCC_BASE + IPCC_C1SCR_OFFSET))
#define IPCC_C1TOC2SR  (*(volatile uint32_t *)(IPCC_BASE + IPCC_C1TOC2SR_OFFSET))
#define IPCC_C2SCR     (*(volatile uint32_t *)(IPCC_BASE + IPCC_C2SCR_OFFSET))
#define IPCC_C2TOC1SR  (*(volatile uint32_t *)(IPCC_BASE + IPCC_C2TOC1SR_OFFSET))

#define IPCC_CH_HANDSHAKE     1U
#define IPCC_CH_BEACON        2U  /* M4 -> M0+ : TX ring notify */
#define IPCC_CH_COMMAND       3U  /* M0+ -> M4 : RX ring notify */

#define IPCC_RX_BIT(ch)       (1UL << ((ch) - 1U))
#define IPCC_TX_BIT(ch)       (1UL << (((ch) - 1U) + 16U))

static inline void ipcc_m4_send(uint32_t ch)
{
  IPCC_C1SCR = IPCC_TX_BIT(ch);
  __asm__ volatile ("dmb" ::: "memory");
}

static inline bool ipcc_m4_received(uint32_t ch)
{
  return (IPCC_C2TOC1SR & IPCC_RX_BIT(ch)) != 0U;
}

static inline void ipcc_m4_clear(uint32_t ch)
{
  IPCC_C1SCR = IPCC_RX_BIT(ch);
  __asm__ volatile ("dmb" ::: "memory");
}

static inline void ipcc_m0_send(uint32_t ch)
{
  IPCC_C2SCR = IPCC_TX_BIT(ch);
  __asm__ volatile ("dmb" ::: "memory");
}

static inline bool ipcc_m0_received(uint32_t ch)
{
  return (IPCC_C1TOC2SR & IPCC_RX_BIT(ch)) != 0U;
}

static inline void ipcc_m0_clear(uint32_t ch)
{
  IPCC_C2SCR = IPCC_RX_BIT(ch);
  __asm__ volatile ("dmb" ::: "memory");
}

/* ------------------------------------------------------------
 * Radio event log (M0+ -> M4)
 * ------------------------------------------------------------ */
#define IPC_RADIO_LOG_BASE    0x20008900UL
#define RADIO_LOG_MAGIC       0x524C4F47UL  /* "RLOG" */
#define RADIO_LOG_DEPTH       8U
#define RADIO_LOG_TEXT_MAX    88U

#define RADIO_EVT_CW          0x01
#define RADIO_EVT_B1          0x02
#define RADIO_EVT_B2          0x03
#define RADIO_EVT_RX_CMD      0x04
#define RADIO_EVT_TX_ACK      0x05
#define RADIO_EVT_TX_NACK     0x06
#define RADIO_EVT_INFO        0x07

struct radio_log_msg_s
{
  uint32_t timestamp_ms;
  uint8_t  event_type;
  char     text[RADIO_LOG_TEXT_MAX];
};

struct shared_radio_log_s
{
  volatile uint32_t magic;
  volatile uint32_t write_index;      /* written by M0+ only */
  volatile uint32_t read_index;       /* written by M4 only */
  struct radio_log_msg_s slots[RADIO_LOG_DEPTH];
};

#define SHARED_RADIO_LOG \
  ((volatile struct shared_radio_log_s *)IPC_RADIO_LOG_BASE)

/* ------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------ */
void rb_ipc_init(void);             /* Called by M4 at boot */

/* M4: queue one sheet-exact Beacon1/Beacon2 IPC payload into the TX ring,
 * notify M0+. Returns false if the ring is full or not initialized. */
bool live_telem_update_b1(const struct beacon1_ipc_s *b1);
bool live_telem_update_b2(const struct beacon2_ipc_s *b2);

/* M0+: single pass over the ring, filling whichever of b1_out/b2_out had
 * a real packet arrive (leaving the other untouched -- caller keeps its
 * last value, same "only overwrite on real data" rule as the rest of
 * the pipeline). got_b1 and got_b2 report which one(s) actually arrived.
 * Returns true if either one did. */
bool live_telem_drain_all(struct beacon1_ipc_s *b1_out, bool *got_b1,
                          struct beacon2_ipc_s *b2_out, bool *got_b2);

/* M0+: read one raw packet (false = empty or CRC error; slot is consumed) */
bool rb_tx_read(struct tx_packet_s *pkt);
bool rb_tx_empty(void);
bool rb_tx_full(void);

/* RX ring: commands, M0+ -> M4 */
bool rb_rx_write(const struct rx_command_s *cmd);
bool rb_rx_read(struct rx_command_s *cmd);
bool rb_rx_empty(void);
bool rb_rx_full(void);

/* Radio log */
void radio_log_init(void);
bool radio_log_write(uint8_t event_type, const char *msg);
bool radio_log_read(struct radio_log_msg_s *msg);
bool radio_log_empty(void);

#endif /* __SRING_BUFFER_H */