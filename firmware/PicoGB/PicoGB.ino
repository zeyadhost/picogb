// PicoGB - a Game Boy emulator for the Hack Club Sprig (RP2040).
//
//   core 0: the emulator (gb*.cpp), menus, storage, USB loader protocol,
//           audio out through the Sprig's MAX98357A (I2S, paces emulation)
//   core 1: display. Scales 160x144 to the 160x128 panel by blending rows,
//           rotates it for the portrait panel and DMAs it out in strips.
//
// Storage: the microSD card holds the game library, saves, save states and
// screenshots (sdcard.h). A game is copied from the card into flash when it
// is launched, like plugging in a cartridge, and runs from there; flash keeps
// the most recently played games so they start instantly (library.h).
// Without a card, games uploaded over USB are kept in flash.
//
// Controls: WASD = D-pad, L = A, J = B, I = Start, K = Select.
// Hold Start + Select (I + K) for half a second for the pause menu.
#include "gb.h"
#include "sprig_hw.h"
#include "library.h"
#include "sdcard.h"
#include "link.h"
#include "font.h"
#include <I2S.h>
#include <hardware/watchdog.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define UI_W 160
#define UI_H 128

static gb_t gb;
static uint8_t fbs[2][GB_W * GB_H] __attribute__((aligned(4)));
static uint8_t back;                    // frame buffer the emulator draws into
static const uint8_t *last_frame = fbs[1];
static uint8_t cart_ram[GB_MAX_CART_RAM];
static uint8_t bank0[0x4000];           // ROM bank 0, copied from flash into RAM
static uint16_t dpal[2][64];            // Color mode: each frame buffer's palette as RGB565
// The menu canvas (colour indexes) borrows a frame buffer: menus and the
// emulator never run at the same time.
static uint8_t *ui = fbs[0];
static_assert(UI_W * UI_H <= GB_W * GB_H, "menu canvas must fit a frame buffer");
// Shared by loader replies and SD-to-flash copies, which never overlap.
static uint8_t scratch[6144];
#define iobuf scratch

enum { ST_MENU, ST_GAME, ST_PAUSE };
static int state = ST_MENU;
static bool use_sd;                     // card mounted: library, saves and states live there
static sd_game_t games[SD_MAX_GAMES];
static int ngames;
static sd_game_t cur;                   // the game being played
static const lib_entry_t *playing;
static int playing_index = -1;
static uint8_t pc_keys;                 // held keys sent by the loader
static uint16_t emu_us;
static I2S i2s(OUTPUT);
#define FRAME_US 16743      // one Game Boy frame: 70224 cycles at 4.194304 MHz
#define AUDIO_TARGET 1200   // samples kept queued, about 37 ms of slack for SD card stalls
static bool audio_on;

// Sound runs only while a game does. Outside a game the I2S clock is stopped,
// which makes the MAX98357A mute itself. Left running, its DMA would loop the
// last two buffers as a loud buzz whenever a flash write holds off interrupts.
static void audio_run(bool on) {
    if (on == audio_on) return;
    audio_on = on;
    if (on) {
        i2s.setBCLK(10);  // LRCLK is BCLK + 1 = GP11, DIN GP9: the Sprig's MAX98357A
        i2s.setDATA(9);
        i2s.setBitsPerSample(16);
        i2s.setBuffers(10, 256);  // about 60 ms of slack for SD card write stalls
        i2s.begin(GB_AUDIO_RATE);
    } else {
        i2s.end();
    }
}

// Performance counters for the running game, read with the loader's 'A' command.
static struct {
    uint32_t frames, underruns, slow_frames, max_emu_us, max_frame_us, sum_emu_us, stalls, max_stall_ms, dropped;
    uint32_t page_loads, max_load_us, sum_load_us;  // big games: 4 KB ROM pages read from the card
    uint32_t ppu0, apu0;  // gb_prof_* when the counters were reset
} perf;

// ---------------------------------------------------------------- crash reports
// A hard fault records where it happened and reboots, and a 3 s watchdog
// catches freezes. The details survive the reboot in the watchdog scratch
// registers; they are shown in the menu, sent in the 'I' reply and appended
// to log.txt on the card.
#define FAULT_MAGIC 0x6B0DFA17u
#define STAGE(n) (watchdog_hw->scratch[1] = (n))  // what the main loop is doing, for freeze reports
enum { STG_MENU = 1, STG_EMU, STG_PUBLISH, STG_MIRROR, STG_AUDIO, STG_SAVE, STG_LINK, STG_COPY, STG_STATE };
static uint32_t last_reset[3];  // kind (0 none, 1 crash, 2 froze), pc or stage, lr

extern "C" void __attribute__((used)) fault_record(uint32_t *frame) {
    watchdog_hw->scratch[0] = FAULT_MAGIC;
    watchdog_hw->scratch[1] = frame[6];  // stacked PC
    watchdog_hw->scratch[2] = frame[5];  // stacked LR
    watchdog_reboot(0, 0, 10);
    for (;;) {}
}

extern "C" __attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"   // which stack was in use when the fault hit?
        "beq 1f\n"
        "mrs r0, psp\n"
        "b 2f\n"
        "1: mrs r0, msp\n"
        "2: ldr r1, =fault_record\n"
        "bx r1\n"
        ".ltorg\n");
}

static void check_last_reset() {
    if (watchdog_hw->scratch[0] == FAULT_MAGIC) {
        last_reset[0] = 1;
        last_reset[1] = watchdog_hw->scratch[1];
        last_reset[2] = watchdog_hw->scratch[2];
    } else if (watchdog_enable_caused_reboot()) {
        last_reset[0] = 2;
        last_reset[1] = watchdog_hw->scratch[1];
        last_reset[2] = watchdog_hw->scratch[2];
    }
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = 0;
}

static void log_line(const char *fmt, ...) {
    if (!use_sd) return;
    char line[120];
    int n = snprintf(line, sizeof line, "[%lu s] ", (unsigned long)(millis() / 1000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    sd_append_line("/PicoGB/log.txt", line);
}

// ---------------------------------------------------------------- colours

typedef struct { const char *name; uint32_t c[4]; } palette_t;
static const palette_t palettes[] = {
    // Original Game Boy games have four shades and no colours. "White" looks
    // like a Game Boy Pocket, "Classic" like the first green-screen Game Boy,
    // and the rest are tints like the ones a Game Boy Color offers old games.
    {"White",   {0xF8F8F8, 0xA8A8A8, 0x545454, 0x000000}},
    {"Classic", {0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F}},
    {"Blue",    {0xE8F0FF, 0x88A8E8, 0x3050A8, 0x081838}},
    {"Red",     {0xFFF0E8, 0xF09880, 0xA83838, 0x380808}},
    {"Green",   {0xE8FFE0, 0x90D880, 0x388840, 0x082810}},
    {"Yellow",  {0xFFF8D8, 0xE8C858, 0x987820, 0x302000}},
    {"Sunset",  {0xF8E8C8, 0xE09050, 0xA02840, 0x281848}},
    {"Ice",     {0xE8F8F8, 0x88C0D8, 0x4070A8, 0x102040}},
};
#define NUM_PALETTES (int)(sizeof palettes / sizeof palettes[0])
static int palette_i, volume = 3;  // volume 0 = mute
static int default_palette;        // for games without their own choice
static bool game_palette_changed;  // the running game's choice needs saving

enum { C_BG = 4, C_BAR, C_TEXT, C_DIM, C_ACCENT, C_SEL, C_WARN, C_HEAD };
static const uint32_t ui_colors[8] = {0x0E141C, 0x1C2C3C, 0xE8F0F0, 0x8494A4, 0x60D080, 0x2C5A84, 0xF06050, 0xF0C850};

static uint16_t pal[16];                // RGB565: 0..3 game shades, 4.. menu colours
static volatile uint16_t blend[4 * 4 * 8];  // [shade a][shade b][eighths of b] -> RGB565

static uint16_t rgb565(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F); }

static void set_palette(int i) {
    palette_i = (i + NUM_PALETTES) % NUM_PALETTES;
    const uint32_t *c = palettes[palette_i].c;
    for (int k = 0; k < 4; k++) pal[k] = rgb565(c[k]);
    for (int k = 0; k < 8; k++) pal[4 + k] = rgb565(ui_colors[k]);
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++)
            for (int f = 0; f < 8; f++) {
                uint32_t ca = c[a], cb = c[b], out = 0;
                for (int s = 0; s < 24; s += 8) {
                    uint32_t va = (ca >> s) & 0xFF, vb = (cb >> s) & 0xFF;
                    out |= ((va * (8 - f) + vb * f) / 8) << s;
                }
                blend[a << 5 | b << 3 | f] = rgb565(out);
            }
}

