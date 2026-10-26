// APU: the four sound channels, mixed to mono (the Sprig has one speaker).
// One output sample every 128 T-cycles (GB_AUDIO_RATE); channel timers are
// advanced a whole sample at a time.
#include "gb.h"
#include <string.h>

#define A (g->apu)
#define REG(r) g->io[r]
enum { SQ1, SQ2, WAVE, NOISE };

static const uint8_t duty_table[4] = {0x01, 0x81, 0x87, 0x7E};  // 12.5, 25, 50, 75 %
// Bits that always read back as 1, for 0x10..0x2F.
static const uint8_t read_mask[0x20] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
    0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
// Register base of each channel's NRx0..NRx4 block.
static const uint8_t ch_base[4] = {0x10, 0x15, 0x1A, 0x1F};

void gb_apu_reset(gb_t *g) {
    memset(&A, 0, sizeof A);
    A.lfsr = 0x7FFF;
    A.seq_timer = 8192;
    A.sample_timer = 128;
    static const uint8_t init[0x17] = {  // post-boot register values
        0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF,
        0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1};
    memcpy(&g->io[0x10], init, sizeof init);
    A.dac[SQ1] = true;
}

static int period(gb_t *g, int ch) {
    switch (ch) {
    case SQ1: case SQ2: return (2048 - A.freq[ch]) * 4;
    case WAVE: return (2048 - A.freq[ch]) * 2;
    default: {
        uint8_t r = REG(0x22);
        int div = (r & 7) ? (r & 7) * 16 : 8;
        return div << (r >> 4);
    }
    }
}

static uint16_t sweep_calc(gb_t *g) {
    uint16_t d = A.sweep_shadow >> A.sweep_shift;
    uint16_t f = A.sweep_dir ? A.sweep_shadow - d : A.sweep_shadow + d;
    if (f > 2047) A.on[SQ1] = false;
    return f;
}

static void trigger(gb_t *g, int ch) {
    A.on[ch] = A.dac[ch];
    if (A.len[ch] == 0) A.len[ch] = ch == WAVE ? 256 : 64;
    A.timer[ch] = period(g, ch);
    if (ch == WAVE) {
        A.wave_pos = 0;
    } else {
        uint8_t nr2 = REG(ch_base[ch] + 2);
        A.env_vol[ch] = nr2 >> 4;
        A.env_dir[ch] = (nr2 >> 3) & 1;
        A.env_per[ch] = nr2 & 7;
        A.env_timer[ch] = A.env_per[ch];
        if (ch == NOISE) A.lfsr = 0x7FFF;
    }
    if (ch == SQ1) {
        A.sweep_shadow = A.freq[SQ1];
        A.sweep_timer = A.sweep_per ? A.sweep_per : 8;
        A.sweep_en = A.sweep_per || A.sweep_shift;
        if (A.sweep_shift) sweep_calc(g);
    }
}

uint8_t gb_apu_read(gb_t *g, uint8_t reg) {
    if (reg >= 0x30) return REG(reg);  // wave RAM
    if (reg == 0x26)
        return (REG(0x26) & 0x80) | 0x70 | (A.on[SQ1] ? 1 : 0) | (A.on[SQ2] ? 2 : 0) | (A.on[WAVE] ? 4 : 0) |
               (A.on[NOISE] ? 8 : 0);
    return REG(reg) | read_mask[reg - 0x10];
}

void gb_apu_write(gb_t *g, uint8_t reg, uint8_t v) {
    if (reg >= 0x30) { REG(reg) = v; return; }
    if (reg == 0x26) {
        if (!(v & 0x80) && (REG(0x26) & 0x80)) {  // power off clears everything
            for (int r = 0x10; r < 0x26; r++) REG(r) = 0;
            for (int c = 0; c < 4; c++) A.on[c] = A.dac[c] = false;
        }
        REG(0x26) = v & 0x80;
        return;
    }
    if (!(REG(0x26) & 0x80)) return;  // writes are ignored while the APU is off
    REG(reg) = v;

    int ch = reg < 0x15 ? SQ1 : reg < 0x1A ? SQ2 : reg < 0x1F ? WAVE : reg < 0x24 ? NOISE : -1;
    if (ch < 0) return;  // NR50 / NR51: read at mix time
    switch (reg - ch_base[ch]) {
    case 0:
        if (ch == SQ1) {
            A.sweep_per = (v >> 4) & 7;
            A.sweep_dir = (v >> 3) & 1;
            A.sweep_shift = v & 7;
        } else if (ch == WAVE) {
            A.dac[WAVE] = v & 0x80;
            if (!A.dac[WAVE]) A.on[WAVE] = false;
        }
        break;
    case 1:
        if (ch <= SQ2) A.duty[ch] = v >> 6;
        A.len[ch] = ch == WAVE ? 256 - v : 64 - (v & 63);
        break;
    case 2:
        if (ch != WAVE) {
            A.dac[ch] = (v & 0xF8) != 0;
            if (!A.dac[ch]) A.on[ch] = false;
        }
        break;
    case 3:
        if (ch != NOISE) A.freq[ch] = (A.freq[ch] & 0x700) | v;
        break;
    case 4:
        if (ch != NOISE) A.freq[ch] = (A.freq[ch] & 0xFF) | (v & 7) << 8;
        A.len_en[ch] = v & 0x40;
        if (v & 0x80) trigger(g, ch);
        break;
    }
}

