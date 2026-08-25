#ifndef TEMP_SENSE_XFER_SESSION_H
#define TEMP_SENSE_XFER_SESSION_H

#include <stddef.h>

/*
 * The v1.2 protocol's REQUEST/DATA/ACK/NACK state machine (see
 * temp-logger-udp-protocol.md and xfer_proto.h). Added alongside the
 * existing fetch/ack commands, not replacing them yet — see the migration
 * plan's step 9 for the cutover.
 *
 * A single global in-flight-transfer struct is enough: there is one
 * designated collector, stop-and-wait means at most one packet is ever
 * outstanding, and every DATA packet is rebuilt on demand from the SD ring
 * rather than cached, so the only real state is which transfer/packet is
 * current.
 *
 * Call only when cmd[0] == XFER_MAGIC — temp-sense.c's handle_wifi_cmd()
 * checks that before falling into the ASCII strncmp chain.
 */
void xfer_session_handle(const char *cmd, size_t cmd_len, char *resp,
                          size_t resp_size, size_t *resp_len);

#endif