// ---------------------------------------------------------------- settings

static char last_played[SD_NAME_LEN];
static bool settings_dirty;
static uint32_t settings_changed_at;

static void settings_load() {
    char buf[160];
    int n = sd_load("/PicoGB/settings.txt", buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
        if (!strncmp(line, "colors=", 7)) { default_palette = constrain(atoi(line + 7), 0, NUM_PALETTES - 1); set_palette(default_palette); }
        else if (!strncmp(line, "vol=", 4)) volume = constrain(atoi(line + 4), 0, 4);
        else if (!strncmp(line, "last=", 5)) strncpy(last_played, line + 5, SD_NAME_LEN - 1);
    }
}

static void settings_save() {
    settings_dirty = false;
    if (!use_sd) return;
    char buf[160];
    int n = snprintf(buf, sizeof buf, "colors=%d\r\nvol=%d\r\nlast=%s\r\n", default_palette, volume, last_played);
    sd_store("/PicoGB/settings.txt", buf, n);
}

static void settings_changed() {
    settings_dirty = true;
    settings_changed_at = millis();
}

static const char *volume_name() {
    static char b[8];
    if (!volume) return "Mute";
    snprintf(b, sizeof b, "%d/4", volume);
    return b;
}

// ---------------------------------------------------------------- display (core 1)

enum { SRC_GAME, SRC_UI, SRC_CGB };
static volatile const uint16_t *disp_pal;  // SRC_CGB: the frame's 64 colours
static volatile const uint8_t *disp_src;
static volatile uint8_t disp_kind;
static volatile bool disp_pending;

// Hands a finished picture to core 1. Returns false if it is still busy with the last one.
static bool publish(const uint8_t *src, uint8_t kind, const uint16_t *cpal = nullptr) {
    if (disp_pending) return false;
    disp_pal = cpal;
    disp_src = src;
    disp_kind = kind;
    __dmb();
    disp_pending = true;
    return true;
}

#define STRIP 16
static uint16_t strip[2][LCD_COLS * STRIP];
static uint8_t row_src[UI_H], row_frac[UI_H];  // view row -> source row and blend weight

// The bus is held strip by strip: if core 0 is waiting to read the SD card
// (a big game loading a ROM page mid-frame), it gets the bus between strips
// instead of after the whole 15 ms frame.
static void GB_FAST(show)(const uint8_t *src, uint8_t kind) {
    sprig_bus_take(BUS_LCD);
    for (int s = 0; s < LCD_ROWS / STRIP; s++) {
        uint16_t *out = strip[s & 1];
        for (int r = 0; r < STRIP; r++) {
            int x = s * STRIP + r;  // panel row = view column
            uint16_t *o = out + r * LCD_COLS;
            if (kind == SRC_GAME) {
                for (int c = 0; c < LCD_COLS; c++) {
                    int y = UI_H - 1 - c;
                    const uint8_t *p = src + row_src[y] * GB_W + x;
                    o[c] = blend[(p[0] & 3) << 5 | (p[GB_W] & 3) << 3 | row_frac[y]];
                }
            } else if (kind == SRC_CGB) {
                const uint16_t *cp = (const uint16_t *)disp_pal;
                for (int c = 0; c < LCD_COLS; c++) {
                    int y = UI_H - 1 - c, f = row_frac[y];
                    const uint8_t *p = src + row_src[y] * GB_W + x;
                    uint32_t a = cp[p[0]];
                    if (f) {  // blend with the row below, all three channels at once
                        uint32_t b = cp[p[GB_W]];
                        a = (a | a << 16) & 0x07E0F81F;
                        b = (b | b << 16) & 0x07E0F81F;
                        a = ((a * (8 - f) + b * f) >> 3) & 0x07E0F81F;
                        a |= a >> 16;
                    }
                    o[c] = a;
                }
            } else {
                for (int c = 0; c < LCD_COLS; c++) o[c] = pal[src[(UI_H - 1 - c) * UI_W + x]];
            }
        }
        if (s == LCD_ROWS / STRIP - 1) disp_pending = false;  // source no longer needed
        while (sprig_lcd_busy()) tight_loop_contents();
        if (sprig_bus_wanted()) {
            sprig_bus_give(BUS_LCD);
            while (sprig_bus_wanted()) tight_loop_contents();
            sprig_bus_take(BUS_LCD);
        }
        sprig_lcd_rows(s * STRIP, STRIP, out);
    }
    sprig_bus_give(BUS_LCD);  // waits for the last strip to finish
}

void setup1() {
    for (int y = 0; y < UI_H; y++) {
        int pos = y * 9;  // 144 / 128 = 9 / 8
        row_src[y] = pos >> 3;
        row_frac[y] = pos & 7;
        if (row_src[y] >= GB_H - 1) { row_src[y] = GB_H - 2; row_frac[y] = 7; }
    }
}

void loop1() {
    if (!disp_pending) { tight_loop_contents(); return; }
    show((const uint8_t *)disp_src, disp_kind);
}

// ---------------------------------------------------------------- drawing

static void ui_rect(int x, int y, int w, int h, uint8_t c) {
    for (int j = y; j < y + h; j++)
        if (j >= 0 && j < UI_H)
            for (int i = x; i < x + w; i++)
                if (i >= 0 && i < UI_W) ui[j * UI_W + i] = c;
}

static int ui_text(int x, int y, const char *s, uint8_t c, int max_chars = 99) {
    for (; *s && max_chars-- > 0; s++, x += FONT_W + 1) {
        uint8_t ch = (uint8_t)*s;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const uint8_t *g = font5x8[ch - FONT_FIRST];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++)
                if ((g[j] >> (FONT_W - 1 - i)) & 1) {
                    int px = x + i, py = y + j;
                    if (px >= 0 && px < UI_W && py >= 0 && py < UI_H) ui[py * UI_W + px] = c;
                }
    }
    return x;
}

static void ui_center(int y, const char *s, uint8_t c) {
    int w = (int)strlen(s) * (FONT_W + 1);
    if (w > UI_W) w = UI_W;
    ui_text((UI_W - w) / 2, y, s, c, UI_W / (FONT_W + 1));
}

static void ui_right(int y, const char *s, uint8_t c) { ui_text(UI_W - 4 - (int)strlen(s) * (FONT_W + 1), y, s, c); }

static void ui_bar(int y, uint32_t done, uint32_t total) {
    ui_rect(20, y, UI_W - 40, 6, C_BAR);
    ui_rect(20, y, total ? (int)((UI_W - 40) * (uint64_t)done / total) : 0, 6, C_ACCENT);
}

// Call before drawing a menu screen. Core 1 reads a picture out to the LCD a
// strip at a time, left to right, over about 17 ms, and drawing into that
// buffer meanwhile made the right side flicker. In the main menu both frame
// buffers are free, so draw into the one not on screen. In the pause menu one
// of them holds the last game frame for screenshots, so wait for core 1.
static void ui_begin() {
    if (state == ST_MENU) ui = (const uint8_t *)disp_src == fbs[0] ? fbs[1] : fbs[0];
    while (disp_pending && (const uint8_t *)disp_src == ui) tight_loop_contents();
}

static void ui_show() {
    while (!publish(ui, SRC_UI)) tight_loop_contents();
}

static char message[64];
static uint32_t message_until;

static void set_message(const char *m) {
    strncpy(message, m, sizeof message - 1);
    message_until = millis() + 3000;
}

