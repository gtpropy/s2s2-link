#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "Sring_buffer.h"

static inline void rb_memory_barrier(void)
{
  __asm__ volatile ("dmb" ::: "memory");
}

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) */
static uint16_t rb_calc_crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFF;

  for (uint16_t i = 0; i < len; i++)
    {
      crc ^= (uint16_t)data[i] << 8;
      for (uint8_t bit = 0; bit < 8; bit++)
        {
          crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                               : (uint16_t)(crc << 1);
        }
    }

  return crc;
}

/* ============================================================
 * Init (called once by M4 at boot)
 * ============================================================ */
void rb_ipc_init(void)
{
  if (SHARED_RING_TX->magic == RING_TX_MAGIC &&
      SHARED_RING_RX->magic == RING_RX_MAGIC &&
      SHARED_RADIO_LOG->magic == RADIO_LOG_MAGIC)
    {
      return;
    }

  /* TX ring */
  SHARED_RING_TX->magic = 0;
  rb_memory_barrier();
  SHARED_RING_TX->write_index = 0;
  SHARED_RING_TX->read_index  = 0;
  memset((void *)SHARED_RING_TX->slots, 0, sizeof(SHARED_RING_TX->slots));
  rb_memory_barrier();
  SHARED_RING_TX->magic = RING_TX_MAGIC;

  /* RX ring */
  SHARED_RING_RX->magic = 0;
  rb_memory_barrier();
  SHARED_RING_RX->write_index = 0;
  SHARED_RING_RX->read_index  = 0;
  memset((void *)SHARED_RING_RX->slots, 0, sizeof(SHARED_RING_RX->slots));
  rb_memory_barrier();
  SHARED_RING_RX->magic = RING_RX_MAGIC;

  /* Radio log */
  radio_log_init();

  /* Flash data ring (Ringbuffer 3: M4 -> M0+) */
  SHARED_FLASH_DATA->magic = 0;
  rb_memory_barrier();
  SHARED_FLASH_DATA->write_index = 0;
  SHARED_FLASH_DATA->read_index  = 0;
  memset((void *)SHARED_FLASH_DATA->slots, 0, sizeof(SHARED_FLASH_DATA->slots));
  rb_memory_barrier();
  SHARED_FLASH_DATA->magic = FLASH_DATA_MAGIC;

  rb_memory_barrier();
}

/* ============================================================
 * TX ring: beacon id + 5 scaled integers (M4 -> M0+)
 * Empty: read == write.  Full: (write + 1) % DEPTH == read.
 * ============================================================ */
bool rb_tx_empty(void)
{
  if (SHARED_RING_TX->magic != RING_TX_MAGIC)
    {
      return true;
    }

  return SHARED_RING_TX->read_index == SHARED_RING_TX->write_index;
}

bool rb_tx_full(void)
{
  if (SHARED_RING_TX->magic != RING_TX_MAGIC)
    {
      return false;
    }

  return ((SHARED_RING_TX->write_index + 1U) % RING_TX_DEPTH) ==
         SHARED_RING_TX->read_index;
}

/* M4 side. seq increments on EVERY call (even if the ring is full),
 * so gaps in seq on the M0+ side reveal dropped packets. */
bool live_telem_update_float(uint16_t beacon_id,
                             int16_t d1, int16_t d2, int16_t d3,
                             int16_t d4, int16_t d5)
{
  static uint16_t s_seq = 0;
  uint16_t seq = s_seq++;

  if (SHARED_RING_TX->magic != RING_TX_MAGIC || rb_tx_full())
    {
      return false;
    }

  struct tx_packet_s p;
  p.data[0] = d1;
  p.data[1] = d2;
  p.data[2] = d3;
  p.data[3] = d4;
  p.data[4] = d5;
  p.id      = beacon_id;
  p.seq     = seq;
  p.crc16   = rb_calc_crc16((const uint8_t *)&p,
                            offsetof(struct tx_packet_s, crc16));

  uint32_t idx = SHARED_RING_TX->write_index % RING_TX_DEPTH;
  memcpy((void *)&SHARED_RING_TX->slots[idx], &p, sizeof(p));

  rb_memory_barrier();

  SHARED_RING_TX->write_index = (idx + 1U) % RING_TX_DEPTH;

  rb_memory_barrier();

  ipcc_m4_send(IPCC_CH_BEACON);   /* notify M0+ */
  return true;
}

/* M0+ side. Reads one slot and always consumes it.
 * Returns false if empty or CRC error. */
bool rb_tx_read(struct tx_packet_s *pkt)
{
  if (!pkt || rb_tx_empty())
    {
      return false;
    }

  uint32_t idx = SHARED_RING_TX->read_index % RING_TX_DEPTH;
  memcpy(pkt, (const void *)&SHARED_RING_TX->slots[idx], sizeof(*pkt));

  rb_memory_barrier();

  SHARED_RING_TX->read_index = (idx + 1U) % RING_TX_DEPTH;

  rb_memory_barrier();

  return rb_calc_crc16((const uint8_t *)pkt,
                       offsetof(struct tx_packet_s, crc16)) == pkt->crc16;
}

