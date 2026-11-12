/* ota_relay.cpp - receive a firmware image over ESP-NOW (BOX SIDE).
 *
 * Identical file in freestar_dash_iobox / freestar_pdb / freestar_ecu_bridge -
 * copy it, never edit one copy. Sender: freestar_dash/main/ota_relay.c.
 */

#include "ota_relay.h"

#include <Update.h>

#include "link_sec.h"

namespace {

constexpr uint8_t  F_BEGIN = 0xD1;
constexpr uint8_t  F_DATA  = 0xD2;
constexpr uint8_t  F_ACK   = 0xD3;
constexpr uint8_t  F_END   = 0xD4;

constexpr uint16_t SEQ_BEGIN = 0xFFFE;
constexpr uint16_t SEQ_END   = 0xFFFF;

/* Must match OTA_RELAY_CHUNK on the dash. */
constexpr uint16_t CHUNK_MAX = 200;

enum Status : uint8_t {
    ST_OK          = 0,
    ST_BUSY        = 1,
    ST_WRITE_FAIL  = 2,
    ST_BAD_DATA    = 3,
    ST_VERIFY_FAIL = 4,
    ST_WRONG_ROLE  = 5,
};

enum Stage : uint8_t { IDLE, PENDING_BEGIN, RECEIVING, PENDING_END };

uint8_t  s_roleMask = 0;

Stage    s_stage   = IDLE;
uint32_t s_size    = 0;
uint32_t s_crc     = 0;
uint16_t s_nchunks = 0;
uint16_t s_next    = 0;
uint32_t s_crcAcc  = 0xFFFFFFFFu;

bool     s_haveChunk = false;
uint16_t s_chunkSeq  = 0;
uint16_t s_chunkLen  = 0;
uint8_t  s_chunk[CHUNK_MAX];

uint32_t s_restartAt = 0;

/* ---- helpers ------------------------------------------------------------ */

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t crc16_of(const uint8_t *p, uint16_t n)
{
    uint16_t c = 0xFFFFu;
    for (uint16_t i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
        }
    }
    return c;
}

uint32_t crc32_update(uint32_t c, const uint8_t *p, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        c ^= p[i];
        for (uint8_t b = 0; b < 8; b++) {
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
        }
    }
    return c;
}

void ack(uint16_t seq, uint8_t status)
{
    uint8_t f[4];
    f[0] = F_ACK;
    f[1] = (uint8_t)(seq & 0xFF);
    f[2] = (uint8_t)(seq >> 8);
    f[3] = status;
    linkSecSend(f, sizeof(f));
}

}  // namespace

/* ---- public ------------------------------------------------------------- */

void otaRelayBegin(uint8_t roleMask)
{
    s_roleMask = roleMask;
}

bool otaRelayActive(void)
{
    return s_stage != IDLE;
}

bool otaRelayFrame(const uint8_t *data, int len)
{
    if (!data || len < 1) return false;

    switch (data[0]) {
    case F_BEGIN: {
        if (len < 12) return true;
        if (!(data[1] & s_roleMask)) {
            /* Someone else's image. Answer so the dash is not left guessing,
             * but never touch flash. */
            ack(SEQ_BEGIN, ST_WRONG_ROLE);
            return true;
        }
        const uint32_t size = rd32(data + 4);
        const uint32_t crc  = rd32(data + 8);

        if (s_stage == RECEIVING || s_stage == PENDING_END) {
            /* A repeated BEGIN for the image already in flight is almost always
             * a lost ack, not a new request - re-answering is what makes the
             * dash's retry work. A different image gets refused. */
            if (size == s_size && crc == s_crc) ack(SEQ_BEGIN, ST_OK);
            else                                ack(SEQ_BEGIN, ST_BUSY);
            return true;
        }
        if (s_stage == PENDING_BEGIN) {
            /* loop() is about to answer this same BEGIN. Stay silent so two
             * acks do not race and put the dash ahead of the write. */
            return true;
        }
        if (Update.isRunning()) {
            ack(SEQ_BEGIN, ST_BUSY);      /* the box's own ArduinoOTA is writing */
            return true;
        }

        s_size    = size;
        s_crc     = crc;
        s_nchunks = rd16(data + 2);
        s_next    = 0;
        s_crcAcc  = 0xFFFFFFFFu;
        s_haveChunk = false;
        s_stage     = PENDING_BEGIN;   /* loop() calls Update.begin() */
        return true;
    }

    case F_DATA: {
        if (len < 6) return true;
        const uint16_t seq = rd16(data + 1);
        const uint16_t n   = (uint16_t)(len - 5);

        if (s_stage != RECEIVING) {
            ack(seq, ST_BUSY);
            return true;
        }
        /* Retry of the chunk we already wrote and acked: re-ack, do not write
         * it twice. This is what makes a lost ack survivable. */
        if (seq == (uint16_t)(s_next - 1) && s_next > 0) {
            ack(seq, ST_OK);
            return true;
        }
        if (seq != s_next || n > CHUNK_MAX) {
            ack(seq, ST_BAD_DATA);
            return true;
        }
        if (crc16_of(data + 5, n) != rd16(data + 3)) {
            ack(seq, ST_BAD_DATA);
            return true;
        }
        memcpy(s_chunk, data + 5, n);
        s_chunkLen  = n;
        s_chunkSeq  = seq;
        s_haveChunk = true;             /* loop() writes it */
        return true;
    }

    case F_END: {
        if (len < 9) return true;
        if (s_stage != RECEIVING) {
            ack(SEQ_END, ST_BAD_DATA);
            return true;
        }
        if (rd32(data + 1) != s_size || rd32(data + 5) != s_crc) {
            ack(SEQ_END, ST_BAD_DATA);
            return true;
        }
        s_stage = PENDING_END;          /* loop() commits */
        return true;
    }

    default:
        return false;
    }
}

void otaRelayTick(void)
{
    /* A restart asked for by a successful commit. Delayed so the ack frame has
     * time to leave the radio before the CPU goes down. */
    if (s_restartAt && (int32_t)(millis() - s_restartAt) >= 0) {
        s_restartAt = 0;
        ESP.restart();
    }

    if (s_stage == PENDING_BEGIN) {
        s_stage = IDLE;
        if (!Update.begin(s_size)) {
            Update.abort();
            ack(SEQ_BEGIN, ST_WRITE_FAIL);
            return;
        }
        s_stage = RECEIVING;
        ack(SEQ_BEGIN, ST_OK);
        return;
    }

    if (s_haveChunk) {
        s_haveChunk = false;
        const size_t wrote = Update.write(s_chunk, s_chunkLen);
        if (wrote != s_chunkLen) {
            s_stage = IDLE;
            Update.abort();
            ack(s_chunkSeq, ST_WRITE_FAIL);
            return;
        }
        s_crcAcc = crc32_update(s_crcAcc, s_chunk, s_chunkLen);
        s_next   = (uint16_t)(s_chunkSeq + 1);
        ack(s_chunkSeq, ST_OK);
        return;
    }

    if (s_stage == PENDING_END) {
        s_stage = IDLE;
        const bool all = (Update.progress() == s_size);
        const bool crcOk = ((s_crcAcc ^ 0xFFFFFFFFu) == s_crc);
        const bool committed = all && crcOk && Update.end(false);
        if (!committed) {
            Update.abort();
            ack(SEQ_END, (all && crcOk) ? ST_WRITE_FAIL : ST_VERIFY_FAIL);
            return;
        }
        ack(SEQ_END, ST_OK);
        s_restartAt = millis() + 400;
    }
}
