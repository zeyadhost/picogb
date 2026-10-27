// CPU (Sharp SM83), memory map, timer, joypad, serial and cartridge mappers.
#include "gb.h"
#include <string.h>

uint32_t gb_prof_ppu_us, gb_prof_apu_us;

enum { FZ = 0x80, FN = 0x40, FH = 0x20, FC = 0x10 };
enum { RB, RC, RD, RE, RH, RL, R_HL, RA };
enum { IF_VBLANK = 1, IF_STAT = 2, IF_TIMER = 4, IF_SERIAL = 8, IF_JOYPAD = 16 };

#define IF g->io[0x0F]

// ---------------------------------------------------------------- mapper

// ROM bank numbers wrap at the ROM size. That's a power of two except for a
// few padded homebrew ROMs, and division is slow (and lives in flash).
GB_INLINE uint32_t wrap_bank(const gb_t *g, uint32_t b) {
    uint32_t n = g->rom_banks;
    return (n & (n - 1)) ? b % n : b & (n - 1);
}

// The banks mapped at 0000-3FFF and 4000-7FFF.
GB_INLINE void rom_banks_now(const gb_t *g, uint32_t *lo_out, uint32_t *hi_out) {
    uint32_t lo = 0, hi;
    switch (g->mbc) {
    case 1:
        hi = (g->bank_hi << 5) | ((g->rom_bank & 0x1F) ? (g->rom_bank & 0x1F) : 1);
        if (g->mbc1_mode) lo = g->bank_hi << 5;
        break;
    case 2: hi = (g->rom_bank & 0x0F) ? (g->rom_bank & 0x0F) : 1; break;
    case 3: hi = (g->rom_bank & 0x7F) ? (g->rom_bank & 0x7F) : 1; break;
    case 5: hi = g->rom_bank & 0x1FF; break;
    default: hi = 1;
    }
    *lo_out = wrap_bank(g, lo);
    *hi_out = wrap_bank(g, hi);
}

// Where page `page` of a bank is, or null if it has to be loaded first.
GB_INLINE const uint8_t *rom_page_ptr(gb_t *g, uint32_t bank, uint32_t page) {
    if (bank == 0) return g->rom_bank0 + page * 0x1000;
    if (bank < g->rom_resident) return g->rom + bank * 0x4000 + page * 0x1000;
    return g->rom_page(g, bank, page, false);
}

static void GB_FAST(map_rom)(gb_t *g) {
    uint32_t lo, hi;
    rom_banks_now(g, &lo, &hi);
    for (int i = 0; i < 4; i++) {
        g->rd[i] = rom_page_ptr(g, lo, i);
        g->rd[4 + i] = rom_page_ptr(g, hi, i);
    }
}

// A read from a ROM page that isn't in memory: load it (big games only).
// Loading may drop other pages from the cache, so the map is rebuilt.
static uint8_t GB_FAST(read_rom_slow)(gb_t *g, uint16_t a) {
    uint32_t lo, hi;
    rom_banks_now(g, &lo, &hi);
    const uint8_t *p = g->rom_page ? g->rom_page(g, a < 0x4000 ? lo : hi, (a >> 12) & 3, true) : nullptr;
    map_rom(g);
    return p ? p[a & 0xFFF] : 0xFF;
}

// Cart RAM: fast reads for plain RAM; writes always take the slow path so
// saves know when the RAM changed. RAM sizes are powers of two.
static void GB_FAST(map_ram)(gb_t *g) {
    g->rd[0xA] = g->rd[0xB] = nullptr;
    if (g->ram_en && g->ram_size >= 0x2000 && g->mbc != 2 && !(g->mbc == 3 && g->ram_bank >= 8)) {
        uint32_t nb = g->ram_size >> 13;
        uint8_t bank = (g->mbc == 1) ? (g->mbc1_mode ? g->bank_hi : 0) : g->ram_bank;
        uint8_t *p = g->cart_ram + (bank & (nb - 1)) * 0x2000;
        g->rd[0xA] = p;
        g->rd[0xB] = p + 0x1000;
    }
}

GB_INLINE void map_banks(gb_t *g) {
    map_rom(g);
    map_ram(g);
}

static uint8_t *GB_FAST(cart_ram_ptr)(gb_t *g, uint16_t a) {
    if (!g->ram_en || g->ram_size == 0) return nullptr;
    if (g->mbc == 2) return g->cart_ram + (a & 0x1FF);
    uint8_t bank = (g->mbc == 1) ? (g->mbc1_mode ? g->bank_hi : 0) : g->ram_bank;
    uint32_t off = (uint32_t)bank * 0x2000 + (a & 0x1FFF);
    return g->cart_ram + (off & (g->ram_size - 1));
}

static void GB_FAST(rtc_tick)(gb_t *g, uint32_t t) {
    if (!g->has_rtc || (g->rtc[4] & 0x40)) return;  // RTC halt bit: clock stopped
    g->rtc_cycles += t;
    while (g->rtc_cycles >= GB_CLOCK) {
        g->rtc_cycles -= GB_CLOCK;
        if (++g->rtc[0] < 60) continue;
        g->rtc[0] = 0;
        if (++g->rtc[1] < 60) continue;
        g->rtc[1] = 0;
        if (++g->rtc[2] < 24) continue;
        g->rtc[2] = 0;
        uint16_t day = (g->rtc[3] | (g->rtc[4] & 1) << 8) + 1;
        if (day > 511) { day = 0; g->rtc[4] |= 0x80; }  // day counter overflow flag
        g->rtc[3] = day;
        g->rtc[4] = (g->rtc[4] & 0xFE) | (day >> 8);
    }
}

