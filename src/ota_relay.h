/* ota_relay.h - receive a firmware image over ESP-NOW (BOX SIDE).
 *
 * Identical file in freestar_dash_iobox / freestar_pdb / freestar_ecu_bridge.
 * Sender + wire format: freestar_dash/main/ota_relay.h (read that first).
 *
 * The dash is the only board with a network path, so it holds the verified
 * image and pushes it here. The box has no credentials, no display and no
 * reason to reach the internet - it just accepts an image from the dash it is
 * already paired to and writes it to the idle OTA slot.
 *
 *   0xD1 BEGIN  [D1][role u8][nchunks u16 LE][size u32][crc32 u32]
 *   0xD2 DATA   [D2][seq u16 LE][crc16 u16][payload 1..200]
 *   0xD3 ACK    [D3][seq u16 LE][status u8]
 *   0xD4 END    [D4][size u32][crc32 u32]
 *
 * WHY THE WORK IS SPLIT BETWEEN THE CALLBACK AND loop():
 * Update.begin()/write()/end() disable the flash cache and block for tens of
 * milliseconds, sometimes seconds. Running them inside the ESP-NOW receive
 * callback would stall the WiFi task and drop frames. So the callback only
 * parses, checks and stages one chunk; otaRelayTick() does every flash call
 * and sends the ack.
 *
 * The role byte is a mask (1 = iobox, 2 = PDB, 4 = bridge). It stops a
 * mis-selected image from being written into the wrong board: the PDB image
 * addressed to role 2 is dropped by the iobox even on an unpaired broadcast
 * link.
 */
#pragma once

#include <Arduino.h>

/* Call once in setup(), after linkSecBegin(role). */
void otaRelayBegin(uint8_t roleMask);

/* Call FIRST-in-callback style from the ESP-NOW receive handler, after
 * linkSecFrame() and after the sender check. Returns true when the frame was
 * an OTA relay frame and must not be decoded further. */
bool otaRelayFrame(const uint8_t *data, int len);

/* Call at the top of loop(). Does all Update.* work and the acks. */
void otaRelayTick(void);

/* True while an image is being received. A board that is writing flash should
 * not also be trusted for control decisions; exposed for a status flag. */
bool otaRelayActive(void);
