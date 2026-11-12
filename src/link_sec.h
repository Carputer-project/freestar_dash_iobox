/* link_sec.h - ESP-NOW pairing + link security for the box side.
 *
 * Identical file in freestar_dash_iobox / freestar_pdb / freestar_ecu_bridge.
 * Protocol: how_to_md_files/espnow_pairing_protocol.md
 *
 * Before this, every box accepted frames from ANY ESP32 in radio range: `F 0`
 * turned the fan off, `I 0` closed the IAC, `B 0` muted the buzzer, anything
 * could claim to be the dash. Now the dash and the box share a per-box key and
 * every frame is unicast + AES encrypted, the box only answers its paired dash,
 * and commands carry a replay counter.
 *
 * The key is handed over once, in the clear, inside a window a human opens on
 * the dash page. That is WPS by design; see the doc for what it does and does
 * not protect against before calling it secure.
 */
#pragma once

#include <Arduino.h>

enum LinkSecRole : uint8_t {
    LINK_ROLE_IOBOX  = 1,
    LINK_ROLE_PDB    = 2,
    LINK_ROLE_BRIDGE = 4,
};

/* Call from setup() after esp_now_init() and after the broadcast peer exists. */
void linkSecBegin(uint8_t roleMask);

/* Call from loop(): sends 0xE2 while a pairing window is advertised. */
void linkSecTick();

/* Call FIRST in the ESP-NOW receive callback. Returns true when the frame was a
 * pairing frame and must not be decoded further. */
bool linkSecFrame(const uint8_t *src, const uint8_t *data, int len);

/* True when a frame from `mac` may be attributed to the dash: always true while
 * unpaired (so a box that never went through the page keeps working), strict
 * once paired. */
bool linkSecSrcOk(const uint8_t *mac);

/* Send a frame to the dash: unicast + encrypted when paired, broadcast when
 * not. Same call shape as esp_now_send(). */
bool linkSecSend(const uint8_t *data, size_t len);

/* Replay check for command frames: true only for a strictly increasing value. */
bool linkSecCmdOk(uint32_t ctr);

bool linkSecPaired();
const uint8_t *linkSecDashMac();   /* NULL when unpaired */

/* 'PAIRC' on the console: forget the dash and its key. */
void linkSecClear();

/* Optional: called with the dash MAC whenever a pairing completes, so a board
 * that already keeps its own peer allow-list (the iobox's dashmac) can copy it
 * in and the two never disagree. */
typedef void (*LinkSecPairedCb)(const uint8_t *mac);
void linkSecSetPairedCb(LinkSecPairedCb cb);

void linkSecPrint(Stream &out);
