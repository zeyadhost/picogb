#include "library.h"
#include "gb.h"
#include <Arduino.h>
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <string.h>

#define DIR_A     LIB_BASE
#define DIR_B     (LIB_BASE + LIB_SECTOR)
#define DATA_BASE (LIB_BASE + 2 * LIB_SECTOR)
#define DIR_MAGIC 0x324C4750u  // "PGL2"
#define BLOCK     0x10000u     // 64 KB erase block

typedef struct {
    uint32_t magic, seq, crc, lru_clock;
    lib_entry_t e[LIB_MAX_GAMES];
} lib_dir_t;
static_assert(sizeof(lib_dir_t) <= LIB_SECTOR, "directory must fit one sector");

static lib_dir_t dir;
static uint32_t dir_at;  // flash offset of the copy `dir` was loaded from

static inline const uint8_t *flash_ptr(uint32_t off) { return (const uint8_t *)(XIP_BASE + off); }

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
            table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// Code runs from flash (XIP), which is unreadable while it is being written,
// so core 1 is paused and interrupts are off for the length of each call.
static void flash_op(uint32_t off, uint32_t erase_len, const uint8_t *data, uint32_t data_len) {
    rp2040.idleOtherCore();
    uint32_t s = save_and_disable_interrupts();
    if (erase_len) flash_range_erase(off, erase_len);
    if (data) flash_range_program(off, data, data_len);
    restore_interrupts(s);
    rp2040.resumeOtherCore();
}

static void flash_sector(uint32_t off, const uint8_t *data) { flash_op(off, LIB_SECTOR, data, LIB_SECTOR); }

static uint32_t dir_crc(const lib_dir_t *d) {
    return crc32_update(d->lru_clock, (const uint8_t *)d->e, sizeof d->e);
}

static bool dir_valid(const lib_dir_t *d) { return d->magic == DIR_MAGIC && d->crc == dir_crc(d); }

static struct {
    bool active, save_area;
    char name[LIB_NAME_LEN];
    uint32_t off, size, got, stamp, erased_to;
    uint32_t crc;
    uint8_t sector[LIB_SECTOR];
} up;  // up.sector doubles as the scratch sector for directory and save writes

static void dir_commit() {
    uint8_t *buf = up.sector;
    dir.seq++;
    dir.crc = dir_crc(&dir);
    memset(buf, 0xFF, LIB_SECTOR);
    memcpy(buf, &dir, sizeof dir);
    dir_at = dir_at == DIR_A ? DIR_B : DIR_A;  // never overwrite the copy we trust
    flash_sector(dir_at, buf);
}

void lib_init(void) {
    const lib_dir_t *a = (const lib_dir_t *)flash_ptr(DIR_A), *b = (const lib_dir_t *)flash_ptr(DIR_B);
    bool va = dir_valid(a), vb = dir_valid(b);
    if (va && (!vb || a->seq >= b->seq)) { dir = *a; dir_at = DIR_A; }
    else if (vb) { dir = *b; dir_at = DIR_B; }
    else {
        memset(&dir, 0, sizeof dir);
        dir.magic = DIR_MAGIC;
        dir_at = DIR_B;  // first commit goes to A
    }
}

bool lib_program_fits(void) {
    extern char __flash_binary_end;
    return (uintptr_t)&__flash_binary_end <= XIP_BASE + LIB_BASE;
}

int lib_count(void) {
    int n = 0;
    for (int i = 0; i < LIB_MAX_GAMES; i++) n += dir.e[i].used;
    return n;
}

static int slot_of(int i) {
    for (int k = 0; k < LIB_MAX_GAMES; k++)
        if (dir.e[k].used && i-- == 0) return k;
    return -1;
}

const lib_entry_t *lib_get(int i) {
    int k = slot_of(i);
    return k < 0 ? nullptr : &dir.e[k];
}

int lib_find(const char *name, uint32_t size, uint32_t stamp, uint16_t gcsum) {
    int idx = 0;
    for (int k = 0; k < LIB_MAX_GAMES; k++) {
        const lib_entry_t *e = &dir.e[k];
        if (!e->used) continue;
        if (e->rom_len == size && e->stamp == stamp && e->gcsum == gcsum && !strcmp(e->name, name)) return idx;
        idx++;
    }
    return -1;
}