/* M0+ side. Drain everything, keep the newest valid packet. */
bool live_telem_get_float(uint16_t *id,
                          int16_t *d1, int16_t *d2, int16_t *d3,
                          int16_t *d4, int16_t *d5)
{
  struct tx_packet_s pkt;
  struct tx_packet_s last;
  bool got = false;

  if (ipcc_m0_received(IPCC_CH_BEACON))
    {
      ipcc_m0_clear(IPCC_CH_BEACON);
    }

  while (!rb_tx_empty())
    {
      if (rb_tx_read(&pkt))
        {
          last = pkt;
          got  = true;
        }
    }

  if (!got)
    {
      if (id) { *id = 0; }
      return false;
    }

  if (id) { *id = last.id; }
  if (d1) { *d1 = last.data[0]; }
  if (d2) { *d2 = last.data[1]; }
  if (d3) { *d3 = last.data[2]; }
  if (d4) { *d4 = last.data[3]; }
  if (d5) { *d5 = last.data[4]; }

  return true;
}

/* ============================================================
 * RX ring: commands (M0+ -> M4)
 * ============================================================ */
bool rb_rx_empty(void)
{
  if (SHARED_RING_RX->magic != RING_RX_MAGIC)
    {
      return true;
    }

  return SHARED_RING_RX->read_index == SHARED_RING_RX->write_index;
}

bool rb_rx_full(void)
{
  if (SHARED_RING_RX->magic != RING_RX_MAGIC)
    {
      return false;
    }

  return ((SHARED_RING_RX->write_index + 1U) % RING_RX_DEPTH) ==
         SHARED_RING_RX->read_index;
}

bool rb_rx_write(const struct rx_command_s *cmd)
{
  if (!cmd || SHARED_RING_RX->magic != RING_RX_MAGIC || rb_rx_full())
    {
      return false;
    }

  uint32_t idx = SHARED_RING_RX->write_index % RING_RX_DEPTH;
  volatile struct rx_command_s *slot = &SHARED_RING_RX->slots[idx];

  slot->len = CMD_PAYLOAD_LEN;
  memcpy((void *)slot->cmd, cmd->cmd, CMD_PAYLOAD_LEN);
  slot->crc16 = rb_calc_crc16(cmd->cmd, CMD_PAYLOAD_LEN);

  rb_memory_barrier();

  SHARED_RING_RX->write_index = (idx + 1U) % RING_RX_DEPTH;

  rb_memory_barrier();
  return true;
}

bool rb_rx_read(struct rx_command_s *cmd)
{
  if (!cmd || rb_rx_empty())
    {
      return false;
    }

  uint32_t idx = SHARED_RING_RX->read_index % RING_RX_DEPTH;
  volatile struct rx_command_s *slot = &SHARED_RING_RX->slots[idx];

  cmd->len = CMD_PAYLOAD_LEN;
  memcpy(cmd->cmd, (const void *)slot->cmd, CMD_PAYLOAD_LEN);
  cmd->crc16 = slot->crc16;

  rb_memory_barrier();

  SHARED_RING_RX->read_index = (idx + 1U) % RING_RX_DEPTH;

  rb_memory_barrier();

  return rb_calc_crc16(cmd->cmd, CMD_PAYLOAD_LEN) == cmd->crc16;
}

/* ============================================================
 * Radio event log (M0+ -> M4)
 * Drops the newest message when full (keeps SPSC ownership clean).
 * ============================================================ */
void radio_log_init(void)
{
  SHARED_RADIO_LOG->magic = 0;
  rb_memory_barrier();

  SHARED_RADIO_LOG->write_index = 0;
  SHARED_RADIO_LOG->read_index  = 0;
  memset((void *)SHARED_RADIO_LOG->slots, 0, sizeof(SHARED_RADIO_LOG->slots));
  rb_memory_barrier();

  SHARED_RADIO_LOG->magic = RADIO_LOG_MAGIC;
  rb_memory_barrier();
}

bool radio_log_empty(void)
{
  if (SHARED_RADIO_LOG->magic != RADIO_LOG_MAGIC)
    {
      return true;
    }

  return SHARED_RADIO_LOG->read_index == SHARED_RADIO_LOG->write_index;
}

bool radio_log_write(uint8_t event_type, const char *msg)
{
  if (!msg || SHARED_RADIO_LOG->magic != RADIO_LOG_MAGIC)
    {
      return false;
    }

  uint32_t idx = SHARED_RADIO_LOG->write_index % RADIO_LOG_DEPTH;

  if (((idx + 1U) % RADIO_LOG_DEPTH) == SHARED_RADIO_LOG->read_index)
    {
      return false; /* full */
    }

  volatile struct radio_log_msg_s *slot = &SHARED_RADIO_LOG->slots[idx];

  slot->event_type   = event_type;
  slot->timestamp_ms = 0;
  strncpy((char *)slot->text, msg, RADIO_LOG_TEXT_MAX - 1);
  slot->text[RADIO_LOG_TEXT_MAX - 1] = '\0';

  rb_memory_barrier();

  SHARED_RADIO_LOG->write_index = (idx + 1U) % RADIO_LOG_DEPTH;

  rb_memory_barrier();
  return true;
}

bool radio_log_read(struct radio_log_msg_s *msg)
{
  if (!msg || radio_log_empty())
    {
      return false;
    }

  uint32_t idx = SHARED_RADIO_LOG->read_index % RADIO_LOG_DEPTH;
  volatile struct radio_log_msg_s *slot = &SHARED_RADIO_LOG->slots[idx];

  msg->event_type   = slot->event_type;
  msg->timestamp_ms = slot->timestamp_ms;
  strncpy(msg->text, (const char *)slot->text, RADIO_LOG_TEXT_MAX - 1);
  msg->text[RADIO_LOG_TEXT_MAX - 1] = '\0';

  rb_memory_barrier();

  SHARED_RADIO_LOG->read_index = (idx + 1U) % RADIO_LOG_DEPTH;

  rb_memory_barrier();
  return true;
}