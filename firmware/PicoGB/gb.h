// PicoGB - a Game Boy and Game Boy Color emulator written for the RP2040.
//
// The core is plain C++ with no platform code, so it also builds on the PC
// (host/gbtest.cpp) to run test ROMs. It starts in the post-boot-ROM state;
// no Nintendo boot ROM is used. Games whose header says they support the
// Game Boy Color run in Color mode, everything else as an original Game Boy.
//
// Frame buffer pixels: in Game Boy mode the shade 0..3; in Color mode an
// index into frame_pal: 0..31 background (palette * 4 + colour), 32..63
// sprites. frame_pal is the palette RAM as it was when the frame started.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GB_W 160
#define GB_H 144
#define GB_CLOCK 4194304               // T-cycles per second
#define GB_FRAME_CYCLES 70224          // T-cycles per frame (154 lines x 456)
#define GB_AUDIO_RATE 32768            // one sample every 128 T-cycles
#define GB_AUDIO_MAX 1024              // samples buffered per run_frame (a frame is ~549)
#define GB_MAX_CART_RAM 0x8000

#ifdef ARDUINO
#include <pico.h>
#include <hardware/timer.h>
#define GB_FAST(f) __not_in_flash_func(f)
#define GB_NOW() time_us_32()
#else
#define GB_FAST(f) f
#define GB_NOW() 0u
#endif

// Time spent drawing lines and mixing audio, in microseconds (Pico only).
extern uint32_t gb_prof_ppu_us, gb_prof_apu_us;

enum {  // joypad bits, 1 = pressed
    GB_RIGHT = 1, GB_LEFT = 2, GB_UP = 4, GB_DOWN = 8,
    GB_A = 16, GB_B = 32, GB_SELECT = 64, GB_START = 128,
};

enum { GB_OK, GB_ERR_SIZE, GB_ERR_MAPPER, GB_ERR_CGB_ONLY, GB_ERR_RAM };

typedef struct {
    // per-channel state, indexed SQ1, SQ2, WAVE, NOISE (see gb_apu.cpp)
    bool on[4], dac[4], len_en[4];
    uint16_t len[4];
    uint8_t vol[4], env_vol[4], env_dir[4], env_per[4], env_timer[4];
    uint16_t freq[4];
    int32_t timer[4];
    uint8_t duty_pos[2], duty[2];
    uint8_t sweep_per, sweep_dir, sweep_shift, sweep_timer;
    bool sweep_en;
    uint16_t sweep_shadow;
    uint8_t wave_pos;
    uint16_t lfsr;
    uint8_t seq_step;
    int32_t seq_timer;
    int32_t sample_timer;
} gb_apu_t;