// Full-screen notice with an optional progress bar, e.g. while copying a game.
static void draw_progress(const char *title, const char *what, uint32_t done, uint32_t total) {
    ui_begin();
    ui_rect(0, 0, UI_W, UI_H, C_BG);
    ui_center(36, title, C_TEXT);
    ui_center(52, what, C_DIM);
    if (total) {
        ui_bar(68, done, total);
        char b[32];
        snprintf(b, sizeof b, "%lu / %lu KB", (unsigned long)(done / 1024), (unsigned long)(total / 1024));
        ui_center(80, b, C_DIM);
    }
    ui_show();
}

// ---------------------------------------------------------------- catalog

static void refresh_catalog() {
    if (use_sd) {
        ngames = sd_scan(games, SD_MAX_GAMES);
        return;
    }
    ngames = 0;
    for (int i = 0; i < lib_count() && ngames < SD_MAX_GAMES; i++) {
        const lib_entry_t *e = lib_get(i);
        sd_game_t *g = &games[ngames++];
        memset(g, 0, sizeof *g);
        strncpy(g->name, e->name, SD_NAME_LEN - 1);
        memcpy(g->title, e->title, 17);
        strcpy(g->ext, ".gb");
        g->size = e->rom_len;
        g->stamp = e->stamp;
        g->gcsum = e->gcsum;
        g->has_save = e->save_len != 0;
    }
}

static int find_game(const char *name) {
    for (int i = 0; i < ngames; i++)
        if (!strcmp(games[i].name, name)) return i;
    return -1;
}

static int cached_index(const sd_game_t *g) { return lib_find(g->name, g->size, g->stamp, g->gcsum); }

static const char *cart_error(int err) {
    switch (err) {
    case GB_OK: return nullptr;
    case GB_ERR_CGB_ONLY: return "Game Boy Color only";
    case GB_ERR_MAPPER: return "unsupported cartridge";
    case GB_ERR_RAM: return "save RAM over 32 KB";
    default: return "not a Game Boy ROM";
    }
}

// ---------------------------------------------------------------- input

static uint8_t read_keys() {
    uint8_t h = sprig_buttons_held(), k = pc_keys;
    if (h & 1 << BTN_W) k |= GB_UP;
    if (h & 1 << BTN_A) k |= GB_LEFT;
    if (h & 1 << BTN_S) k |= GB_DOWN;
    if (h & 1 << BTN_D) k |= GB_RIGHT;
    if (h & 1 << BTN_L) k |= GB_A;
    if (h & 1 << BTN_J) k |= GB_B;
    if (h & 1 << BTN_I) k |= GB_START;
    if (h & 1 << BTN_K) k |= GB_SELECT;
    return k;
}

// Newly pressed keys, with auto-repeat for the directions.
static uint8_t key_presses(uint8_t k) {
    static uint8_t last;
    static uint32_t repeat_at;
    const uint8_t dirs = GB_UP | GB_DOWN | GB_LEFT | GB_RIGHT;
    uint32_t now = millis();
    uint8_t pressed = k & ~last;
    if (pressed & dirs) repeat_at = now + 400;
    else if ((k & dirs) && now >= repeat_at) { pressed |= k & dirs; repeat_at = now + 120; }
    last = k;
    return pressed;
}

// ---------------------------------------------------------------- saves and states

static void game_path(char *out, const char *dir, const char *ext) { sd_path(out, dir, cur.name, ext); }

static bool save_file_open;  // the game's .sav on the card is open for block writes

// Writes up to `max_blocks` changed 512-byte blocks of battery RAM to the
// card in place, or everything pending if max_blocks < 0. Many games, Pokemon
// among them, also use save RAM as scratch space, so a few blocks per frame
// keeps a full 32 KB rewrite from freezing the game.
static void save_battery_blocks(int max_blocks) {
    if (!save_file_open) return;
    STAGE(STG_SAVE);
    bool wrote = false;
    while (gb.ram_dirty_blocks && max_blocks--) {
        int b = __builtin_ctzll(gb.ram_dirty_blocks);
        gb.ram_dirty_blocks &= ~(1ull << b);
        uint32_t off = b * 512u;
        if (off >= gb.ram_size) continue;
        uint32_t n = gb.ram_size - off < 512 ? gb.ram_size - off : 512;
        if (!sd_save_block(off, cart_ram + off, n)) { log_line("save write failed: %s", cur.name); break; }
        wrote = true;
    }
    if (wrote && !gb.ram_dirty_blocks) sd_save_sync();
}

// Writes everything pending right away (pause, quit, restart).
static void save_battery() {
    if (state == ST_MENU) return;
    if (use_sd) {
        save_battery_blocks(-1);
        gb.ram_dirty = false;
        return;
    }
    if (!gb.ram_dirty) return;
    gb.ram_dirty = false;
    if (gb.has_battery && gb.ram_size) lib_save_write(playing, cart_ram, gb.ram_size);
}

// ---------------------------------------------------------------- big games

// Banks that don't fit in flash are read from the card in 4 KB pages, into RAM
// the running game leaves free: the bank 0 copy (bank 0 is then read from
// flash), cart RAM beyond the game's own, and on the original Game Boy work
// RAM banks 2-7, which only the Color has.
#define MAX_PAGES 18
#define NO_BANK 0xFFFF
static struct { uint8_t *buf; uint16_t bank; uint8_t page; uint32_t last; } pages[MAX_PAGES];
static int npages;
static uint32_t page_clock;

static const uint8_t *GB_FAST(rom_page)(gb_t *, uint32_t bank, uint32_t page, bool load) {
    for (int i = 0; i < npages; i++)
        if (pages[i].bank == bank && pages[i].page == page) {
            pages[i].last = ++page_clock;
            return pages[i].buf;
        }
    if (!load || !npages) return nullptr;
    int v = 0;  // least recently used
    for (int i = 1; i < npages; i++)
        if (pages[i].last < pages[v].last) v = i;
    pages[v].bank = NO_BANK;
    uint32_t t0 = time_us_32();
    bool ok = sd_rom_read(bank * 0x4000 + page * 0x1000, pages[v].buf, 0x1000);
    uint32_t us = time_us_32() - t0;
    perf.sum_load_us += us;
    if (us > perf.max_load_us) perf.max_load_us = us;
    if (!ok) return nullptr;
    pages[v].bank = bank;
    pages[v].page = page;
    pages[v].last = ++page_clock;
    perf.page_loads++;
    return pages[v].buf;
}

static void pages_forget() {
    for (int i = 0; i < npages; i++) pages[i].bank = NO_BANK;
}

static void pages_add(uint8_t *p, uint32_t len) {
    for (uint32_t o = 0; o + 0x1000 <= len && npages < MAX_PAGES; o += 0x1000) {
        pages[npages].buf = p + o;
        pages[npages++].bank = NO_BANK;
    }
}

// Starts (or restarts) the game in `e`; cart RAM must already hold its save.
static int start_rom(const lib_entry_t *e) {
    const uint8_t *rom = lib_rom(e);
    bool big = e->stored_len < e->rom_len;
    npages = 0;  // nothing may be found in the old pages while gb_init maps the banks
    gb.rom_page = big ? rom_page : nullptr;
    gb.rom_resident = e->stored_len / 0x4000;
    int r = gb_init(&gb, rom, e->rom_len, cart_ram, fbs[back]);
    if (r) return r;
    if (big) {
        uint32_t own = (gb.ram_size + 0xFFF) & ~0xFFFu;
        pages_add(bank0, sizeof bank0);
        pages_add(cart_ram + own, sizeof cart_ram - own);
        if (!gb.cgb) pages_add(gb.wram + 0x2000, sizeof gb.wram - 0x2000);
    } else {
        memcpy(bank0, rom, sizeof bank0);
        gb_set_bank0(&gb, bank0);
    }
    return GB_OK;
}

static bool save_state(const char *path) {
    gb_state_hdr_t h = {GB_STATE_MAGIC, GB_STATE_VERSION, sizeof(gb_t), gb.ram_size, {0}, {0}, gb.frame};
    gb_title(gb.rom, h.title);
    STAGE(STG_STATE);
    if (!sd_write_open(path)) return false;
    bool ok = sd_write(&h, sizeof h) && sd_write(&gb, sizeof gb) && (!gb.ram_size || sd_write(cart_ram, gb.ram_size));
    if (!ok) { sd_write_abort(); return false; }
    return sd_write_commit();
}