// Bank switches are frequent (some games switch thousands of times a frame),
// so only the part of the map a register affects is rebuilt, and only when
// the value changes.
static void GB_FAST(mbc_write)(gb_t *g, uint16_t a, uint8_t v) {
    switch (g->mbc) {
    case 1:
        if (a < 0x2000) {
            bool en = (v & 0x0F) == 0x0A;
            if (en != g->ram_en) { g->ram_en = en; map_ram(g); }
        } else if (a < 0x4000) {
            if ((v & 0x1F) != g->rom_bank) { g->rom_bank = v & 0x1F; map_rom(g); }
        } else if (a < 0x6000) {
            if ((v & 3) != g->bank_hi) { g->bank_hi = v & 3; map_banks(g); }
        } else if ((v & 1) != g->mbc1_mode) {
            g->mbc1_mode = v & 1;
            map_banks(g);
        }
        break;
    case 2:
        if (a < 0x4000) {
            if (a & 0x100) {
                if ((v & 0x0F) != g->rom_bank) { g->rom_bank = v & 0x0F; map_rom(g); }
            } else {
                g->ram_en = (v & 0x0F) == 0x0A;  // MBC2 RAM is never mapped directly
            }
        }
        break;
    case 3:
        if (a < 0x2000) {
            bool en = (v & 0x0F) == 0x0A;
            if (en != g->ram_en) { g->ram_en = en; map_ram(g); }
        } else if (a < 0x4000) {
            if ((v & 0x7F) != g->rom_bank) { g->rom_bank = v & 0x7F; map_rom(g); }
        } else if (a < 0x6000) {
            if (v != g->ram_bank) { g->ram_bank = v; map_ram(g); }
        } else {
            if (g->rtc_latch == 0 && v == 1)
                for (int i = 0; i < 5; i++) g->rtc_latched[i] = g->rtc[i];
            g->rtc_latch = v;
        }
        break;
    case 5:
        if (a < 0x2000) {
            bool en = (v & 0x0F) == 0x0A;
            if (en != g->ram_en) { g->ram_en = en; map_ram(g); }
        } else if (a < 0x4000) {
            uint16_t bank = a < 0x3000 ? (g->rom_bank & 0x100) | v : (g->rom_bank & 0xFF) | (v & 1) << 8;
            if (bank != g->rom_bank) { g->rom_bank = bank; map_rom(g); }
        } else if (a < 0x6000) {
            if ((v & 0x0F) != g->ram_bank) { g->ram_bank = v & 0x0F; map_ram(g); }
        }
        break;
    }
}

// ---------------------------------------------------------------- I/O

GB_INLINE uint8_t joypad_read(gb_t *g) {
    uint8_t sel = g->io[0x00] & 0x30, n = 0x0F;
    if (!(sel & 0x10)) n &= ~(g->buttons & 0x0F);
    if (!(sel & 0x20)) n &= ~(g->buttons >> 4);
    return 0xC0 | sel | n;
}

static const uint8_t GB_RAM timer_shift[4] = {10, 4, 6, 8};  // TAC rate: 1024, 16, 64, 256 T-cycles

GB_INLINE void tima_inc(gb_t *g) {
    if (++g->io[0x05] == 0) {
        g->io[0x05] = g->io[0x06];
        IF |= IF_TIMER;
    }
}

// TIMA counts falling edges of one bit of the 16-bit divider.
GB_INLINE void timer_tick(gb_t *g, uint32_t t) {
    uint32_t old = g->div;
    g->div = (uint16_t)(old + t);
    uint8_t tac = g->io[0x07];
    if (!(tac & 4)) return;
    uint8_t s = timer_shift[tac & 3];
    uint32_t inc = ((old + t) >> s) - (old >> s);
    while (inc--) tima_inc(g);
}

// Runs the timer up to the current memory access within the instruction, so
// reads and writes of DIV/TIMA/TMA/TAC/IF see the right cycle.
GB_INLINE void timer_sync(gb_t *g) {
    timer_tick(g, g->acc_t - g->synced_t);
    g->synced_t = g->acc_t;
}

// Work RAM at C000-DFFF (also echoed at E000-FDFF): 4 KB bank 0, then the
// bank selected by SVBK (always 1 on the original Game Boy).
GB_INLINE uint8_t *wram_ptr(gb_t *g, uint16_t a) {
    uint16_t off = a & 0x1FFF;
    return off < 0x1000 ? g->wram + off : g->wram + g->wram_bank * 0x1000 + (off - 0x1000);
}