typedef struct gb {
    // CPU
    uint8_t r[8];          // B C D E H L - A, the order opcodes use (6 means (HL))
    uint8_t f;
    uint16_t sp, pc;
    bool ime, halted, halt_bug;
    uint8_t ei_delay;

    // memory: 4 KB pages; a null entry means "go through the slow path"
    const uint8_t *rd[16];
    uint8_t *wr[16];
    const uint8_t *rom, *rom_bank0;
    uint32_t rom_size;
    uint16_t rom_banks;
    uint8_t *cart_ram;
    uint32_t ram_size;
    bool ram_dirty;
    uint32_t ram_write_frame;  // frame of the last cart RAM write, for autosave
    uint64_t ram_dirty_blocks; // bit n set: the 512 bytes at n*512 changed since the last save

    // mapper
    uint8_t mbc;           // 0 none, 1 MBC1, 2 MBC2, 3 MBC3, 5 MBC5
    bool has_battery, has_rtc;
    bool ram_en, mbc1_mode;
    uint16_t rom_bank;
    uint8_t ram_bank, bank_hi;
    uint8_t rtc[5], rtc_latched[5], rtc_latch;
    uint32_t rtc_cycles;

    uint8_t wram[0x8000], vram[0x4000], oam[0xA0], hram[0x7F], io[0x80];
    uint8_t ie;

    // Game Boy Color
    bool cgb, double_speed;
    uint8_t key1, vram_bank, wram_bank;
    uint8_t bg_pal[64], obj_pal[64];   // palette RAM, RGB555 little-endian
    uint8_t frame_pal[128];            // bg_pal + obj_pal when this frame started
    uint16_t hdma_src, hdma_dst;
    uint8_t hdma_len;                  // 16-byte blocks left of an HBlank DMA
    bool hdma_active;
    uint32_t stall;                    // CPU cycles a general DMA still holds the bus

    // timer
    uint16_t div;          // DIV is the top byte of this 16-bit counter
    // Cycles into the current instruction: memory accesses add 4 each, and a
    // timer register access first brings the timer up to that exact cycle.
    uint16_t acc_t, synced_t;
    uint32_t cycles;       // T-cycles this frame

    // PPU
    uint16_t dot;          // T-cycle within the current line
    uint16_t ppu_next;     // `dot` of the next mode change: call gb_ppu_event() there
    uint8_t ly, mode, win_line;
    bool stat_line, lcd_was_on, win_triggered;
    uint8_t *fb;           // GB_W x GB_H, format at the top of this file; written line by line
    uint32_t frame;

    uint8_t buttons;

    gb_apu_t apu;
    int16_t audio[GB_AUDIO_MAX];
    uint16_t audio_n;

    // test ROMs print their results through the serial port
    void (*serial_out)(uint8_t c);
} gb_t;

// Loads a cartridge: rom stays where it is (flash on the Pico); cart_ram must
// hold GB_MAX_CART_RAM bytes and should already contain the save, if any.
int gb_init(gb_t *g, const uint8_t *rom, uint32_t rom_size, uint8_t *cart_ram, uint8_t *fb);
// Points the core at a copy of ROM bank 0 in RAM, which reads faster than flash.
void gb_set_bank0(gb_t *g, const uint8_t *bank0);
// Runs until the next frame is complete (or GB_FRAME_CYCLES if the LCD is off).
void gb_run_frame(gb_t *g);
void gb_set_buttons(gb_t *g, uint8_t pressed);
uint32_t gb_cart_ram_size(const uint8_t *rom);  // from the header, 0 if none
// Checks a ROM header; returns GB_OK or an error, fills the mapper info.
int gb_cart_info(const uint8_t *rom, uint32_t rom_size, uint8_t *mbc, bool *battery, bool *rtc, uint32_t *ram);
void gb_title(const uint8_t *rom, char out[17]);

// Save states are the raw gb_t followed by the cart RAM. After reading one
// back into a gb_t, restore its pointer fields (rom, rom_bank0, cart_ram, fb,
// serial_out) and call gb_remap() to rebuild the memory map.
#define GB_STATE_MAGIC 0x53424750u  // "PGBS"
#define GB_STATE_VERSION 4
typedef struct {
    uint32_t magic, version, gb_size, ram_size;
    char title[17];
    uint8_t pad[3];
    uint32_t frame;
} gb_state_hdr_t;
void gb_remap(gb_t *g);

// gb_ppu.cpp
void gb_ppu_reset(gb_t *g);
void gb_ppu_event(gb_t *g);
uint8_t gb_ppu_read_stat(gb_t *g);
void gb_ppu_write(gb_t *g, uint8_t reg, uint8_t v);
void gb_hdma_hblank(gb_t *g);  // gb.cpp: one 16-byte block of an HBlank DMA

// gb_apu.cpp
void gb_apu_reset(gb_t *g);
void gb_apu_event(gb_t *g);  // a frame-sequencer step or a sample is due
uint8_t gb_apu_read(gb_t *g, uint8_t reg);
void gb_apu_write(gb_t *g, uint8_t reg, uint8_t v);