void lib_touch(int i) {
    int k = slot_of(i);
    if (k < 0 || dir.e[k].lru == dir.lru_clock) return;  // already the most recent: no flash write
    dir.e[k].lru = ++dir.lru_clock;
    dir_commit();
}

const uint8_t *lib_rom(const lib_entry_t *e) { return flash_ptr(e->rom_off); }

static uint32_t round_up(uint32_t n) { return (n + LIB_SECTOR - 1) & ~(LIB_SECTOR - 1); }

// First free gap of `len` bytes in the data area, or 0.
static uint32_t alloc(uint32_t len, uint32_t also_off = 0, uint32_t also_len = 0) {
    len = round_up(len);
    uint32_t at = DATA_BASE;
    for (;;) {
        if (at + len > LIB_END) return 0;
        uint32_t clash = 0;
        auto check = [&](uint32_t off, uint32_t n) {
            n = round_up(n);
            if (n && at < off + n && off < at + len) clash = clash > off + n ? clash : off + n;
        };
        for (int i = 0; i < LIB_MAX_GAMES; i++) {
            if (!dir.e[i].used) continue;
            check(dir.e[i].rom_off, dir.e[i].rom_len);
            check(dir.e[i].save_off, dir.e[i].save_len);
        }
        check(also_off, also_len);
        if (!clash) return at;
        at = clash;
    }
}

uint32_t lib_free_bytes(void) {
    uint32_t used = 0;
    for (int i = 0; i < LIB_MAX_GAMES; i++)
        if (dir.e[i].used) used += round_up(dir.e[i].rom_len) + round_up(dir.e[i].save_len);
    return LIB_END - DATA_BASE - used;
}

// Drops the least recently played cached game (one without a flash save).
static bool evict_one() {
    int best = -1;
    for (int k = 0; k < LIB_MAX_GAMES; k++)
        if (dir.e[k].used && !dir.e[k].save_len && (best < 0 || dir.e[k].lru < dir.e[best].lru)) best = k;
    if (best < 0) return false;
    dir.e[best].used = 0;
    return true;
}

// ---------------------------------------------------------------- upload

bool lib_upload_active(void) { return up.active; }
uint32_t lib_upload_progress(void) { return up.got; }
void lib_upload_abort(void) { up.active = false; }

const char *lib_upload_begin(const char *name, uint32_t size, uint32_t stamp, bool save_area, bool evict) {
    up.active = false;
    if (size < 0x8000 || size > (8u << 20) || (size & 0x3FFF)) return "not a Game Boy ROM size";
    if (round_up(size) > LIB_END - DATA_BASE) return "game too big for flash";
    bool evicted = false;
    uint32_t off;
    for (;;) {
        bool slot = false;
        for (int i = 0; i < LIB_MAX_GAMES; i++) slot |= !dir.e[i].used;
        off = slot ? alloc(size) : 0;
        // Leave room for the largest save so finish can't run out of space.
        if (off && (!save_area || alloc(GB_MAX_CART_RAM, off, size))) break;
        if (!evict || !evict_one()) {
            if (evicted) dir_commit();
            return slot ? "not enough flash space" : "library full";
        }
        evicted = true;
    }
    if (evicted) dir_commit();
    strncpy(up.name, name, LIB_NAME_LEN - 1);
    up.name[LIB_NAME_LEN - 1] = 0;
    up.off = off;
    up.size = size;
    up.stamp = stamp;
    up.save_area = save_area;
    up.got = 0;
    up.crc = 0;
    up.erased_to = off;
    up.active = true;
    return nullptr;
}