static uint8_t GB_FAST(read_slow)(gb_t *g, uint16_t a) {
    if (a < 0x8000) return read_rom_slow(g, a);
    if (a < 0xC000) {  // cart RAM / RTC
        if (g->mbc == 3 && g->ram_en && g->ram_bank >= 8) return g->ram_bank <= 0x0C ? g->rtc_latched[g->ram_bank - 8] : 0xFF;
        uint8_t *p = cart_ram_ptr(g, a);
        if (!p) return 0xFF;
        return g->mbc == 2 ? (*p | 0xF0) : *p;
    }
    if (a < 0xFE00) return *wram_ptr(g, a);
    if (a < 0xFEA0) return g->oam[a - 0xFE00];
    if (a < 0xFF00) return 0xFF;
    if (a >= 0xFF80) return a == 0xFFFF ? g->ie : g->hram[a - 0xFF80];
    uint8_t r = a & 0x7F;
    if ((r >= 0x04 && r <= 0x07) || r == 0x0F) timer_sync(g);
    if (g->cgb) switch (r) {
        case 0x4D: return 0x7E | (g->double_speed ? 0x80 : 0) | (g->key1 & 1);
        case 0x4F: return 0xFE | g->vram_bank;
        case 0x55: return g->hdma_active ? (g->hdma_len - 1) & 0x7F : 0xFF;
        case 0x68: return g->io[0x68] | 0x40;
        case 0x69: return g->bg_pal[g->io[0x68] & 0x3F];
        case 0x6A: return g->io[0x6A] | 0x40;
        case 0x6B: return g->obj_pal[g->io[0x6A] & 0x3F];
        case 0x70: return 0xF8 | g->wram_bank;
    }
    switch (r) {
    case 0x00: return joypad_read(g);
    case 0x01: case 0x05: case 0x06: return g->io[r];  // SB, TIMA, TMA
    case 0x02: return g->io[0x02] | 0x7E;
    case 0x04: return g->div >> 8;
    case 0x07: return g->io[0x07] | 0xF8;
    case 0x0F: return IF | 0xE0;
    case 0x41: return gb_ppu_read_stat(g);
    case 0x44: return g->ly;
    }
    if (r >= 0x10 && r < 0x40) return gb_apu_read(g, r);
    if (r >= 0x40 && r < 0x4C) return g->io[r];
    return 0xFF;
}

GB_INLINE uint8_t rd8_raw(gb_t *g, uint16_t a);

// Copies one 16-byte block from hdma_src to VRAM (current bank).
static void GB_FAST(hdma_block)(gb_t *g) {
    uint8_t *vram = g->vram + g->vram_bank * 0x2000;
    for (int i = 0; i < 16; i++) vram[(g->hdma_dst + i) & 0x1FFF] = rd8_raw(g, g->hdma_src + i);
    g->hdma_src += 16;
    g->hdma_dst = (g->hdma_dst + 16) & 0x1FF0;
}

void GB_FAST(gb_hdma_hblank)(gb_t *g) {
    if (!g->hdma_active) return;
    hdma_block(g);
    if (--g->hdma_len == 0) g->hdma_active = false;
}

static void GB_FAST(remap_cgb)(gb_t *g) {
    uint8_t *v = g->vram + g->vram_bank * 0x2000;
    g->rd[0x8] = g->wr[0x8] = v;
    g->rd[0x9] = g->wr[0x9] = v + 0x1000;
    g->rd[0xD] = g->wr[0xD] = g->wram + g->wram_bank * 0x1000;
}

// Color-only registers; returns false if r isn't one of them.
static bool GB_FAST(cgb_write)(gb_t *g, uint8_t r, uint8_t v) {
    switch (r) {
    case 0x4D: g->key1 = v & 1; return true;  // speed switch happens at STOP
    case 0x4F: g->vram_bank = v & 1; remap_cgb(g); return true;
    case 0x51: g->hdma_src = (g->hdma_src & 0x00FF) | v << 8; return true;
    case 0x52: g->hdma_src = (g->hdma_src & 0xFF00) | (v & 0xF0); return true;
    case 0x53: g->hdma_dst = (g->hdma_dst & 0x00FF) | (v & 0x1F) << 8; return true;
    case 0x54: g->hdma_dst = (g->hdma_dst & 0xFF00) | (v & 0xF0); return true;
    case 0x55: {
        if (g->hdma_active && !(v & 0x80)) { g->hdma_active = false; return true; }  // cancels an HBlank DMA
        int blocks = (v & 0x7F) + 1;
        if (v & 0x80) {
            g->hdma_active = true;
            g->hdma_len = blocks;
        } else {  // general DMA: all at once, the CPU waits
            for (int i = 0; i < blocks; i++) hdma_block(g);
            g->stall += blocks * (g->double_speed ? 64 : 32);
        }
        return true;
    }
    case 0x68: g->io[0x68] = v & 0xBF; return true;
    case 0x69: case 0x6B: {
        uint8_t *idx = &g->io[r == 0x69 ? 0x68 : 0x6A];
        (r == 0x69 ? g->bg_pal : g->obj_pal)[*idx & 0x3F] = v;
        if (*idx & 0x80) *idx = 0x80 | ((*idx + 1) & 0x3F);
        return true;
    }
    case 0x6A: g->io[0x6A] = v & 0xBF; return true;
    case 0x70: g->wram_bank = (v & 7) ? (v & 7) : 1; remap_cgb(g); return true;
    }
    return false;
}

