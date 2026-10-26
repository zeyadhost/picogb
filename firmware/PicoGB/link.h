// USB link to the loader (loader/index.html, loader/gbload.py).
//
// PC -> Pico:  "PGC" cmd(u8) len(u16) payload crc16(u16)
// Pico -> PC:  "PGR" type(u8) len(u16) payload crc16(u16)
// Little-endian; crc16 is CRC-16/CCITT-FALSE over cmd/type, len and payload.
//
// Each command is answered with a packet of the same letter. status is 0 for
// ok, 1 for an error, in which case an error message follows it.
//   'I'  info                                    -> 26 bytes: ver u8 (2), state u8 (0 menu, 1 game,
//        2 paused), game u8 (running game's index, 255 none), count u8, flash free bytes u32,
//        last reset: kind u32 (0 none, 1 crash, 2 watchdog), pc or stage u32, lr u32,
//        sd u8 (card mounted), sd free KB u32, games in flash u8
//   'L'  list                                    -> per game, 87 bytes: index u8, size u32,
//        flags u8 (1 has a save, 2 in flash, 4 can't run), name[64], title[17]
//   'U'  upload begin   size u32, file name...   -> status u8, message...
//   'D'  upload data    offset u32, bytes...     -> status u8, received u32, message...
//   'F'  upload finish  crc32 u32                -> status u8, index u8, message...
//   'X'  delete         index u8                 -> status u8, message...
//   'R'  run            index u8                 -> status u8, message...
//   'Q'  quit to menu                            -> status u8
//   'A'  perf counters  [reset u8]               -> 11 x u32: frames, underruns, slow frames,
//        max emu us, max frame us, avg emu us, stalls, max stall ms, avg ppu us, avg apu us,
//        dropped frames. A nonzero reset byte clears them after the reply.
//   'G'  read file      offset u32, path...      -> status u8, file size u32, up to 1024 bytes
//   'O'  list folder    path...                  -> status u8, "name<TAB>size<TAB>d|f\n" lines
//   'Z'  SD diagnostics                          -> text
//   'K'  held keys      mask u8 (GB_* bits)         (no reply)
//   'P'  palette        index u8                    (no reply)
// 'G' and 'O' paths are relative to /PicoGB on the card.
//
// Unsolicited while a loader is connected, skipped when the send queue is full
// (game frames go out at 30 fps):
//   'V'  screen. Header: kind u8, volume u8 (0-4), emu_us u16, frame u32. Then:
//        kind 0, game:  palette 16 x u16 RGB565, 160x144 at 2 bpp, 4 pixels per byte
//        kind 1, menu:  palette 16 x u16 RGB565, 160x128 at 4 bpp, 2 pixels per byte
//        kind 2, Color: palette 64 x u16 RGB565, first row u8 (0 or 72), then 72 rows
//                       of 160 bytes, one palette index each (half a frame per packet)
//        Packed pixels go left to right, top to bottom, first pixel in the low bits.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LINK_MAX_PAYLOAD 1100

typedef void (*link_handler_t)(uint8_t cmd, const uint8_t *payload, uint16_t len);

void link_init(link_handler_t handler);
void link_poll(void);                 // parses whatever arrived and calls the handler
bool link_connected(void);            // a loader has the port open
// Queues a packet. Waits for room if `wait`, otherwise drops it and returns false.
bool link_send(uint8_t type, const uint8_t *payload, uint16_t len, bool wait);
// Same, with the payload in two pieces (header + bulk data).
bool link_send2(uint8_t type, const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen, bool wait);
uint32_t link_tx_free(void);
// Builds a packet piece by piece straight into the send queue. begin() checks
// there is room for `len` payload bytes and returns false (queueing nothing)
// if not; then put() exactly `len` bytes and call end().
bool link_stream_begin(uint8_t type, uint16_t len);
void link_stream_put(const uint8_t *p, uint32_t n);
void link_stream_end(void);
