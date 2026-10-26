// Game store in the Pico's flash, after the program:
//   LIB_BASE + 0x0000, 0x1000   directory, two copies (the newer valid one wins)
//   LIB_BASE + 0x2000 ...       ROM images and save areas, 4 KB aligned
// The last 4 KB of flash belong to arduino-pico's EEPROM emulation.
//
// With an SD card this is a cache: games are copied in from the card when
// launched (the "cartridge slot"), keyed by name, size, file date and header
// checksum, and the least recently played are evicted to make room. Without
// a card, games uploaded over USB live here with their own save areas.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LIB_BASE      0x80000u              // 512 KB: the program must stay below this
#define LIB_END       (0x200000u - 0x1000u)
#define LIB_SECTOR    0x1000u
#define LIB_MAX_GAMES 24
#define LIB_NAME_LEN  64

typedef struct {
    char name[LIB_NAME_LEN];
    char title[17];            // from the ROM header
    uint8_t used;
    uint16_t gcsum;            // header global checksum
    uint32_t rom_off, rom_len;
    uint32_t save_off, save_len;  // 0 for SD-cached games: their saves live on the card
    uint32_t crc, stamp, lru;
} lib_entry_t;

void lib_init(void);
int lib_count(void);
const lib_entry_t *lib_get(int i);        // i-th used entry, or null
int lib_find(const char *name, uint32_t size, uint32_t stamp, uint16_t gcsum);
void lib_touch(int i);                    // mark as most recently played
const uint8_t *lib_rom(const lib_entry_t *e);
uint32_t lib_free_bytes(void);
bool lib_program_fits(void);              // the firmware ends below LIB_BASE

// Adding a game: begin() reserves space, evicting cached games if `evict`;
// write() takes the data in order; finish() checks the CRC and the ROM header
// and commits the entry. Each returns null or an error message.
const char *lib_upload_begin(const char *name, uint32_t size, uint32_t stamp, bool save_area, bool evict);
const char *lib_upload_write(uint32_t offset, const uint8_t *data, uint32_t len);
const char *lib_upload_finish(uint32_t crc, int *index);
void lib_upload_abort(void);
uint32_t lib_upload_progress(void);
bool lib_upload_active(void);

const char *lib_delete(int i);
void lib_save_write(const lib_entry_t *e, const uint8_t *ram, uint32_t len);
void lib_save_read(const lib_entry_t *e, uint8_t *ram, uint32_t len);

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);