static void GB_FAST(write_slow)(gb_t *g, uint16_t a, uint8_t v) {
    if (a < 0x8000) { mbc_write(g, a, v); return; }
    if (a < 0xC000) {  // cart RAM / RTC (VRAM never gets here)
        if (g->mbc == 3 && g->ram_en && g->ram_bank >= 8) {
            if (g->ram_bank <= 0x0C) g->rtc[g->ram_bank - 8] = v;
            return;
        }
        uint8_t *p = cart_ram_ptr(g, a);
        if (!p) return;
        *p = g->mbc == 2 ? (v & 0x0F) : v;
        g->ram_dirty = true;
        g->ram_write_frame = g->frame;
        g->ram_dirty_blocks |= 1ull << (((uint32_t)(p - g->cart_ram) >> 9) & 63);
        return;
    }
    if (a < 0xFE00) { *wram_ptr(g, a) = v; return; }
    if (a < 0xFEA0) { g->oam[a - 0xFE00] = v; return; }
    if (a < 0xFF00) return;
    if (a >= 0xFF80) {
        if (a == 0xFFFF) g->ie = v;
        else g->hram[a - 0xFF80] = v;
        return;
    }
    uint8_t r = a & 0x7F;
    if ((r >= 0x04 && r <= 0x07) || r == 0x0F) timer_sync(g);
    if (g->cgb && cgb_write(g, r, v)) return;
    switch (r) {
    case 0x00: g->io[0x00] = v & 0x30; return;
    case 0x01: g->io[0x01] = v; return;
    case 0x02:
        g->io[0x02] = v;
        if ((v & 0x81) == 0x81) {  // internal-clock transfer: deliver at once
            if (g->serial_out) g->serial_out(g->io[0x01]);
            g->io[0x01] = 0xFF;
            g->io[0x02] &= 0x7F;
            IF |= IF_SERIAL;
        }
        return;
    case 0x04: {  // resetting DIV can tick TIMA if the selected bit was high
        uint8_t tac = g->io[0x07];
        if ((tac & 4) && (g->div & (1u << (timer_shift[tac & 3] - 1)))) tima_inc(g);
        g->div = 0;
        return;
    }
    case 0x05: case 0x06: g->io[r] = v; return;
    case 0x07: g->io[0x07] = v & 7; return;
    case 0x0F: IF = v & 0x1F; return;
    }
    if (r >= 0x10 && r < 0x40) { gb_apu_write(g, r, v); return; }
    if (r == 0x46) {  // OAM DMA, done instantly
        uint16_t src = v << 8;
        for (int i = 0; i < 0xA0; i++) {
            uint16_t s = src + i;
            const uint8_t *p = g->rd[s >> 12];
            g->oam[i] = p ? p[s & 0xFFF] : read_slow(g, s);
        }
        g->io[0x46] = v;
        return;
    }
    if (r >= 0x40 && r < 0x4C) { gb_ppu_write(g, r, v); return; }
}

GB_INLINE uint8_t rd8_raw(gb_t *g, uint16_t a) {
    const uint8_t *p = g->rd[a >> 12];
    return p ? p[a & 0xFFF] : read_slow(g, a);
}

// CPU accesses: each takes one M-cycle, which elapses before the access.
GB_INLINE uint8_t rd8(gb_t *g, uint16_t a) {
    g->acc_t += 4;
    return rd8_raw(g, a);
}

GB_INLINE void wr8(gb_t *g, uint16_t a, uint8_t v) {
    g->acc_t += 4;
    uint8_t *p = g->wr[a >> 12];
    if (p) p[a & 0xFFF] = v;
    else write_slow(g, a, v);
}

GB_INLINE uint16_t rd16(gb_t *g, uint16_t a) { return rd8(g, a) | rd8(g, a + 1) << 8; }

// ---------------------------------------------------------------- setup

uint32_t gb_cart_ram_size(const uint8_t *rom) {
    uint8_t type = rom[0x147];
    if (type == 0x05 || type == 0x06) return 512;  // MBC2 built-in RAM (4-bit cells)
    static const uint32_t sizes[6] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    uint8_t s = rom[0x149];
    return s < 6 ? sizes[s] : 0;
}

