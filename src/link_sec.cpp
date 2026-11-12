#include "link_sec.h"

#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#define LS_NS        "linksec"
#define LS_VER       1
#define LS_CHANNEL   1

#define F_PAIR_OPEN   0xE1
#define F_PAIR_REQ    0xE2
#define F_PAIR_KEY    0xE3
#define F_PAIR_ACK    0xE4
#define F_PAIR_FORGET 0xE5

#define LS_REQ_MS     500    /* 0xE2 rate while a window is open */
#define LS_WIN_MS     2500   /* a window is kept alive by 0xE1 at 5 Hz */
#define LS_ACK_COUNT  3      /* 0 / +150 / +300 ms - the dash arms its key at +250 */
#define LS_ACK_STEP   150
#define LS_CTR_SAVE   5      /* persist the counter at most every 5 s */
#define LS_CTR_BLOCK  8      /* or every 8 commands, whichever comes first */

static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static Preferences s_prefs;
static uint8_t  s_role   = 0;
static bool     s_paired = false;
static uint8_t  s_dashMac[6];
static uint8_t  s_lmk[16];

/* Window advertised by the dash (0xE1). */
static bool     s_winOpen = false;
static uint8_t  s_winMac[6];
static uint8_t  s_token[16];
static uint32_t s_winUntilMs = 0;
static uint32_t s_lastReqMs  = 0;
static bool     s_winPeer   = false;

/* Pending 0xE4 repeats. */
static uint32_t s_ackAtMs  = 0;
static uint8_t  s_ackLeft  = 0;

/* Replay counter. */
static uint32_t s_ctrSeen  = 0;
static uint32_t s_ctrSaved = 0;
static uint32_t s_ctrSaveMs = 0;

static LinkSecPairedCb s_pairedCb = nullptr;

static bool addEncPeer(const uint8_t *mac, const uint8_t *key) {
    esp_now_del_peer(mac);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    memcpy(p.lmk, key, 16);
    p.channel = LS_CHANNEL;
    p.ifidx   = WIFI_IF_AP;
    p.encrypt = true;
    return esp_now_add_peer(&p) == ESP_OK;
}

static bool addPlainPeer(const uint8_t *mac) {
    esp_now_del_peer(mac);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = LS_CHANNEL;
    p.ifidx   = WIFI_IF_AP;
    p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK;
}

/* The role is a property of the FIRMWARE, not of the stored pairing: each repo
 * is a different board. Reading it back from NVS meant a board reflashed with a
 * different role kept the old one and advertised the wrong role in 0xE2, so the
 * dash filed the key under the wrong slot. Never persist it. */
static void load() {
    size_t n = s_prefs.getBytesLength("mac");
    if (n != 6) return;
    if (s_prefs.getBytes("mac", s_dashMac, 6) != 6) return;
    n = s_prefs.getBytesLength("lmk");
    if (n != 16) return;
    if (s_prefs.getBytes("lmk", s_lmk, 16) != 16) return;
    s_paired = true;
}

static void save() {
    if (!s_paired) {
        s_prefs.remove("mac");
        s_prefs.remove("lmk");
        return;
    }
    s_prefs.putBytes("mac", s_dashMac, 6);
    s_prefs.putBytes("lmk", s_lmk, 16);
}

static void sendAck() {
    uint8_t f[9];
    f[0] = F_PAIR_ACK;
    f[1] = LS_VER;
    f[2] = s_role;
    memcpy(&f[3], s_dashMac, 6);
    /* Encrypted: this only goes out once the new key is installed, so its
     * arrival proves both ends hold the same one. */
    esp_now_send(s_dashMac, f, sizeof(f));
}

static void clearPairing(bool tellConsole) {
    s_paired = false;
    memset(s_dashMac, 0, 6);
    memset(s_lmk, 0, 16);
    save();
    if (tellConsole) Serial.println("linksec: dash forgotten (unpaired)");
}