// Loads a state into the running game. On a header mismatch nothing changes;
// on a short read the machine is left broken and the caller must restart it.
enum { LOAD_OK, LOAD_NONE, LOAD_BROKEN };
static int load_state(const char *path) {
    uint32_t size;
    STAGE(STG_STATE);
    if (!sd_read_open(path, &size)) return LOAD_NONE;
    gb_state_hdr_t h;
    char title[17];
    gb_title(gb.rom, title);
    bool known = sd_read(&h, sizeof h) == (int)sizeof h && h.magic == GB_STATE_MAGIC &&
                 ((h.version == GB_STATE_VERSION && h.gb_size == sizeof(gb_t)) ||
                  (h.version == 4 && h.gb_size == GB_STATE_V4_SIZE));
    if (!known || h.ram_size != gb.ram_size || strcmp(h.title, title) || size != sizeof h + h.gb_size + h.ram_size) {
        sd_read_close();
        return LOAD_NONE;
    }
    const uint8_t *rom = gb.rom, *b0 = gb.rom_bank0;
    uint8_t *ram = gb.cart_ram, *fb = gb.fb;
    void (*so)(uint8_t) = gb.serial_out;
    const uint8_t *(*page)(gb_t *, uint32_t, uint32_t, bool) = gb.rom_page;
    uint16_t resident = gb.rom_resident;
    bool ok = sd_read(&gb, h.gb_size) == (int)h.gb_size && (!h.ram_size || sd_read(cart_ram, h.ram_size) == (int)h.ram_size);
    sd_read_close();
    gb.rom = rom; gb.rom_bank0 = b0; gb.cart_ram = ram; gb.fb = fb; gb.serial_out = so;
    gb.rom_page = page; gb.rom_resident = resident;
    pages_forget();  // the state may have overwritten the work RAM pages
    gb_remap(&gb);
    return ok ? LOAD_OK : LOAD_BROKEN;
}

static bool screenshot() {
    static int next = 1;
    char p[96], suffix[12];
    for (; next < 1000; next++) {
        snprintf(suffix, sizeof suffix, "-%03d.bmp", next);
        sd_path(p, "shots", cur.name, suffix);
        if (!sd_exists(p)) break;
    }
    if (next >= 1000 || !sd_write_open(p)) return false;
    // 8-bit indexed BMP, rows bottom-up: 4 colours, or 64 for a Color frame.
    int ncolors = gb.cgb ? 64 : 4;
    uint8_t h[54 + 64 * 4];
    uint32_t hlen = 54 + ncolors * 4;
    memset(h, 0, sizeof h);
    auto put32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
    h[0] = 'B'; h[1] = 'M';
    put32(2, hlen + GB_W * GB_H);
    put32(10, hlen);
    put32(14, 40);
    put32(18, GB_W);
    put32(22, GB_H);
    h[26] = 1; h[28] = 8;
    put32(34, GB_W * GB_H);
    put32(46, ncolors);
    for (int i = 0; i < ncolors; i++) {
        uint32_t rgb = palettes[palette_i].c[i & 3];  // 0x00RRGGBB is BGRX in memory, as BMP wants
        if (gb.cgb) {
            uint16_t c = gb.frame_pal[2 * i] | gb.frame_pal[2 * i + 1] << 8;
            uint32_t r = c & 31, g5 = (c >> 5) & 31, b = (c >> 10) & 31;
            rgb = (r << 3 | r >> 2) << 16 | (g5 << 3 | g5 >> 2) << 8 | (b << 3 | b >> 2);
        }
        put32(54 + 4 * i, rgb);
    }
    bool ok = sd_write(h, hlen);
    for (int y = GB_H - 1; y >= 0 && ok; y--) ok = sd_write(last_frame + y * GB_W, GB_W);
    if (!ok) { sd_write_abort(); return false; }
    next++;
    return sd_write_commit();
}

// ---------------------------------------------------------------- launching

static void quit_game() {
    if (state == ST_MENU) return;
    save_battery();
    if (use_sd) {
        char p[96];
        game_path(p, "states", ".res");
        if (!save_state(p)) log_line("resume save failed: %s", cur.name);
    }
    if (save_file_open) { sd_save_close(); save_file_open = false; }
    if (gb.rom_page) { sd_rom_close(); gb.rom_page = nullptr; npages = 0; }
    save_game_palette();
    set_palette(default_palette);
    state = ST_MENU;
    playing = nullptr;
    playing_index = -1;
    int i = find_game(cur.name);
    if (i >= 0) games[i].has_save = cur.has_save;
}

// Case-insensitive whole-word match, e.g. "Blue" in "Pokemon Blue".
static bool has_word(const char *s, const char *w) {
    int n = strlen(w);
    for (const char *p = s; *p; p++) {
        if (strncasecmp(p, w, n)) continue;
        bool before = p == s || !isalpha((unsigned char)p[-1]), after = !isalpha((unsigned char)p[n]);
        if (before && after) return true;
    }
    return false;
}

// The game's saved colours, else a tint its name or title suggests, else the default.
static int game_palette(const sd_game_t *g) {
    char p[96], buf[32];
    sd_path(p, "saves", g->name, ".cfg");
    int n = sd_load(p, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        if (!strncmp(buf, "colors=", 7)) return constrain(atoi(buf + 7), 0, NUM_PALETTES - 1);
    }
    static const char *tints[] = {"Blue", "Red", "Green", "Yellow"};
    for (const char *t : tints)
        if (has_word(g->name, t) || has_word(g->title, t))
            for (int i = 0; i < NUM_PALETTES; i++)
                if (!strcmp(palettes[i].name, t)) return i;
    return default_palette;
}

static void save_game_palette() {
    if (!game_palette_changed || !use_sd) return;
    game_palette_changed = false;
    char p[96], buf[16];
    game_path(p, "saves", ".cfg");
    int n = snprintf(buf, sizeof buf, "colors=%d\r\n", palette_i);
    sd_store(p, buf, n);
}

// Copies a game from the card into the flash cache: all of it, or as many
// banks as fit for a big game. Returns null or an error.
static const char *copy_to_flash(const sd_game_t *g, int *lib_index) {
    char p[96];
    sd_path(p, "games", g->name, g->ext);
    uint32_t full, size;
    if (!sd_read_open(p, &full)) return "can't open the game file";
    size = full < lib_capacity() ? full : lib_capacity() & ~0x3FFFu;
    const char *err = lib_upload_begin(g->name, full, size, g->stamp, false, true);
    if (err) { sd_read_close(); return err; }
    uint32_t crc = 0, shown = 0;
    for (uint32_t off = 0; off < size;) {
        watchdog_update();
        STAGE(STG_COPY);
        if (off == 0 || off - shown >= 32768) {
            draw_progress(g->name, "Loading cartridge...", off, size);
            shown = off;
        }
        uint32_t want = size - off < sizeof iobuf ? size - off : sizeof iobuf;
        int n = sd_read(iobuf, want);
        if (n != (int)want) { sd_read_close(); lib_upload_abort(); return "SD read error"; }
        crc = crc32_update(crc, iobuf, n);
        err = lib_upload_write(off, iobuf, n);
        if (err) { sd_read_close(); lib_upload_abort(); return err; }
        off += n;
    }
    sd_read_close();
    draw_progress(g->name, "Checking...", size, size);
    return lib_upload_finish(crc, lib_index);
}

