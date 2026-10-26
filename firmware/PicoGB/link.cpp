#include "link.h"
#include "sprig_hw.h"
#include <Arduino.h>
#include <tusb.h>
#include <USB.h>
#include <string.h>

// Outgoing bytes go through a ring buffer that a timer interrupt drains into
// the 256-byte TinyUSB FIFO, so the emulator never blocks on USB. The same
// interrupt samples the buttons every millisecond.
#define TX_SIZE (1 << 14)
#define TICK_US 80
static uint8_t tx[TX_SIZE];
static volatile uint32_t tx_head, tx_tail;  // head: written by main code, tail: by the interrupt
static repeating_timer_t timer;
static link_handler_t on_packet;
static volatile bool connected;

static uint16_t crc_table[256];

static uint16_t crc16(uint16_t crc, const uint8_t *p, uint32_t n) {
    while (n--) crc = (crc << 8) ^ crc_table[((crc >> 8) ^ *p++) & 0xFF];
    return crc;
}

static bool tick(repeating_timer_t *) {
    static int ticks;
    if (++ticks >= 1000 / TICK_US) { ticks = 0; sprig_buttons_sample(); }
    if (!mutex_try_enter(&USB.mutex, nullptr)) return true;  // main code is in the USB stack
    tud_task();
    connected = tud_cdc_connected();
    uint32_t tail = tx_tail, head = tx_head;
    if (!connected) {
        tail = head;  // nobody listening: drop
    } else {
        while (tail != head) {
            uint32_t avail = tud_cdc_write_available();
            if (!avail) break;
            uint32_t n = (head - tail) & (TX_SIZE - 1), end = TX_SIZE - (tail & (TX_SIZE - 1));
            if (n > end) n = end;
            if (n > avail) n = avail;
            uint32_t sent = tud_cdc_write(tx + (tail & (TX_SIZE - 1)), n);
            if (!sent) break;  // e.g. the host just closed the port: retry next tick, never spin here
            tail += sent;
        }
        tud_cdc_write_flush();
    }
    tx_tail = tail;
    mutex_exit(&USB.mutex);
    return true;
}

void link_init(link_handler_t handler) {
    for (int i = 0; i < 256; i++) {
        uint16_t c = i << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
        crc_table[i] = c;
    }
    on_packet = handler;
    Serial.begin(115200);
    add_repeating_timer_us(-TICK_US, tick, nullptr, &timer);
}

bool link_connected(void) { return connected; }

uint32_t link_tx_free(void) { return TX_SIZE - 1 - ((tx_head - tx_tail) & (TX_SIZE - 1)); }

static void put(const uint8_t *p, uint32_t n) {
    uint32_t h = tx_head;
    while (n--) tx[h++ & (TX_SIZE - 1)] = *p++;
    tx_head = h & (TX_SIZE - 1);
}

bool link_send2(uint8_t type, const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen, bool wait) {
    uint32_t total = 8 + alen + blen;
    if (total > TX_SIZE - 1) return false;
    uint32_t give_up = time_us_32() + 500000;  // a stalled reader can't hang the Sprig
    while (link_tx_free() < total) {
        if (!wait || !connected || (int32_t)(time_us_32() - give_up) > 0) return false;
        tight_loop_contents();
    }
    uint16_t len = alen + blen;
    uint8_t hdr[6] = {'P', 'G', 'R', type, (uint8_t)len, (uint8_t)(len >> 8)};
    uint16_t crc = crc16(0xFFFF, hdr + 3, 3);
    crc = crc16(crc, a, alen);
    crc = crc16(crc, b, blen);
    uint8_t tail[2] = {(uint8_t)crc, (uint8_t)(crc >> 8)};
    put(hdr, 6);
    put(a, alen);
    put(b, blen);
    put(tail, 2);
    return true;
}

static uint16_t stream_crc;

bool link_stream_begin(uint8_t type, uint16_t len) {
    if (!connected || link_tx_free() < 8u + len) return false;
    uint8_t hdr[6] = {'P', 'G', 'R', type, (uint8_t)len, (uint8_t)(len >> 8)};
    stream_crc = crc16(0xFFFF, hdr + 3, 3);
    put(hdr, 6);
    return true;
}

void link_stream_put(const uint8_t *p, uint32_t n) {
    stream_crc = crc16(stream_crc, p, n);
    put(p, n);
}

void link_stream_end(void) {
    uint8_t tail[2] = {(uint8_t)stream_crc, (uint8_t)(stream_crc >> 8)};
    put(tail, 2);
}

bool link_send(uint8_t type, const uint8_t *payload, uint16_t len, bool wait) {
    return link_send2(type, payload, len, nullptr, 0, wait);
}

// ---------------------------------------------------------------- receive

static uint8_t rx[8 + LINK_MAX_PAYLOAD];
static uint32_t rx_n;

void link_poll(void) {
    int avail = Serial.available();
    while (avail > 0) {
        int n = sizeof rx - rx_n;
        if (n > avail) n = avail;
        n = Serial.readBytes((char *)rx + rx_n, n);
        if (n <= 0) break;
        rx_n += n;
        avail -= n;

        // Pull out complete packets; resync on the "PGC" magic.
        for (;;) {
            uint32_t k = 0;
            while (k + 3 <= rx_n && memcmp(rx + k, "PGC", 3)) k++;
            if (k) { memmove(rx, rx + k, rx_n - k); rx_n -= k; }
            if (rx_n < 6) break;
            uint16_t len = rx[4] | rx[5] << 8;
            if (len > LINK_MAX_PAYLOAD) { memmove(rx, rx + 1, --rx_n); continue; }
            if (rx_n < 8u + len) break;
            uint16_t crc = rx[6 + len] | rx[7 + len] << 8;
            if (crc16(0xFFFF, rx + 3, 3 + len) == crc) on_packet(rx[3], rx + 6, len);
            uint32_t used = crc16(0xFFFF, rx + 3, 3 + len) == crc ? 8 + len : 1;
            memmove(rx, rx + used, rx_n - used);
            rx_n -= used;
        }
    }
}