void linkSecSetPairedCb(LinkSecPairedCb cb) { s_pairedCb = cb; }

void linkSecBegin(uint8_t roleMask) {
    s_role = roleMask;
    s_prefs.begin(LS_NS, false);
    load();
    s_ctrSeen  = s_prefs.getUInt("ctr", 0);
    s_ctrSaved = s_ctrSeen;

    if (s_paired) {
        if (addEncPeer(s_dashMac, s_lmk)) {
            Serial.printf("linksec: paired (encrypted) with %02x:%02x:%02x:%02x:%02x:%02x\n",
                          s_dashMac[0], s_dashMac[1], s_dashMac[2],
                          s_dashMac[3], s_dashMac[4], s_dashMac[5]);
        } else {
            Serial.println("linksec: encrypted peer add FAILED");
        }
        if (s_pairedCb) s_pairedCb(s_dashMac);
    } else {
        Serial.println("linksec: unpaired - broadcast in/out, open the pair window on the dash to bind");
    }
}

bool linkSecPaired() { return s_paired; }
const uint8_t *linkSecDashMac() { return s_paired ? s_dashMac : nullptr; }

bool linkSecSrcOk(const uint8_t *mac) {
    if (!mac) return false;
    if (!s_paired) return true;             /* legacy behaviour until paired */
    return memcmp(mac, s_dashMac, 6) == 0;
}

bool linkSecSend(const uint8_t *data, size_t len) {
    if (s_paired) return esp_now_send(s_dashMac, data, len);
    return esp_now_send(kBroadcast, data, len);
}

bool linkSecCmdOk(uint32_t ctr) {
    /* Strictly increasing: a captured command frame replayed later is dropped.
     * Pairs with the dash's block-reserved counter, so a dash reboot resumes
     * above anything the box has already accepted. */
    if (ctr <= s_ctrSeen) return false;
    s_ctrSeen = ctr;
    uint32_t now = millis();
    if (s_ctrSeen - s_ctrSaved >= LS_CTR_BLOCK ||
        (uint32_t)(now - s_ctrSaveMs) >= LS_CTR_SAVE * 1000u) {
        s_prefs.putUInt("ctr", s_ctrSeen);
        s_ctrSaved = s_ctrSeen;
        s_ctrSaveMs = now;
    }
    return true;
}

void linkSecClear() {
    if (s_paired) esp_now_del_peer(s_dashMac);
    clearPairing(true);
    s_winOpen = false;
    if (s_winPeer) { esp_now_del_peer(s_winMac); s_winPeer = false; }
}