// 512 Hz frame sequencer: length at 256 Hz, sweep at 128 Hz, envelope at 64 Hz.
static void sequencer(gb_t *g) {
    uint8_t s = A.seq_step = (A.seq_step + 1) & 7;
    if (!(s & 1))
        for (int c = 0; c < 4; c++)
            if (A.len_en[c] && A.len[c] && --A.len[c] == 0) A.on[c] = false;
    if ((s == 2 || s == 6) && A.sweep_en) {
        if (--A.sweep_timer == 0) {
            A.sweep_timer = A.sweep_per ? A.sweep_per : 8;
            if (A.sweep_per) {
                uint16_t f = sweep_calc(g);
                if (f <= 2047 && A.sweep_shift) {
                    A.sweep_shadow = A.freq[SQ1] = f;
                    sweep_calc(g);
                }
            }
        }
    }
    if (s == 7)
        for (int c = 0; c < 4; c++) {
            if (c == WAVE || !A.env_per[c] || --A.env_timer[c]) continue;
            A.env_timer[c] = A.env_per[c];
            if (A.env_dir[c] && A.env_vol[c] < 15) A.env_vol[c]++;
            else if (!A.env_dir[c] && A.env_vol[c] > 0) A.env_vol[c]--;
        }
}

// Advances a channel by one sample period and returns its level, -15..15.
static inline int channel(gb_t *g, int c) {
    if (!A.on[c]) return 0;
    int p = period(g, c);
    A.timer[c] -= 128;
    while (A.timer[c] <= 0) {
        A.timer[c] += p;
        if (c <= SQ2) A.duty_pos[c] = (A.duty_pos[c] + 1) & 7;
        else if (c == WAVE) A.wave_pos = (A.wave_pos + 1) & 31;
        else {
            uint16_t x = (A.lfsr ^ (A.lfsr >> 1)) & 1;
            A.lfsr = (A.lfsr >> 1) | (x << 14);
            if (REG(0x22) & 8) A.lfsr = (A.lfsr & ~0x40) | (x << 6);  // 7-bit mode
        }
    }
    switch (c) {
    case SQ1: case SQ2:
        return ((duty_table[A.duty[c]] >> (7 - A.duty_pos[c])) & 1) ? A.env_vol[c] : -A.env_vol[c];
    case WAVE: {
        uint8_t b = REG(0x30 + (A.wave_pos >> 1));
        int s = (A.wave_pos & 1) ? (b & 15) : (b >> 4);
        static const uint8_t shift[4] = {4, 0, 1, 2};
        return ((s * 2 - 15) >> shift[(REG(0x1C) >> 5) & 3]);
    }
    default:
        return (A.lfsr & 1) ? -A.env_vol[c] : A.env_vol[c];
    }
}

// The CPU loop counts seq_timer and sample_timer down and calls this when
// either runs out.
void GB_FAST(gb_apu_event)(gb_t *g) {
    if (A.seq_timer <= 0) {
        A.seq_timer += 8192;
        sequencer(g);
    }
    if (A.sample_timer > 0) return;
    A.sample_timer += 128;

    uint32_t t0 = GB_NOW();
    int mix = 0;
    uint8_t pan = REG(0x25);
    for (int c = 0; c < 4; c++) {
        int v = channel(g, c);
        if (pan & (0x11 << c)) mix += v;  // panned to at least one side
    }
    uint8_t nr50 = REG(0x24);
    int vol = ((nr50 >> 4) & 7) + (nr50 & 7) + 2;  // 2..16
    if (g->audio_n < GB_AUDIO_MAX) g->audio[g->audio_n++] = (int16_t)(mix * vol * 32);  // max 60*16*32 = 30720
    gb_prof_apu_us += GB_NOW() - t0;
}
