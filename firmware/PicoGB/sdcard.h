// microSD storage for PicoGB. Everything lives in /PicoGB on the card's FAT
// partition, next to whatever else is on it:
//   games/    .gb / .gbc files, copied there on the PC or uploaded by the loader
//   saves/    <game>.sav       battery RAM, same format as other emulators
//   states/   <game>.s1..s4    save states, <game>.res resume point
//   shots/    <game>-NNN.bmp   screenshots
//   settings.txt, log.txt
// Each call holds the shared SPI bus only for its own transfers.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SD_NAME_LEN 64     // game name without extension, including the 0
#define SD_MAX_GAMES 64

typedef struct {
    char name[SD_NAME_LEN];
    char title[17];         // from the ROM header
    char ext[5];
    uint32_t size, stamp;   // stamp: FAT modify date << 16 | time
    uint16_t gcsum;         // header global checksum, part of the cache key
    int8_t err;             // gb_cart_info() result, 0 if it can run
    bool has_save;
} sd_game_t;

bool sd_init(void);         // false: no card, or not FAT
bool sd_ok(void);
uint32_t sd_free_kb(void);  // computed once at mount, then tracked

// Scans games/ (sorted by name). Returns the count.
int sd_scan(sd_game_t *out, int max);

// Paths for a game's files, e.g. sd_path(buf, "saves", name, ".sav").
void sd_path(char *out, const char *dir, const char *name, const char *ext);
bool sd_exists(const char *path);
bool sd_remove(const char *path);

// Sequential read of one file at a time.
bool sd_read_open(const char *path, uint32_t *size);
int sd_read(void *buf, uint32_t n);  // bytes read, <0 on error
void sd_read_close(void);

// Sequential write to a temporary file that replaces `path` on commit, so a
// power cut never leaves a half-written save.
bool sd_write_open(const char *path);
bool sd_write(const void *buf, uint32_t n);
bool sd_write_commit(void);
void sd_write_abort(void);

// The running game's battery save, kept open so changed 512-byte blocks can
// be written in place a few at a time while it plays. open() creates the
// file from `ram` if it is missing or the wrong size.
bool sd_save_open(const char *path, const uint8_t *ram, uint32_t size);
bool sd_save_block(uint32_t off, const uint8_t *data, uint32_t n);
bool sd_save_sync(void);
void sd_save_close(void);

// Whole small files.
int sd_load(const char *path, void *buf, uint32_t max);  // bytes, <0 if missing
bool sd_store(const char *path, const void *buf, uint32_t n);
bool sd_append_line(const char *path, const char *line);

// For the loader's file access: one chunk of any file under /PicoGB, and a
// folder listing as "name<TAB>size<TAB>d|f" lines. Paths are relative to /PicoGB.
int sd_read_at(const char *rel, uint32_t off, void *buf, uint32_t n, uint32_t *size);  // <0: no file
int sd_list(const char *rel, char *out, int max);

// Human-readable report of the card and the PicoGB folders, for the loader.
int sd_diag(char *out, int max);

// Makes a safe FAT file name from an uploaded one.
void sd_clean_name(char *dst, const char *src, int max);