// Programs one 4 KB sector, erasing ahead in 64 KB blocks where the
// allocation allows (a block erase costs about as much as one sector erase).
static void program_sector(uint32_t at, const uint8_t *data) {
    if (at >= up.erased_to) {
        uint32_t end = up.off + round_up(up.size);
        uint32_t len = (at % BLOCK == 0 && at + BLOCK <= end) ? BLOCK : LIB_SECTOR;
        flash_op(at, len, data, LIB_SECTOR);
        up.erased_to = at + len;
    } else {
        flash_op(at, 0, data, LIB_SECTOR);
    }
}

const char *lib_upload_write(uint32_t offset, const uint8_t *data, uint32_t len) {
    if (!up.active) return "no upload in progress";
    if (offset != up.got || up.got + len > up.size) return "out of order";
    while (len) {
        uint32_t in = up.got % LIB_SECTOR, n = LIB_SECTOR - in;
        if (n > len) n = len;
        memcpy(up.sector + in, data, n);
        up.crc = crc32_update(up.crc, data, n);
        up.got += n;
        data += n;
        len -= n;
        if (up.got % LIB_SECTOR == 0) program_sector(up.off + up.got - LIB_SECTOR, up.sector);
    }
    return nullptr;
}

const char *lib_upload_finish(uint32_t crc, int *index) {
    if (!up.active) return "no upload in progress";
    up.active = false;
    if (up.got != up.size) return "upload incomplete";
    if (crc != up.crc) return "checksum mismatch";
    const uint8_t *rom = flash_ptr(up.off);
    if (crc32_update(0, rom, up.size) != crc) return "flash verify failed";

    uint8_t mbc;
    bool battery, rtc;
    uint32_t ram;
    int err = gb_cart_info(rom, up.size, &mbc, &battery, &rtc, &ram);
    if (err == GB_ERR_CGB_ONLY) return "Game Boy Color only";
    if (err == GB_ERR_MAPPER) return "unsupported cartridge type";
    if (err == GB_ERR_RAM) return "save RAM over 32 KB";
    if (err) return "bad ROM header";

    int slot = -1;
    for (int i = 0; i < LIB_MAX_GAMES; i++)
        if (!dir.e[i].used) { slot = i; break; }
    if (slot < 0) return "library full";
    lib_entry_t *e = &dir.e[slot];
    memset(e, 0, sizeof *e);
    memcpy(e->name, up.name, LIB_NAME_LEN);
    gb_title(rom, e->title);
    e->rom_off = up.off;
    e->rom_len = up.size;
    e->crc = crc;
    e->stamp = up.stamp;
    e->gcsum = rom[0x14E] << 8 | rom[0x14F];
    e->lru = ++dir.lru_clock;
    if (up.save_area && battery && ram) {
        e->save_len = ram;
        e->save_off = alloc(e->save_len, up.off, up.size);
        for (uint32_t o = 0; o < round_up(e->save_len); o += LIB_SECTOR) flash_sector(e->save_off + o, nullptr);
    }
    e->used = 1;
    dir_commit();
    *index = 0;
    for (int i = 0; i < slot; i++) *index += dir.e[i].used;
    return nullptr;
}

const char *lib_delete(int i) {
    int k = slot_of(i);
    if (k < 0) return "no such game";
    dir.e[k].used = 0;
    dir_commit();
    return nullptr;
}

// ---------------------------------------------------------------- saves

void lib_save_write(const lib_entry_t *e, const uint8_t *ram, uint32_t len) {
    uint8_t *buf = up.sector;  // no upload runs while a game is saving
    if (!e->save_len) return;
    if (len > e->save_len) len = e->save_len;
    for (uint32_t o = 0; o < len; o += LIB_SECTOR) {
        uint32_t n = len - o < LIB_SECTOR ? len - o : LIB_SECTOR;
        if (!memcmp(flash_ptr(e->save_off + o), ram + o, n)) continue;  // only rewrite changed sectors
        memset(buf, 0xFF, LIB_SECTOR);
        memcpy(buf, ram + o, n);
        flash_sector(e->save_off + o, buf);
    }
}

void lib_save_read(const lib_entry_t *e, uint8_t *ram, uint32_t len) {
    memset(ram, 0xFF, len);
    if (e->save_len) memcpy(ram, flash_ptr(e->save_off), len < e->save_len ? len : e->save_len);
}
