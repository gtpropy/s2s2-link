/*
 * s2s2_auth.h -- Authentication for uplinked TYPE_TELECOMMAND traffic only.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
 *
 * Telemetry, beacons, ACKs, and digipeated traffic stay exactly as they
 * are -- unauthenticated, decodable by any AX.25/FX.25-aware station.
 * That's intentional (see DPM_ARCHITECTURE.md, README_GMSK_S2S2_ARCHITECTURE.md):
 * the whole point of using real AX.25/FX.25 is free interop with the
 * existing ground-station network. Interop for *commands that change
 * satellite state* was never the goal, and today nothing stops anyone
 * with compatible RF gear from forging one (opcode + node ID is all a
 * command needs) -- this file closes that gap.
 *
 * Mechanism: SipHash-2-4 (Aumasson/Bernstein), a keyed pseudorandom
 * function purpose-built for exactly this -- short, fast, high-quality
 * MACs over small messages, no big lookup tables, well suited to an M0+
 * with no hardware crypto accelerator wired up. Not a general-purpose
 * hash; do not reuse this key/function for anything else.
 *
 * Two things make a captured command unusable:
 *   1. The 8-byte MAC (wrong key -> can't forge it).
 *   2. A monotonic counter (right key, replayed old command -> rejected
 *      because its counter is not strictly greater than the last one
 *      this boot accepted).
 *
 * KNOWN LIMITATION -- read before relying on this: the replay counter
 * lives in RAM only (s2s2_auth_check_and_update_counter below). There is
 * no flash/EEPROM-backed persistence in this firmware yet (same honest
 * gap as CMD_GET_SAT_LOG's "not available on this build"). A satellite
 * reset zeroes the counter, so a command captured before a reset is
 * technically replayable again after one. Closing that fully needs a
 * real non-volatile counter slot -- not implemented here. This still
 * stops the common case (replay without a reset in between, and any
 * forged command without the key), which is the actual threat this was
 * built for.
 */
#ifndef S2S2_AUTH_H
#define S2S2_AUTH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2S2_AUTH_KEY_LEN 16   /* SipHash's 128-bit key */
#define S2S2_AUTH_MAC_LEN 8    /* SipHash-2-4's 64-bit output */

/* Defined in secrets.c, generated from secrets.h (gitignored -- see
   secrets.example.h for the template). Never commit the real key. */
extern const uint8_t s2s2_auth_key[S2S2_AUTH_KEY_LEN];

/* Raw SipHash-2-4 over one buffer, keyed with s2s2_auth_key. Exposed
   mainly for host-side testing; normal callers should use
   s2s2_auth_compute_mac below instead. */
uint64_t siphash24(const uint8_t key[S2S2_AUTH_KEY_LEN], const uint8_t *data, size_t len);

/* Computes the MAC for one command. Binds to src_id/dst_id (so a MAC
   computed for one link direction/session can't be replayed as if it
   came from somewhere else) plus every unsigned command byte -- opcode,
   subsystem, data_id, address, num_packets, counter, in that wire order
   (see s2s2_commands.h's cubus_command_t / COMMAND_PAYLOAD_LEN). */
void s2s2_auth_compute_mac(uint8_t src_id, uint8_t dst_id,
                            const uint8_t *signed_bytes, uint16_t signed_len,
                            uint8_t mac_out[S2S2_AUTH_MAC_LEN]);

/* Ground-station side: next counter value to stamp on an outgoing
   command. Starts at 1 each boot and only increases -- never reuse a
   value, even across a GS restart (see the RAM-only limitation above:
   restarting the GS mid-session is fine since the satellite's floor only
   moves forward, but restarting the GS and reusing counter 1 would
   collide with a value it may already have accepted). */
uint32_t s2s2_auth_next_counter(void);

/* Satellite side: replay guard. Returns true (and advances the internal
   floor to `counter`) if counter is strictly greater than the last
   accepted value this boot; returns false (reject, floor unchanged)
   otherwise. Call this AFTER s2s2_auth_compute_mac has confirmed the MAC
   is valid -- never advance the floor on an unauthenticated counter
   value, or an attacker without the key could burn through counters to
   deny future legitimate commands. */
bool s2s2_auth_check_and_update_counter(uint32_t counter);

#ifdef __cplusplus
}
#endif

#endif /* S2S2_AUTH_H */