void gb_title(const uint8_t *rom, char out[17]) {
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t c = rom[0x134 + i];
        if (c == 0 || (i >= 11 && (rom[0x143] & 0x80))) break;  // Color carts reuse the end of the title field
        out[n++] = (c >= 32 && c < 127) ? c : '?';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

void gb_set_bank0(gb_t *g, const uint8_t *bank0) {
    g->rom_bank0 = bank0;
    map_banks(g);
}

int gb_cart_info(const uint8_t *rom, uint32_t rom_size, uint8_t *mbc_out, bool *battery_out, bool *rtc_out,
                 uint32_t *ram_out) {
    if (rom_size < 0x8000 || (rom_size & 0x3FFF)) return GB_ERR_SIZE;
    uint8_t mbc, type = rom[0x147];
    bool battery = false, rtc = false;
    switch (type) {
    case 0x00: case 0x08: case 0x09: mbc = 0; battery = type == 0x09; break;
    case 0x01: case 0x02: case 0x03: mbc = 1; battery = type == 0x03; break;
    case 0x05: case 0x06: mbc = 2; battery = type == 0x06; break;
    case 0x0F: case 0x10: mbc = 3; battery = rtc = true; break;
    case 0x11: case 0x12: case 0x13: mbc = 3; battery = type == 0x13; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
        mbc = 5; battery = type == 0x1B || type == 0x1E; break;
    default: return GB_ERR_MAPPER;
    }
    uint32_t ram = gb_cart_ram_size(rom);
    if (ram > GB_MAX_CART_RAM) return GB_ERR_RAM;
    *mbc_out = mbc;
    *battery_out = battery;
    *rtc_out = rtc;
    *ram_out = ram;
    return GB_OK;
}

int gb_init(gb_t *g, const uint8_t *rom, uint32_t rom_size, uint8_t *cart_ram, uint8_t *fb) {
    uint8_t mbc;
    bool battery, rtc;
    uint32_t ram;
    int err = gb_cart_info(rom, rom_size, &mbc, &battery, &rtc, &ram);
    if (err) return err;

    void (*serial)(uint8_t) = g->serial_out;
    const uint8_t *(*page)(gb_t *, uint32_t, uint32_t, bool) = g->rom_page;
    uint16_t resident = g->rom_resident;
    memset(g, 0, sizeof *g);
    g->serial_out = serial;
    g->rom = g->rom_bank0 = rom;
    g->rom_size = rom_size;
    g->rom_banks = rom_size / 0x4000;
    g->rom_page = page;
    g->rom_resident = page && resident < g->rom_banks ? resident : g->rom_banks;
    g->cart_ram = cart_ram;
    g->ram_size = ram;
    g->mbc = mbc;
    g->has_battery = battery;
    g->has_rtc = rtc;
    g->rom_bank = 1;
    g->fb = fb;

    g->cgb = rom[0x143] & 0x80;  // supports (0x80) or needs (0xC0) the Game Boy Color
    g->wram_bank = 1;
    if (g->cgb) {
        // State after the CGB boot ROM. A = 0x11 is how games detect a Color.
        g->r[RA] = 0x11; g->f = 0x80;
        g->r[RB] = 0x00; g->r[RC] = 0x00; g->r[RD] = 0xFF; g->r[RE] = 0x56;
        g->r[RH] = 0x00; g->r[RL] = 0x0D;
        for (int i = 0; i < 64; i += 2) {  // palettes start out white
            g->bg_pal[i] = g->obj_pal[i] = 0xFF;
            g->bg_pal[i + 1] = g->obj_pal[i + 1] = 0x7F;
        }
    } else {
        // State after the DMG boot ROM.
        g->r[RA] = 0x01; g->f = 0xB0;
        g->r[RB] = 0x00; g->r[RC] = 0x13; g->r[RD] = 0x00; g->r[RE] = 0xD8;
        g->r[RH] = 0x01; g->r[RL] = 0x4D;
    }
    g->sp = 0xFFFE; g->pc = 0x0100;
    g->div = 0xABCC;
    g->io[0x00] = 0x30;
    g->io[0x40] = 0x91;
    g->io[0x47] = 0xFC; g->io[0x48] = 0xFF; g->io[0x49] = 0xFF;
    IF = 0x01;

    gb_remap(g);
    gb_ppu_reset(g);
    gb_apu_reset(g);
    return GB_OK;
}

void gb_remap(gb_t *g) {
    for (int i = 0; i < 16; i++) g->wr[i] = nullptr;
    g->rd[0x8] = g->vram; g->rd[0x9] = g->vram + 0x1000;
    g->wr[0x8] = g->vram; g->wr[0x9] = g->vram + 0x1000;
    g->rd[0xC] = g->wr[0xC] = g->wram;
    g->rd[0xD] = g->wr[0xD] = g->wram + 0x1000;
    g->rd[0xE] = g->wr[0xE] = g->wram;  // echo RAM
    g->rd[0xF] = nullptr;
    remap_cgb(g);  // VRAM and WRAM banks (bank 0 / 1 on the original Game Boy)
    map_banks(g);
}

void gb_set_buttons(gb_t *g, uint8_t pressed) {
    if (pressed & ~g->buttons) IF |= IF_JOYPAD;
    g->buttons = pressed;
}

// ---------------------------------------------------------------- CPU

#define HL ((uint16_t)(g->r[RH] << 8 | g->r[RL]))
#define SET_HL(v) do { uint16_t _v = (v); g->r[RH] = _v >> 8; g->r[RL] = _v; } while (0)
#define FLAG(c, bit) ((c) ? (bit) : 0)

GB_INLINE uint8_t get_r(gb_t *g, int i) { return i == R_HL ? rd8(g, HL) : g->r[i]; }
GB_INLINE void set_r(gb_t *g, int i, uint8_t v) {
    if (i == R_HL) wr8(g, HL, v);
    else g->r[i] = v;
}

GB_INLINE uint16_t get_rr(gb_t *g, int i) {  // BC DE HL SP
    return i == 3 ? g->sp : (uint16_t)(g->r[i * 2] << 8 | g->r[i * 2 + 1]);
}
GB_INLINE void set_rr(gb_t *g, int i, uint16_t v) {
    if (i == 3) g->sp = v;
    else { g->r[i * 2] = v >> 8; g->r[i * 2 + 1] = v; }
}

GB_INLINE void push(gb_t *g, uint16_t v) {
    g->sp -= 2;
    wr8(g, g->sp + 1, v >> 8);
    wr8(g, g->sp, v);
}
GB_INLINE uint16_t pop(gb_t *g) {
    uint16_t v = rd16(g, g->sp);
    g->sp += 2;
    return v;
}

GB_INLINE uint8_t fetch(gb_t *g) {
    uint8_t v = rd8(g, g->pc);
    if (g->halt_bug) g->halt_bug = false;  // HALT bug: the byte after HALT is read twice
    else g->pc++;
    return v;
}
GB_INLINE uint16_t fetch16(gb_t *g) {
    uint16_t v = rd16(g, g->pc);
    g->pc += 2;
    return v;
}

GB_INLINE void alu(gb_t *g, int op, uint8_t v) {
    uint8_t a = g->r[RA];
    int c = (g->f & FC) ? 1 : 0, res;
    switch (op) {
    case 0:  // ADD
        res = a + v;
        g->f = FLAG((res & 0xFF) == 0, FZ) | FLAG((a & 0xF) + (v & 0xF) > 0xF, FH) | FLAG(res > 0xFF, FC);
        g->r[RA] = res;
        break;
    case 1:  // ADC
        res = a + v + c;
        g->f = FLAG((res & 0xFF) == 0, FZ) | FLAG((a & 0xF) + (v & 0xF) + c > 0xF, FH) | FLAG(res > 0xFF, FC);
        g->r[RA] = res;
        break;
    case 2:  // SUB
    case 7:  // CP
        res = a - v;
        g->f = FN | FLAG((res & 0xFF) == 0, FZ) | FLAG((a & 0xF) < (v & 0xF), FH) | FLAG(res < 0, FC);
        if (op == 2) g->r[RA] = res;
        break;
    case 3:  // SBC
        res = a - v - c;
        g->f = FN | FLAG((res & 0xFF) == 0, FZ) | FLAG((a & 0xF) - (v & 0xF) - c < 0, FH) | FLAG(res < 0, FC);
        g->r[RA] = res;
        break;
    case 4: g->r[RA] = a & v; g->f = FLAG(g->r[RA] == 0, FZ) | FH; break;
    case 5: g->r[RA] = a ^ v; g->f = FLAG(g->r[RA] == 0, FZ); break;
    case 6: g->r[RA] = a | v; g->f = FLAG(g->r[RA] == 0, FZ); break;
    }
}

GB_INLINE uint8_t inc8(gb_t *g, uint8_t v) {
    v++;
    g->f = (g->f & FC) | FLAG(v == 0, FZ) | FLAG((v & 0xF) == 0, FH);
    return v;
}
GB_INLINE uint8_t dec8(gb_t *g, uint8_t v) {
    v--;
    g->f = (g->f & FC) | FN | FLAG(v == 0, FZ) | FLAG((v & 0xF) == 0xF, FH);
    return v;
}

GB_INLINE uint16_t add_sp(gb_t *g, int8_t e) {
    uint16_t sp = g->sp;
    uint8_t u = (uint8_t)e;
    g->f = FLAG((sp & 0xF) + (u & 0xF) > 0xF, FH) | FLAG((sp & 0xFF) + u > 0xFF, FC);
    return sp + e;
}

GB_INLINE uint8_t cb_op(gb_t *g, uint8_t op, uint8_t v) {
    int c = (g->f & FC) ? 1 : 0, out;
    switch (op >> 3) {
    case 0: out = v >> 7; v = v << 1 | out; break;            // RLC
    case 1: out = v & 1; v = v >> 1 | out << 7; break;        // RRC
    case 2: out = v >> 7; v = v << 1 | c; break;              // RL
    case 3: out = v & 1; v = v >> 1 | c << 7; break;          // RR
    case 4: out = v >> 7; v <<= 1; break;                     // SLA
    case 5: out = v & 1; v = (v >> 1) | (v & 0x80); break;    // SRA
    case 6: out = 0; v = v << 4 | v >> 4; break;              // SWAP
    default: out = v & 1; v >>= 1; break;                     // SRL
    }
    g->f = FLAG(v == 0, FZ) | FLAG(out, FC);
    return v;
}

GB_INLINE bool cond(gb_t *g, int cc) {
    switch (cc) {
    case 0: return !(g->f & FZ);
    case 1: return g->f & FZ;
    case 2: return !(g->f & FC);
    default: return g->f & FC;
    }
}

// Executes one instruction, returns M-cycles.
static int GB_FAST(step)(gb_t *g) {
    uint8_t op = fetch(g);

    if (op >= 0x40 && op < 0x80) {  // LD r, r'
        // HALT with IME off and an interrupt already pending doesn't halt;
        // instead the next byte gets fetched twice.
        if (op == 0x76) {
            uint8_t pending = g->ie & IF & 0x1F;
            if (!g->ime && pending) g->halt_bug = true;
            else g->halted = true;
            return 1;
        }
        int d = (op >> 3) & 7, s = op & 7;
        set_r(g, d, get_r(g, s));
        return (d == R_HL || s == R_HL) ? 2 : 1;
    }
    if (op >= 0x80 && op < 0xC0) {  // ALU A, r
        alu(g, (op >> 3) & 7, get_r(g, op & 7));
        return (op & 7) == R_HL ? 2 : 1;
    }

    switch (op) {
    case 0x00: return 1;
    case 0x10:
        g->pc++;
        if (g->cgb && (g->key1 & 1)) {  // STOP performs the speed switch armed through KEY1
            g->double_speed = !g->double_speed;
            g->key1 = 0;
            g->div = 0;
        }
        return 1;
    case 0x01: case 0x11: case 0x21: case 0x31: set_rr(g, op >> 4, fetch16(g)); return 3;
    case 0x02: wr8(g, get_rr(g, 0), g->r[RA]); return 2;
    case 0x12: wr8(g, get_rr(g, 1), g->r[RA]); return 2;
    case 0x22: wr8(g, HL, g->r[RA]); SET_HL(HL + 1); return 2;
    case 0x32: wr8(g, HL, g->r[RA]); SET_HL(HL - 1); return 2;
    case 0x0A: g->r[RA] = rd8(g, get_rr(g, 0)); return 2;
    case 0x1A: g->r[RA] = rd8(g, get_rr(g, 1)); return 2;
    case 0x2A: g->r[RA] = rd8(g, HL); SET_HL(HL + 1); return 2;
    case 0x3A: g->r[RA] = rd8(g, HL); SET_HL(HL - 1); return 2;
    case 0x03: case 0x13: case 0x23: case 0x33: set_rr(g, op >> 4, get_rr(g, op >> 4) + 1); return 2;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: set_rr(g, op >> 4, get_rr(g, op >> 4) - 1); return 2;
    case 0x09: case 0x19: case 0x29: case 0x39: {  // ADD HL,rr
        uint16_t hl = HL, v = get_rr(g, op >> 4);
        uint32_t res = hl + v;
        g->f = (g->f & FZ) | FLAG((hl & 0xFFF) + (v & 0xFFF) > 0xFFF, FH) | FLAG(res > 0xFFFF, FC);
        SET_HL(res);
        return 2;
    }
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
        int i = (op >> 3) & 7;
        set_r(g, i, inc8(g, get_r(g, i)));
        return i == R_HL ? 3 : 1;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int i = (op >> 3) & 7;
        set_r(g, i, dec8(g, get_r(g, i)));
        return i == R_HL ? 3 : 1;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E: {
        int i = (op >> 3) & 7;
        set_r(g, i, fetch(g));
        return i == R_HL ? 3 : 2;
    }
    // RLCA, RRCA, RLA, RRA: unlike the CB-prefixed rotates these always clear Z
    case 0x07: { uint8_t a = g->r[RA]; g->r[RA] = a << 1 | a >> 7; g->f = FLAG(a & 0x80, FC); return 1; }
    case 0x0F: { uint8_t a = g->r[RA]; g->r[RA] = a >> 1 | a << 7; g->f = FLAG(a & 1, FC); return 1; }
    case 0x17: { uint8_t a = g->r[RA]; g->r[RA] = a << 1 | ((g->f & FC) ? 1 : 0); g->f = FLAG(a & 0x80, FC); return 1; }
    case 0x1F: { uint8_t a = g->r[RA]; g->r[RA] = a >> 1 | ((g->f & FC) ? 0x80 : 0); g->f = FLAG(a & 1, FC); return 1; }
    case 0x08: { uint16_t a = fetch16(g); wr8(g, a, g->sp); wr8(g, a + 1, g->sp >> 8); return 5; }
    case 0x18: { int8_t e = fetch(g); g->pc += e; return 3; }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t e = fetch(g);
        if (!cond(g, (op >> 3) & 3)) return 2;
        g->pc += e;
        return 3;
    }
    case 0x27: {  // DAA
        uint8_t a = g->r[RA];
        bool c = g->f & FC;
        if (!(g->f & FN)) {
            if (c || a > 0x99) { a += 0x60; c = true; }
            if ((g->f & FH) || (a & 0x0F) > 9) a += 6;
        } else {
            if (c) a -= 0x60;
            if (g->f & FH) a -= 6;
        }
        g->r[RA] = a;
        g->f = (g->f & FN) | FLAG(a == 0, FZ) | FLAG(c, FC);
        return 1;
    }
    case 0x2F: g->r[RA] = ~g->r[RA]; g->f |= FN | FH; return 1;
    case 0x37: g->f = (g->f & FZ) | FC; return 1;
    case 0x3F: g->f = (g->f & (FZ | FC)) ^ FC; return 1;

    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        if (!cond(g, (op >> 3) & 3)) return 2;
        g->pc = pop(g);
        return 5;
    case 0xC9: g->pc = pop(g); return 4;
    case 0xD9: g->pc = pop(g); g->ime = true; return 4;
    case 0xC1: case 0xD1: case 0xE1: set_rr(g, (op >> 4) - 0xC, pop(g)); return 3;
    case 0xF1: { uint16_t v = pop(g); g->r[RA] = v >> 8; g->f = v & 0xF0; return 3; }
    // the acc_t += 4 before each push is the internal cycle that comes before the stack writes
    case 0xC5: case 0xD5: case 0xE5: g->acc_t += 4; push(g, get_rr(g, (op >> 4) - 0xC)); return 4;
    case 0xF5: g->acc_t += 4; push(g, g->r[RA] << 8 | g->f); return 4;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        uint16_t a = fetch16(g);
        if (!cond(g, (op >> 3) & 3)) return 3;
        g->pc = a;
        return 4;
    }
    case 0xC3: g->pc = fetch16(g); return 4;
    case 0xE9: g->pc = HL; return 1;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        uint16_t a = fetch16(g);
        if (!cond(g, (op >> 3) & 3)) return 3;
        g->acc_t += 4;
        push(g, g->pc);
        g->pc = a;
        return 6;
    }
    case 0xCD: { uint16_t a = fetch16(g); g->acc_t += 4; push(g, g->pc); g->pc = a; return 6; }
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        g->acc_t += 4;
        push(g, g->pc);
        g->pc = op & 0x38;
        return 4;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu(g, (op >> 3) & 7, fetch(g));
        return 2;
    case 0xE0: wr8(g, 0xFF00 | fetch(g), g->r[RA]); return 3;
    case 0xF0: g->r[RA] = rd8(g, 0xFF00 | fetch(g)); return 3;
    case 0xE2: wr8(g, 0xFF00 | g->r[RC], g->r[RA]); return 2;
    case 0xF2: g->r[RA] = rd8(g, 0xFF00 | g->r[RC]); return 2;
    case 0xEA: wr8(g, fetch16(g), g->r[RA]); return 4;
    case 0xFA: g->r[RA] = rd8(g, fetch16(g)); return 4;
    case 0xE8: g->sp = add_sp(g, (int8_t)fetch(g)); return 4;
    case 0xF8: SET_HL(add_sp(g, (int8_t)fetch(g))); return 3;
    case 0xF9: g->sp = HL; return 2;
    case 0xF3: g->ime = false; g->ei_delay = 0; return 1;
    case 0xFB: if (!g->ime) g->ei_delay = 2; return 1;  // EI only takes effect after the next instruction
    case 0xCB: {
        uint8_t cb = fetch(g);
        int i = cb & 7;
        uint8_t v = get_r(g, i);
        if (cb < 0x40) {
            set_r(g, i, cb_op(g, cb, v));
        } else if (cb < 0x80) {  // BIT; RES and SET follow
            g->f = (g->f & FC) | FH | FLAG(!(v & (1 << ((cb >> 3) & 7))), FZ);
            return i == R_HL ? 3 : 2;
        } else if (cb < 0xC0) {
            set_r(g, i, v & ~(1 << ((cb >> 3) & 7)));
        } else {
            set_r(g, i, v | (1 << ((cb >> 3) & 7)));
        }
        return i == R_HL ? 4 : 2;
    }
    default:  // D3 DB DD E3 E4 EB EC ED F4 FC FD lock up the real CPU
        g->pc--;
        return 1;
    }
}