bool linkSecFrame(const uint8_t *src, const uint8_t *data, int len) {
    if (!src || !data || len < 3) return false;

    switch (data[0]) {
    case F_PAIR_OPEN: {
        if (len < 19 || data[1] != LS_VER) return true;
        if (!(data[2] & s_role)) return true;              /* not for this board */
        if (s_paired && memcmp(src, s_dashMac, 6) != 0) return true;  /* someone else's window */

        /* Keep a plaintext peer entry so 0xE2 can be unicast back. A paired box
         * already has an encrypted entry for this MAC, and that one is used. */
        if (!s_paired) {
            if (!s_winPeer || memcmp(s_winMac, src, 6) != 0) {
                if (s_winPeer) esp_now_del_peer(s_winMac);
                s_winPeer = addPlainPeer(src);
            }
        }
        memcpy(s_winMac, src, 6);
        memcpy(s_token, &data[3], 16);
        s_winOpen = true;
        s_winUntilMs = millis() + LS_WIN_MS;
        s_lastReqMs = 0;      /* answer immediately */
        return true;
    }

    case F_PAIR_KEY: {
        if (len < 41 || data[1] != LS_VER) return true;
        if (data[2] != s_role) return true;
        if (s_paired && memcmp(src, s_dashMac, 6) != 0) {
            Serial.println("linksec: PAIR_KEY from a different dash rejected (PAIRC to re-pair)");
            return true;
        }
        /* Accept inside a window with the right token, or from the dash we are
         * already paired to (the dash re-sends the key encrypted 250 ms later,
         * and a re-pair must not need the window to still be open). */
        const bool inWindow = s_winOpen && memcmp(&data[9], s_token, 16) == 0;
        const bool fromPaired = s_paired && memcmp(src, s_dashMac, 6) == 0;
        if (!inWindow && !fromPaired) {
            Serial.println("linksec: PAIR_KEY outside the window rejected");
            return true;
        }

        /* Install the key BEFORE acking: the ack is encrypted with it, so both
         * ends must hold the same value for the frame to be readable at all. */
        if (!addEncPeer(src, &data[25])) {
            Serial.println("linksec: could not add the encrypted peer");
            return true;
        }
        memcpy(s_dashMac, src, 6);
        memcpy(s_lmk, &data[25], 16);
        s_paired = true;
        save();
        s_winOpen = false;
        /* addEncPeer() already replaced the plaintext entry for this MAC, so
         * only drop the window peer when it was a different device. */
        if (s_winPeer && memcmp(s_winMac, src, 6) != 0) esp_now_del_peer(s_winMac);
        s_winPeer = false;

        Serial.printf("linksec: paired with %02x:%02x:%02x:%02x:%02x:%02x\n",
                      src[0], src[1], src[2], src[3], src[4], src[5]);
        if (s_pairedCb) s_pairedCb(s_dashMac);

        /* Three acks: the dash only arms the new key ~250 ms after it sent the
         * plaintext copy, and a single early frame would be undecryptable. */
        s_ackLeft = LS_ACK_COUNT;
        s_ackAtMs = millis();
        sendAck();
        s_ackLeft--;
        s_ackAtMs = millis() + LS_ACK_STEP;
        return true;
    }

    case F_PAIR_FORGET: {
        if (len < 9 || data[1] != LS_VER) return true;
        if (!s_paired || memcmp(src, s_dashMac, 6) != 0) return true;
        esp_now_del_peer(s_dashMac);
        clearPairing(true);
        return true;
    }

    default:
        return false;
    }
}

void linkSecTick() {
    uint32_t now = millis();

    if (s_winOpen && (int32_t)(now - s_winUntilMs) >= 0) s_winOpen = false;

    if (s_winOpen && (s_lastReqMs == 0 || (uint32_t)(now - s_lastReqMs) >= LS_REQ_MS)) {
        s_lastReqMs = now ? now : 1;
        uint8_t f[25];
        f[0] = F_PAIR_REQ;
        f[1] = LS_VER;
        f[2] = (uint8_t)s_role;
        esp_wifi_get_mac(WIFI_IF_AP, &f[3]);   /* our own MAC, for the page */
        memcpy(&f[9], s_token, 16);
        esp_now_send(s_winMac, f, sizeof(f));
    }

    if (s_ackLeft && (int32_t)(now - s_ackAtMs) >= 0) {
        sendAck();
        s_ackLeft--;
        s_ackAtMs = now + LS_ACK_STEP;
    }
}

void linkSecPrint(Stream &out) {
    out.println("--- link security ---");
    if (s_paired) {
        out.printf("paired: %02x:%02x:%02x:%02x:%02x:%02x  (unicast, AES)\n",
                   s_dashMac[0], s_dashMac[1], s_dashMac[2],
                   s_dashMac[3], s_dashMac[4], s_dashMac[5]);
        out.printf("cmd counter seen: %lu\n", (unsigned long)s_ctrSeen);
    } else {
        out.println("paired: NO - broadcast, unencrypted (open the pair window on the dash)");
    }
    out.printf("window: %s\n", s_winOpen ? "open (answering)" : "none");
    out.println("PAIRC = forget the dash and its key");
}
