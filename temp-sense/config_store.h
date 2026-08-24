#ifndef TEMP_SENSE_CONFIG_STORE_H
#define TEMP_SENSE_CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Operator-set policy for the v1.2 UDP protocol's receiver-side retry loop,
 * persisted in config.dat (same magic/version/CRC32 pattern as sd_ring.c's
 * meta.dat, but its own file — ring-watermark bookkeeping and this are
 * unrelated concerns). The device only stores these; it never times
 * anything out itself (the logger stays stateless-per-poll, answering
 * whatever it last received — see the migration plan). A receiver reads
 * them via `config get` at session start and drives its own retry loop.
 */

#define CONFIG_DEFAULT_MAX_RETRIES       5u
#define CONFIG_DEFAULT_RETRY_INTERVAL_MS 5000u

// Reasonable bounds for `config set`, not enforced by the wire format —
// just sanity limits against a typo locking in a useless policy.
#define CONFIG_MIN_MAX_RETRIES       1u
#define CONFIG_MAX_MAX_RETRIES       255u
#define CONFIG_MIN_RETRY_INTERVAL_MS 100u
#define CONFIG_MAX_RETRY_INTERVAL_MS 600000u  // 10 minutes

/*! \brief Load config.dat if present and valid; seed in-memory defaults
 *  (CONFIG_DEFAULT_*) otherwise, without writing them to disk — the file is
 *  only ever created by an explicit config_store_set(). Call once, after
 *  sd_ring_init() succeeds (this needs the same mounted filesystem).
 *
 * \return true if a valid config.dat was loaded, false if defaults were
 *         seeded instead (not an error — an unconfigured device is a valid
 *         and common state).
 */
bool config_store_init(void);

uint32_t config_store_max_retries(void);
uint32_t config_store_retry_interval_ms(void);

/*! \brief Validate against the CONFIG_MIN/MAX_* bounds, and if valid,
 *  update the in-memory values and persist immediately (config changes are
 *  a rare operator action, not a per-sample write, so there is no wear
 *  concern the way there is for meta.dat's lazy persistence).
 *
 * \return true on success; false if either value is out of bounds (nothing
 *         is changed) or the write failed.
 */
bool config_store_set(uint32_t max_retries, uint32_t retry_interval_ms);

#endif