// Everything that runs alongside the CPU. The PPU and APU are only called
// when their next event is due.
GB_INLINE void tick(gb_t *g, uint32_t t) {
    timer_tick(g, t - g->synced_t);
    g->acc_t = g->synced_t = 0;
    // double speed only speeds up the CPU and timer; the PPU, APU and frame
    // pacing keep counting normal-speed cycles
    if (g->double_speed) t >>= 1;
    g->dot += t;
    if (g->dot >= g->ppu_next) gb_ppu_event(g);
    g->apu.seq_timer -= t;
    g->apu.sample_timer -= t;
    if (g->apu.sample_timer <= 0 || g->apu.seq_timer <= 0) gb_apu_event(g);
    if (g->has_rtc) rtc_tick(g, t);
    g->cycles += t;
}

// HALT with nothing pending: nothing can change until the next PPU mode
// change, TIMA overflow or, with the LCD off, the end of the frame, so jump
// straight there. Spinning in HALT used to advance 16 T-cycles at a time and
// check for events after each step; the jump ends on that same step, and the
// APU is still advanced in those steps, so the outcome is identical.
GB_INLINE void halt_tick(gb_t *g) {
    uint32_t ds = g->double_speed;
    uint32_t dist = g->dot < g->ppu_next ? (uint32_t)(g->ppu_next - g->dot) << ds : 1;
    uint8_t tac = g->io[0x07];
    if (tac & 4) {
        uint32_t p = 1u << timer_shift[tac & 3];
        uint32_t ovf = p - (g->div & (p - 1)) + (255u - g->io[0x05]) * p;
        if (ovf < dist) dist = ovf;
    }
    if (!(g->io[0x40] & 0x80)) {
        uint32_t left = g->cycles < GB_FRAME_CYCLES ? (GB_FRAME_CYCLES - g->cycles) << ds : 1;
        if (left < dist) dist = left;
    }
    uint32_t t = (dist + 15) & ~15u;  // CPU T-cycles, in whole 16-cycle steps
    timer_tick(g, t);

    uint32_t qs = 4 - ds, q = 1u << qs, n = t >> ds;  // the same in PPU/APU cycles
    g->dot += n;
    if (g->dot >= g->ppu_next) gb_ppu_event(g);
    for (uint32_t left = n; left;) {
        int32_t due = g->apu.seq_timer < g->apu.sample_timer ? g->apu.seq_timer : g->apu.sample_timer;
        uint32_t k = due <= 0 ? q : (((uint32_t)due + q - 1) >> qs) << qs;  // to the end of the step it fires in
        if (k > left) k = left;
        g->apu.seq_timer -= k;
        g->apu.sample_timer -= k;
        left -= k;
        if (g->apu.sample_timer <= 0 || g->apu.seq_timer <= 0) gb_apu_event(g);
    }
    if (g->has_rtc) rtc_tick(g, n);
    g->cycles += n;
}

GB_INLINE int interrupts(gb_t *g) {
    uint8_t pending = g->ie & IF & 0x1F;
    if (!pending) return 0;
    g->halted = false;
    if (!g->ime) return 0;
    int bit = __builtin_ctz(pending);
    IF &= ~(1 << bit);
    g->ime = false;
    g->acc_t += 8;  // two internal cycles before the PC is pushed
    push(g, g->pc);
    g->pc = 0x40 + bit * 8;
    return 5;
}

void GB_FAST(gb_run_frame)(gb_t *g) {
    uint32_t frame = g->frame;
    g->cycles = 0;
    g->audio_n = 0;
    while (g->frame == frame) {
        int m = interrupts(g);
        if (!m && g->halted) {
            halt_tick(g);
        } else {
            if (!m) {
                m = step(g);
                if (g->ei_delay && --g->ei_delay == 0) g->ime = true;
            }
            tick(g, m * 4);
            if (g->stall) { uint32_t s = g->stall; g->stall = 0; tick(g, s); }
        }
        if (g->cycles >= GB_FRAME_CYCLES && !(g->io[0x40] & 0x80)) g->frame++;  // the LCD is off, so end the frame by cycle count
    }
}