static const char *launch(int index) {
    if (index < 0 || index >= ngames) return "no such game";
    quit_game();
    sd_game_t g = games[index];
    const char *err = cart_error(g.err);
    if (err) return err;

    int li = use_sd ? cached_index(&g) : index;
    if (li < 0) {
        uint32_t t0 = millis();
        err = copy_to_flash(&g, &li);
        if (err) { log_line("copy failed: %s: %s", g.name, err); return err; }
        log_line("copied %s (%lu KB) to flash in %lu ms", g.name, (unsigned long)(g.size / 1024),
                 (unsigned long)(millis() - t0));
    }
    lib_touch(li);
    const lib_entry_t *e = lib_get(li);
    if (!e) return "flash cache error";
    if (e->stored_len < e->rom_len) {  // the rest of a big game is read from the card as it plays
        char p[96];
        sd_path(p, "games", g.name, g.ext);
        int pieces;
        if (!sd_rom_open(p, e->stored_len, &pieces)) return "can't open the game file";
        log_line("big game %s: %lu of %lu KB in flash, the rest read from the card (%d pieces)", g.name,
                 (unsigned long)(e->stored_len / 1024), (unsigned long)(e->rom_len / 1024), pieces);
    }

    cur = g;
    set_palette(use_sd ? game_palette(&g) : default_palette);
    game_palette_changed = false;
    memset(cart_ram, 0xFF, sizeof cart_ram);
    if (use_sd) {
        char p[96];
        game_path(p, "saves", ".sav");
        if (sd_load(p, cart_ram, sizeof cart_ram) < 0 && e->save_len) lib_save_read(e, cart_ram, sizeof cart_ram);
    } else {
        lib_save_read(e, cart_ram, sizeof cart_ram);
    }
    back = 0;
    int r = start_rom(e);
    if (r) return cart_error(r);
    playing = e;
    playing_index = index;
    state = ST_GAME;
    memset(&perf, 0, sizeof perf);
    perf.ppu0 = gb_prof_ppu_us;
    perf.apu0 = gb_prof_apu_us;

    if (use_sd) {  // resume where the last session ended
        char p[96];
        game_path(p, "states", ".res");
        int r = load_state(p);
        if (r == LOAD_BROKEN) start_rom(e);
        if (gb.has_battery && gb.ram_size) {
            game_path(p, "saves", ".sav");
            save_file_open = sd_save_open(p, cart_ram, gb.ram_size);
            if (save_file_open) cur.has_save = true;
            else log_line("can't open save: %s", cur.name);
            if (r == LOAD_OK) gb.ram_dirty_blocks = ~0ull;  // the resumed state's RAM replaces the .sav
        }
    }
    strncpy(last_played, g.name, SD_NAME_LEN - 1);
    settings_save();
    return nullptr;
}

// ---------------------------------------------------------------- uploads to the card

static struct {
    bool active;
    char name[SD_NAME_LEN], ext[5];
    uint32_t size, got, crc;
    uint8_t hdr[0x150];
} up;

static const char *sd_upload_begin(const char *fname, uint32_t size) {
    up.active = false;
    if (size < 0x8000 || size > (8u << 20) || (size & 0x3FFF)) return "not a Game Boy ROM size";
    if (size / 1024 + 64 > sd_free_kb()) return "SD card full";
    char clean[SD_NAME_LEN + 8];
    sd_clean_name(clean, fname, sizeof clean);
    char *dot = strrchr(clean, '.');
    strcpy(up.ext, ".gb");
    if (dot && !strcasecmp(dot, ".gbc")) strcpy(up.ext, ".gbc");
    if (dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"))) *dot = 0;
    clean[SD_NAME_LEN - 1] = 0;
    strcpy(up.name, clean);
    char p[96];
    sd_path(p, "games", up.name, up.ext);
    if (!sd_write_open(p)) return "can't create the file";
    up.size = size;
    up.got = up.crc = 0;
    up.active = true;
    return nullptr;
}

static const char *sd_upload_write(uint32_t off, const uint8_t *d, uint32_t n) {
    if (!up.active) return "no upload in progress";
    if (off != up.got || up.got + n > up.size) return "out of order";
    for (uint32_t i = 0; i < n && off + i < sizeof up.hdr; i++) up.hdr[off + i] = d[i];
    if (!sd_write(d, n)) { sd_write_abort(); up.active = false; return "SD write error"; }
    up.crc = crc32_update(up.crc, d, n);
    up.got += n;
    return nullptr;
}

static const char *sd_upload_finish(uint32_t crc, int *index) {
    if (!up.active) return "no upload in progress";
    up.active = false;
    const char *err = nullptr;
    uint8_t mbc; bool bat, rtc; uint32_t ram;
    if (up.got != up.size) err = "upload incomplete";
    else if (crc != up.crc) err = "checksum mismatch";
    else err = cart_error(gb_cart_info(up.hdr, up.size, &mbc, &bat, &rtc, &ram));
    if (err) { sd_write_abort(); return err; }
    if (!sd_write_commit()) return "SD write error";
    refresh_catalog();
    *index = find_game(up.name);
    return nullptr;
}

// ---------------------------------------------------------------- loader commands

static void reply(uint8_t type, uint8_t status, const char *msg, const uint8_t *extra = nullptr, int extra_len = 0) {
    uint8_t buf[96];
    int n = 0;
    buf[n++] = status;
    if (extra_len) { memcpy(buf + n, extra, extra_len); n += extra_len; }
    if (msg) { int m = strlen(msg); if (m > 80) m = 80; memcpy(buf + n, msg, m); n += m; }
    link_send(type, buf, n, true);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool uploading() { return use_sd ? up.active : lib_upload_active(); }

// Packet layouts are documented in link.h.
static void on_command(uint8_t cmd, const uint8_t *p, uint16_t len) {
    switch (cmd) {
    case 'I': {
        uint8_t b[26] = {2, (uint8_t)state, (uint8_t)playing_index, (uint8_t)ngames};
        wr32(b + 4, lib_free_bytes());
        memcpy(b + 8, last_reset, sizeof last_reset);
        b[20] = use_sd;
        wr32(b + 21, use_sd ? sd_free_kb() : 0);
        b[25] = lib_count();
        link_send('I', b, sizeof b, true);
        break;
    }
    case 'L': {
        static_assert(SD_MAX_GAMES * 87 <= sizeof scratch, "game list must fit");
        uint8_t *b = scratch;
        int n = 0;
        for (int i = 0; i < ngames; i++) {
            const sd_game_t *g = &games[i];
            b[n++] = i;
            wr32(b + n, g->size); n += 4;
            b[n++] = (g->has_save ? 1 : 0) | (cached_index(g) >= 0 ? 2 : 0) | (g->err ? 4 : 0);
            memcpy(b + n, g->name, SD_NAME_LEN); n += SD_NAME_LEN;
            memcpy(b + n, g->title, 17); n += 17;
        }
        link_send('L', b, n, true);
        break;
    }
    case 'U': {
        if (len < 5) { reply('U', 1, "bad request"); break; }
        quit_game();
        char name[SD_NAME_LEN + 8];
        int n = len - 4 < (int)sizeof name - 1 ? len - 4 : (int)sizeof name - 1;
        memcpy(name, p + 4, n);
        name[n] = 0;
        const char *err;
        if (use_sd) {
            err = sd_upload_begin(name, rd32(p));
        } else {
            char *dot = strrchr(name, '.');
            if (dot) *dot = 0;
            name[LIB_NAME_LEN - 1] = 0;
            err = lib_upload_begin(name, rd32(p), rd32(p), 0, true, false);
        }
        reply('U', err ? 1 : 0, err);
        break;
    }
    case 'D': {
        const char *err = len < 4 ? "bad request"
                          : use_sd ? sd_upload_write(rd32(p), p + 4, len - 4)
                                   : lib_upload_write(rd32(p), p + 4, len - 4);
        uint8_t g[4];
        wr32(g, use_sd ? up.got : lib_upload_progress());
        reply('D', err ? 1 : 0, err, g, 4);
        break;
    }
    case 'F': {
        int index = -1;
        const char *err = len < 4 ? "bad request" : use_sd ? sd_upload_finish(rd32(p), &index)
                                                           : lib_upload_finish(rd32(p), &index);
        if (!use_sd) refresh_catalog();
        uint8_t ix = index;
        reply('F', err ? 1 : 0, err, &ix, 1);
        set_message(err ? err : "Game added");
        break;
    }
    case 'X': {
        if (len < 1 || p[0] >= ngames) { reply('X', 1, "no such game"); break; }
        if (p[0] == playing_index) quit_game();
        const sd_game_t *g = &games[p[0]];
        const char *err = nullptr;
        int li = cached_index(g);
        if (use_sd) {
            char path[96];
            sd_path(path, "games", g->name, g->ext);
            if (!sd_remove(path)) err = "couldn't delete the file";
            if (li >= 0) lib_delete(li);
        } else {
            err = lib_delete(p[0]);
        }
        refresh_catalog();
        reply('X', err ? 1 : 0, err);
        break;
    }
    case 'R': {
        const char *err = len < 1 ? "bad request" : launch(p[0]);
        reply('R', err ? 1 : 0, err);
        if (err) set_message(err);
        break;
    }
    case 'Q': quit_game(); reply('Q', 0, nullptr); break;
    case 'A': {
        uint32_t f = perf.frames ? perf.frames : 1;
        uint32_t v[14] = {perf.frames, perf.underruns, perf.slow_frames, perf.max_emu_us, perf.max_frame_us,
                          perf.sum_emu_us / f, perf.stalls, perf.max_stall_ms,
                          (gb_prof_ppu_us - perf.ppu0) / f, (gb_prof_apu_us - perf.apu0) / f, perf.dropped,
                          perf.page_loads, perf.max_load_us, perf.sum_load_us};
        link_send('A', (const uint8_t *)v, sizeof v, true);
        if (len >= 1 && p[0]) {
            memset(&perf, 0, sizeof perf);
            perf.ppu0 = gb_prof_ppu_us;
            perf.apu0 = gb_prof_apu_us;
        }
        break;
    }
    case 'G': {
        uint8_t *b = scratch;
        char path[90];
        int n = len - 4 < (int)sizeof path - 1 ? len - 4 : (int)sizeof path - 1;
        if (len < 5) { reply('G', 1, "bad request"); break; }
        memcpy(path, p + 4, n);
        path[n] = 0;
        uint32_t size = 0;
        int got = sd_read_at(path, rd32(p), b + 5, 1024, &size);
        if (got < 0) { reply('G', 1, "no such file"); break; }
        b[0] = 0;
        wr32(b + 1, size);
        link_send('G', b, 5 + got, true);
        break;
    }
    case 'O': {
        char *txt = (char *)scratch;
        char path[90];
        int n = len < (int)sizeof path - 1 ? len : (int)sizeof path - 1;
        memcpy(path, p, n);
        path[n] = 0;
        int m = sd_list(path, txt + 1, sizeof scratch - 1);
        if (m < 0) { reply('O', 1, "no such folder"); break; }
        txt[0] = 0;
        link_send('O', (const uint8_t *)txt, 1 + m, true);
        break;
    }
    case 'Z': {
        char *txt = (char *)scratch;
        int n = sd_diag(txt, 1000);
        link_send('Z', (const uint8_t *)txt, n, true);
        break;
    }
    case 'K': if (len >= 1) pc_keys = p[0]; break;
    case 'P':
        if (len < 1) break;
        set_palette(p[0]);
        if (state == ST_MENU) { default_palette = palette_i; settings_changed(); }
        else game_palette_changed = true;
        break;
    }
}

// Sends the current picture to the loader as a 'V' packet (see link.h). A
// Color frame goes out half at a time so each packet fits the send queue.
// Nothing is sent when the queue is too full.
static void mirror(const uint8_t *src, uint8_t kind, const uint16_t *cpal = nullptr) {
    if (!link_connected()) return;
    uint8_t hdr[8];
    uint32_t f = gb.frame;
    hdr[0] = kind == SRC_CGB ? 2 : kind; hdr[1] = volume;
    hdr[2] = emu_us; hdr[3] = emu_us >> 8;
    hdr[4] = f; hdr[5] = f >> 8; hdr[6] = f >> 16; hdr[7] = f >> 24;
    if (kind == SRC_CGB) {
        static uint8_t half;
        half ^= 1;
        uint8_t row0 = half ? 0 : GB_H / 2;
        uint32_t n = GB_W * GB_H / 2;
        if (!link_stream_begin('V', sizeof hdr + 128 + 1 + n)) return;
        link_stream_put(hdr, sizeof hdr);
        link_stream_put((const uint8_t *)cpal, 128);  // RGB565, little-endian as stored on the RP2040
        link_stream_put(&row0, 1);
        link_stream_put(src + row0 * GB_W, n);
        link_stream_end();
        return;
    }
    uint32_t n = kind == SRC_GAME ? GB_W * GB_H / 4 : UI_W * UI_H / 2;
    if (!link_stream_begin('V', sizeof hdr + 32 + n)) return;
    link_stream_put(hdr, sizeof hdr);
    uint8_t chunk[64];
    for (int i = 0; i < 16; i++) { chunk[2 * i] = pal[i]; chunk[2 * i + 1] = pal[i] >> 8; }
    link_stream_put(chunk, 32);
    for (uint32_t i = 0; i < n; i += sizeof chunk) {
        uint32_t m = n - i < sizeof chunk ? n - i : sizeof chunk;
        for (uint32_t k = 0; k < m; k++) {
            const uint8_t *s = kind == SRC_GAME ? src + 4 * (i + k) : src + 2 * (i + k);
            chunk[k] = kind == SRC_GAME ? (s[0] & 3) | (s[1] & 3) << 2 | (s[2] & 3) << 4 | (s[3] & 3) << 6
                                        : (s[0] & 15) | (s[1] & 15) << 4;
        }
        link_stream_put(chunk, m);
    }
    link_stream_end();
}

// ---------------------------------------------------------------- main menu

static int sel;
static bool confirm_delete;
static bool options_open;       // the Colors / Volume page
static int options_row;

#define LIST_Y 16
#define ROW_H 11
#define ROWS 8

// Text cut to `max_px` pixels, ending in ".." when it doesn't fit.
static void ui_text_fit(int x, int y, const char *s, uint8_t c, int max_px) {
    int chars = max_px / (FONT_W + 1), len = strlen(s);
    if (len <= chars) { ui_text(x, y, s, c); return; }
    if (chars < 3) return;
    ui_text(x, y, s, c, chars - 2);
    ui_text(x + (chars - 2) * (FONT_W + 1), y, "..", c);
}

static void draw_footer(const char *text, uint8_t c) {
    ui_rect(0, UI_H - 12, UI_W, 12, C_BAR);
    ui_text_fit(3, UI_H - 9, text, c, UI_W - 6);
}

// Options page: default colours for original Game Boy games, and volume.
static void draw_options() {
    ui_rect(0, 0, UI_W, UI_H, C_BG);
    ui_rect(0, 0, UI_W, 12, C_BAR);
    ui_text(3, 3, "Options", C_HEAD);
    char buf[24];

    bool on = options_row == 0;
    if (on) ui_rect(0, 16, UI_W, 38, C_SEL);
    ui_text(6, 20, "Colors", on ? C_TEXT : C_DIM);
    snprintf(buf, sizeof buf, "< %s >", palettes[default_palette].name);
    ui_right(20, buf, on ? C_HEAD : C_DIM);
    for (int i = 0; i < 4; i++) {  // colour indexes 0..3 are the game shades
        ui_rect(6 + i * 37, 32, 35, 11, C_BAR);
        ui_rect(7 + i * 37, 33, 33, 9, i);
    }
    ui_text(6, 45, "Original Game Boy games", C_DIM);

    on = options_row == 1;
    if (on) ui_rect(0, 58, UI_W, 30, C_SEL);
    ui_text(6, 62, "Volume", on ? C_TEXT : C_DIM);
    snprintf(buf, sizeof buf, "< %s >", volume_name());
    ui_right(62, buf, on ? C_HEAD : C_DIM);
    for (int i = 0; i < 4; i++) ui_rect(6 + i * 37, 75, 35, 8, i < volume ? C_ACCENT : C_BAR);

    ui_center(94, "Color games always use", C_DIM);
    ui_center(104, "their own colours", C_DIM);
    draw_footer("A/D change  J back", C_DIM);
}

static void draw_menu() {
    ui_begin();
    if (options_open) { draw_options(); return; }
    ui_rect(0, 0, UI_W, UI_H, C_BG);
    ui_rect(0, 0, UI_W, 12, C_BAR);
    ui_text(3, 3, "PicoGB", C_HEAD);
    char buf[48];
    if (use_sd) snprintf(buf, sizeof buf, "%d games  SD %luM", ngames, (unsigned long)(sd_free_kb() / 1024));
    else snprintf(buf, sizeof buf, "%d games  %luK", ngames, (unsigned long)(lib_free_bytes() / 1024));
    ui_right(3, buf, C_DIM);

    if (uploading()) {
        uint32_t got = use_sd ? up.got : lib_upload_progress();
        ui_center(44, "Receiving game...", C_TEXT);
        if (use_sd) ui_bar(60, got, up.size);
        snprintf(buf, sizeof buf, "%lu KB", (unsigned long)(got / 1024));
        ui_center(72, buf, C_DIM);
    } else if (ngames == 0) {
        ui_center(30, "No games yet", C_TEXT);
        if (use_sd) {
            ui_center(48, "Copy .gb files to", C_DIM);
            ui_center(58, "PicoGB/games on the SD", C_DIM);
            ui_center(68, "card, or use the loader", C_DIM);
        } else {
            ui_center(48, "Insert an SD card, or", C_DIM);
            ui_center(58, "upload with the PicoGB", C_DIM);
            ui_center(68, "loader (Chrome/Edge)", C_DIM);
        }
    } else {
        if (sel >= ngames) sel = ngames - 1;
        int first = sel < ROWS ? 0 : sel - ROWS + 1;
        for (int i = first; i < ngames && i < first + ROWS; i++) {
            const sd_game_t *g = &games[i];
            int y = LIST_Y + (i - first) * ROW_H;
            bool on = i == sel;
            if (on) ui_rect(0, y - 2, UI_W, ROW_H, confirm_delete ? C_WARN : C_SEL);
            if (use_sd && cached_index(g) >= 0) ui_rect(1, y + 2, 2, 3, C_ACCENT);  // marks games already in flash
            if (g->err) strcpy(buf, "??");
            else snprintf(buf, sizeof buf, "%luK", (unsigned long)(g->size / 1024));
            int label_x = UI_W - 4 - (int)strlen(buf) * (FONT_W + 1);
            ui_right(y, buf, on ? C_TEXT : C_DIM);
            ui_text_fit(5, y, g->name, g->err ? C_DIM : on ? C_TEXT : C_DIM, label_x - 5 - 6);
        }
    }

    if (millis() < message_until) draw_footer(message, C_WARN);
    else if (confirm_delete) draw_footer(use_sd ? "Delete game? L yes  J no" : "Delete? L yes  J no", C_WARN);
    else draw_footer(volume ? "L play  I options" : "L play  I options (muted)", C_DIM);
}

// Colors and Volume, shared by the options page and the pause menu. From the
// main menu Colors sets the default; in a game it sets that game's own.
static void adjust_option(int row, int dir) {
    if (row == 0 && state == ST_MENU) {
        default_palette = (default_palette + dir + NUM_PALETTES) % NUM_PALETTES;
        set_palette(default_palette);
    } else if (row == 0) {
        set_palette(palette_i + dir);
        game_palette_changed = true;
        return;
    }
    else volume = constrain(volume + dir, 0, 4);
    settings_changed();
}

static void menu_step() {
    static uint32_t select_since;
    audio_run(false);
    uint8_t k = read_keys(), pressed = key_presses(k);
    uint32_t now = millis();

    if (confirm_delete) {
        if (pressed & GB_A) {
            uint8_t req[1] = {(uint8_t)sel};
            confirm_delete = false;
            on_command('X', req, 1);  // same as a delete from the loader
            set_message("Deleted");
        } else if (pressed & (GB_B | GB_UP | GB_DOWN)) {
            confirm_delete = false;
        }
    } else if (options_open) {
        if (pressed & (GB_UP | GB_DOWN)) options_row ^= 1;
        if (pressed & GB_LEFT) adjust_option(options_row, -1);
        if (pressed & GB_RIGHT) adjust_option(options_row, +1);
        if (pressed & (GB_START | GB_B)) options_open = false;
    } else {
        if ((pressed & GB_UP) && sel > 0) sel--;
        if ((pressed & GB_DOWN) && sel < ngames - 1) sel++;
        if (pressed & GB_START) { options_open = true; options_row = 0; }
        if ((pressed & GB_A) && ngames) {
            const char *err = launch(sel);
            if (err) set_message(err);
            if (state != ST_MENU) return;
        }
        // Holding Select for 1.5 s asks to delete the selected game.
        if (!(k & GB_SELECT)) select_since = now;
        else if (ngames && now - select_since > 1500) { confirm_delete = true; select_since = now + 100000; }
    }
    if (settings_dirty && now - settings_changed_at > 2000) settings_save();

    // While a game is arriving, keep answering the loader quickly and only
    // redraw the progress screen a few times a second.
    static uint32_t drawn_at;
    bool receiving = uploading();
    if (receiving && now - drawn_at < 250) return;
    drawn_at = now;
    draw_menu();
    ui_show();
    mirror(ui, SRC_UI);
    if (!receiving) delay(15);
}

// ---------------------------------------------------------------- pause menu

enum { P_RESUME, P_SAVE, P_LOAD, P_VOLUME, P_COLORS, P_SHOT, P_RESTART, P_QUIT, P_COUNT };
static int psel, pslot = 1;
static bool slot_used[5];

static void slot_path(char *p, int slot) {
    char ext[4] = {'.', 's', (char)('0' + slot), 0};
    game_path(p, "states", ext);
}

static void open_pause() {
    state = ST_PAUSE;
    ui = fbs[back];  // the other buffer is last_frame, kept for screenshots
    psel = P_RESUME;
    if (use_sd)
        for (int s = 1; s <= 4; s++) {
            char p[96];
            slot_path(p, s);
            slot_used[s] = sd_exists(p);
        }
}

static void draw_pause() {
    ui_begin();
    ui_rect(0, 0, UI_W, UI_H, C_BG);
    ui_rect(0, 0, UI_W, 12, C_BAR);
    ui_text(3, 3, cur.name, C_HEAD, 26);
    static const char *items[P_COUNT] = {"Resume", "Save state", "Load state", "Volume", "Colors",
                                         "Screenshot", "Restart game", "Quit to menu"};
    for (int i = 0; i < P_COUNT; i++) {
        int y = 16 + i * 12;
        bool on = i == psel, needs_sd = i == P_SAVE || i == P_LOAD || i == P_SHOT;
        uint8_t c = (needs_sd && !use_sd) ? C_DIM : on ? C_TEXT : C_DIM;
        if (on) ui_rect(0, y - 2, UI_W, 12, C_SEL);
        ui_text(8, y, items[i], c);
        char b[16] = "";
        if (i == P_SAVE || i == P_LOAD) snprintf(b, sizeof b, "< %d%s >", pslot, slot_used[pslot] ? "" : " -");
        if (i == P_VOLUME) snprintf(b, sizeof b, "< %s >", volume_name());
        if (i == P_COLORS) snprintf(b, sizeof b, "< %s >", palettes[palette_i].name);
        if (b[0]) ui_right(y, b, on ? C_HEAD : C_DIM);
    }
    ui_rect(0, UI_H - 12, UI_W, 12, C_BAR);
    if (millis() < message_until) ui_text(3, UI_H - 9, message, C_WARN, 26);
    else ui_text(3, UI_H - 9, use_sd ? "L ok  J back  A/D change" : "No SD card: no states", C_DIM, 26);
}

static void restart_game() {
    char p[96];
    game_path(p, "states", ".res");
    if (use_sd) sd_remove(p);
    save_battery();
    back = 0;
    start_rom(playing);
}

static bool wait_release;  // after leaving the pause menu, ignore keys until they are released

static void pause_step() {
    audio_run(false);
    uint8_t k = read_keys(), pressed = key_presses(k);
    if (pressed & GB_UP) psel = (psel + P_COUNT - 1) % P_COUNT;
    if (pressed & GB_DOWN) psel = (psel + 1) % P_COUNT;
    if ((psel == P_SAVE || psel == P_LOAD) && (pressed & GB_LEFT)) pslot = pslot == 1 ? 4 : pslot - 1;
    if ((psel == P_SAVE || psel == P_LOAD) && (pressed & GB_RIGHT)) pslot = pslot == 4 ? 1 : pslot + 1;
    if (psel == P_VOLUME || psel == P_COLORS) {
        int dir = (pressed & GB_RIGHT) ? 1 : (pressed & GB_LEFT) ? -1 : 0;
        if (dir) adjust_option(psel == P_VOLUME ? 1 : 0, dir);
    }
    if (settings_dirty && millis() - settings_changed_at > 2000) settings_save();
    if (pressed & GB_B) { state = ST_GAME; wait_release = true; return; }
    if (pressed & GB_A) {
        char p[96];
        slot_path(p, pslot);
        switch (psel) {
        case P_RESUME: state = ST_GAME; wait_release = true; return;
        case P_SAVE:
            if (!use_sd) break;
            draw_progress(cur.name, "Saving state...", 0, 0);
            if (save_state(p)) { slot_used[pslot] = true; set_message("State saved"); }
            else set_message("Save failed");
            break;
        case P_LOAD: {
            if (!use_sd) break;
            int r = load_state(p);
            if (r == LOAD_OK) { state = ST_GAME; wait_release = true; return; }
            if (r == LOAD_BROKEN) { restart_game(); set_message("State was damaged"); }
            else set_message(slot_used[pslot] ? "State doesn't fit" : "Slot is empty");
            break;
        }
        case P_SHOT:
            if (!use_sd) break;
            set_message(screenshot() ? "Screenshot saved" : "Screenshot failed");
            break;
        case P_RESTART: restart_game(); state = ST_GAME; wait_release = true; return;
        case P_QUIT: {
            draw_progress(cur.name, "Saving...", 0, 0);
            quit_game();
            int i = find_game(cur.name);
            sel = i < 0 ? 0 : i;
            wait_release = true;
            return;
        }
        }
    }
    draw_pause();
    ui_show();
    mirror(ui, SRC_UI);
    delay(15);
}

// ---------------------------------------------------------------- game loop

static void game_step() {
    static uint32_t combo_since;
    static uint8_t mirror_tick;
    uint8_t k = read_keys();
    uint32_t now = millis();
    if (wait_release) {
        if (k) k = 0;
        else wait_release = false;
    }
    if ((k & (GB_START | GB_SELECT)) != (GB_START | GB_SELECT)) combo_since = now;
    else if (now - combo_since > 500) { save_battery(); open_pause(); return; }
    gb_set_buttons(&gb, k);

    audio_run(true);
    while (disp_pending && disp_src == gb.fb) tight_loop_contents();  // a menu may have just been shown from it
    static uint32_t last_frame_end;
    STAGE(STG_EMU);
    uint32_t t0 = time_us_32();
    gb_run_frame(&gb);
    emu_us = time_us_32() - t0;
    perf.frames++;
    perf.sum_emu_us += emu_us;
    if (emu_us > perf.max_emu_us) perf.max_emu_us = emu_us;

    STAGE(STG_PUBLISH);
    const uint8_t *done = fbs[back];
    uint16_t *cpal = dpal[back];
    if (gb.cgb)
        for (int i = 0; i < 64; i++) {  // RGB555 palette RAM -> RGB565
            uint16_t c = gb.frame_pal[2 * i] | gb.frame_pal[2 * i + 1] << 8;
            uint16_t r = c & 31, g5 = (c >> 5) & 31, b = (c >> 10) & 31;
            cpal[i] = r << 11 | (g5 << 1 | g5 >> 4) << 5 | b;
        }
    uint8_t kind = gb.cgb ? SRC_CGB : SRC_GAME;
    if (publish(done, kind, cpal)) {
        last_frame = done;
        back ^= 1;
        gb.fb = fbs[back];
    } else {
        perf.dropped++;  // core 1 was still sending the previous frame
    }
    STAGE(STG_MIRROR);
    if (++mirror_tick & 1) mirror(done, kind, cpal);  // 30 fps to the loader

    // Frames are paced by the clock at the Game Boy's 59.73 Hz. Waiting for
    // room in the I2S buffers instead let a frame start only when a whole
    // 256-sample buffer drained (every 7.8 ms), so about every seventh frame
    // came 7.8 ms late, a visible hitch. The sound buffer level still nudges
    // the pace, so the emulator follows the speaker's clock and the buffer
    // neither runs dry nor overflows.
    STAGE(STG_AUDIO);
    static uint32_t next_frame;
    static int32_t fill_err;  // smoothed distance from AUDIO_TARGET, in samples
    static int audio_cap;     // the most the buffers have ever had free
    int avail = i2s.availableForWrite() / 4;
    if (avail > audio_cap) audio_cap = avail;
    int queued = audio_cap - avail;
    uint32_t t_now = time_us_32();
    if (!next_frame || (int32_t)(t_now - next_frame) > 50000) {  // first frame, or after a pause or a stall
        next_frame = t_now;
        fill_err = 0;
        for (int i = queued; i < AUDIO_TARGET - (int)gb.audio_n; i++) i2s.write16(0, 0);
    } else {
        fill_err += (queued - AUDIO_TARGET - fill_err) / 8;
        int32_t adjust = fill_err / 4;  // microseconds: a fuller buffer means we're ahead
        if (adjust > 1500) adjust = 1500;
        if (adjust < -1500) adjust = -1500;
        next_frame += FRAME_US + adjust;
        while ((int32_t)(time_us_32() - next_frame) < 0) tight_loop_contents();
    }
    static const uint8_t shift[5] = {16, 4, 3, 2, 1};
    uint32_t deadline = time_us_32() + 40000;
    while (i2s.availableForWrite() / 4 < gb.audio_n && (int32_t)(time_us_32() - deadline) < 0) tight_loop_contents();
    int room = i2s.availableForWrite() / 4;
    if (i2s.getUnderflow()) perf.underruns++;  // the speaker ran dry since the last frame
    for (int i = 0; i < gb.audio_n && i < room; i++) {
        int16_t s = volume ? gb.audio[i] >> shift[volume] : 0;
        i2s.write16(s, s);
    }
    uint32_t end = time_us_32(), frame_us = end - last_frame_end;
    if (last_frame_end && perf.frames > 2) {
        if (frame_us > perf.max_frame_us) perf.max_frame_us = frame_us;
        if (frame_us > 17500) perf.slow_frames++;
        if (frame_us > 50000) { perf.stalls++; if (frame_us / 1000 > perf.max_stall_ms) perf.max_stall_ms = frame_us / 1000; }
    }
    last_frame_end = end;

    // Battery RAM goes out a block per frame once the game has stopped
    // writing to it for a moment.
    if (use_sd) {
        if (gb.ram_dirty_blocks && gb.frame - gb.ram_write_frame > 20) save_battery_blocks(1);
    } else if (gb.ram_dirty && gb.frame - gb.ram_write_frame > 90) {
        save_battery();
    }
}

// ---------------------------------------------------------------- setup

void setup() {
    check_last_reset();
    set_palette(0);
    sprig_init();
    lib_init();
    draw_progress("PicoGB", "Reading SD card...", 0, 0);
    sprig_backlight(true);
    use_sd = sd_init();
    if (use_sd) settings_load();
    link_init(on_command);
    refresh_catalog();
    int last = find_game(last_played);
    if (last >= 0) sel = last;
    if (!lib_program_fits()) set_message("firmware overlaps library!");
    else if (!use_sd) set_message("No SD card: using flash");
    if (last_reset[0]) {
        char m[48];
        snprintf(m, sizeof m, last_reset[0] == 1 ? "crashed at %08lx" : "froze in stage %lu", (unsigned long)last_reset[1]);
        set_message(m);
        log_line("previous run %s (lr %08lx)", m, (unsigned long)last_reset[2]);
    }
    watchdog_enable(3000, true);
}

void loop() {
    watchdog_update();
    STAGE(STG_LINK);
    link_poll();
    if (state == ST_GAME) game_step();
    else if (state == ST_PAUSE) pause_step();
    else { STAGE(STG_MENU); menu_step(); }
}
