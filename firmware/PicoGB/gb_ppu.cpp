// PPU: LCD mode timing, STAT/VBlank interrupts and a scanline renderer.
// Each line is drawn in one go when it enters mode 3, using the registers as
// they are at that moment, which is what nearly every game relies on.
//
// The CPU loop only calls gb_ppu_event() when `dot` reaches `ppu_next`, the
// next mode change, so most instructions cost the PPU a single compare.
#include "gb.h"
#include <string.h>

enum { M_HBLANK, M_VBLANK, M_OAM, M_DRAW };
#define LCDC g->io[0x40]
#define STAT g->io[0x41]

// spread[b] has bit i of b moved to bit 2i: two of them interleave a tile
// row's low and high bitplanes into eight 2-bit colour indices at once.
static uint16_t spread[256];
// expand4[b] has the four 2-bit pixels of b (leftmost in the top bits) as
// four bytes, leftmost at the lowest address.
static uint32_t expand4[256];

#define TAKEN(x) (taken[(x) >> 5] & (1u << ((x) & 31)))
#define TAKE(x) (taken[(x) >> 5] |= 1u << ((x) & 31))

static void GB_FAST(update_stat)(gb_t *g) {
    bool lyc = g->ly == g->io[0x45];
    bool line = (lyc && (STAT & 0x40)) ||
                (g->mode == M_HBLANK && (STAT & 0x08)) ||
                (g->mode == M_VBLANK && (STAT & 0x10)) ||
                (g->mode == M_OAM && (STAT & 0x20));
    if (line && !g->stat_line) g->io[0x0F] |= 2;  // interrupts fire on the rising edge
    g->stat_line = line;
}

void gb_ppu_reset(gb_t *g) {
    if (!spread[1])
        for (int b = 0; b < 256; b++) {
            for (int i = 0; i < 8; i++)
                if (b & (1 << i)) spread[b] |= 1 << (2 * i);
            for (int i = 0; i < 4; i++) expand4[b] |= (uint32_t)((b >> (6 - 2 * i)) & 3) << (8 * i);
        }
    g->ly = 0;
    g->dot = 0;
    g->mode = M_OAM;
    g->ppu_next = 80;
    g->win_line = 0;
    g->win_triggered = false;
    g->lcd_was_on = true;
    g->stat_line = false;
}

uint8_t GB_FAST(gb_ppu_read_stat)(gb_t *g) {
    uint8_t s = 0x80 | (STAT & 0x78);
    if (!(LCDC & 0x80)) return s;
    return s | (g->ly == g->io[0x45] ? 4 : 0) | g->mode;
}

void GB_FAST(gb_ppu_write)(gb_t *g, uint8_t reg, uint8_t v) {
    switch (reg) {
    case 0x40: g->io[0x40] = v; g->ppu_next = 0; break;  // forces an event so LCD on/off takes effect right away
    case 0x41: STAT = v & 0x78; update_stat(g); break;
    case 0x44: break;  // LY is read-only
    case 0x45: g->io[0x45] = v; update_stat(g); break;
    default: g->io[reg] = v;
    }
}

// Colour indices of one tile row, leftmost pixel first.
GB_INLINE void decode8(const uint8_t *row, uint8_t *dst) {
    uint32_t px = spread[row[0]] | spread[row[1]] << 1;
    dst[0] = px >> 14;
    dst[1] = (px >> 12) & 3;
    dst[2] = (px >> 10) & 3;
    dst[3] = (px >> 8) & 3;
    dst[4] = (px >> 6) & 3;
    dst[5] = (px >> 4) & 3;
    dst[6] = (px >> 2) & 3;
    dst[7] = px & 3;
}

// Tile data for BG/window tile number `t` (addressing per LCDC bit 4).
GB_INLINE const uint8_t *bg_tile(const gb_t *g, uint8_t lcdc, uint8_t t) {
    return (lcdc & 0x10) ? g->vram + t * 16 : g->vram + 0x1000 + (int8_t)t * 16;
}

static void GB_NO_LIBCALLS GB_FAST(render_line)(gb_t *g) {
    uint8_t ly = g->ly, lcdc = LCDC;
    uint8_t buf[21 * 8 + 8];  // a partial first tile + 20 more
    const uint8_t *bgi = buf; // raw BG/window colour indices, kept for sprite priority

    if (!g->win_triggered && ly == g->io[0x4A]) g->win_triggered = true;
    if (lcdc & 1) {
        uint8_t y = ly + g->io[0x42], scx = g->io[0x43];
        const uint8_t *map = g->vram + ((lcdc & 0x08) ? 0x1C00 : 0x1800) + (y >> 3) * 32;
        int row = (y & 7) * 2, tx = scx >> 3;
        for (int t = 0; t < 21; t++) decode8(bg_tile(g, lcdc, map[(tx + t) & 31]) + row, buf + t * 8);
        bgi = buf + (scx & 7);

        int wx = g->io[0x4B] - 7;
        if ((lcdc & 0x20) && g->win_triggered && wx < GB_W) {
            uint8_t wy = g->win_line++;
            const uint8_t *wmap = g->vram + ((lcdc & 0x40) ? 0x1C00 : 0x1800) + (wy >> 3) * 32;
            int start = wx < 0 ? 0 : wx;
            uint8_t *dst = buf + (scx & 7) + start;
            int wrow = (wy & 7) * 2, wpx = start - wx;  // wpx: window pixel at the first column it covers
            uint8_t tmp[8];
            for (int x = start; x < GB_W;) {
                decode8(bg_tile(g, lcdc, wmap[wpx >> 3]) + wrow, tmp);
                int from = wpx & 7, n = 8 - from;
                if (n > GB_W - x) n = GB_W - x;
                for (int i = 0; i < n; i++) dst[i] = tmp[from + i];
                dst += n; x += n; wpx += n;
            }
        }
    } else {
        for (int x = 0; x < GB_W; x++) buf[x] = 0;  // BG and window off: colour 0
    }

    uint8_t *out = g->fb + ly * GB_W;
    uint8_t bgp = g->io[0x47];
    const uint8_t pal[4] = {(uint8_t)(bgp & 3), (uint8_t)((bgp >> 2) & 3), (uint8_t)((bgp >> 4) & 3), (uint8_t)(bgp >> 6)};
    for (int x = 0; x < GB_W; x++) out[x] = pal[bgi[x]];

    if (!(lcdc & 2)) return;

    // The first 10 sprites on the line in OAM order, then sorted by X: on the
    // original Game Boy the lower X wins and ties go to the lower OAM index.
    int h = (lcdc & 4) ? 16 : 8;
    uint8_t sel[10];
    int n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        int sy = g->oam[i * 4] - 16;
        if (ly >= sy && ly < sy + h) sel[n++] = i;
    }
    if (!n) return;
    for (int i = 1; i < n; i++) {  // stable insertion sort by X
        uint8_t s = sel[i];
        int j = i - 1;
        while (j >= 0 && g->oam[sel[j] * 4 + 1] > g->oam[s * 4 + 1]) { sel[j + 1] = sel[j]; j--; }
        sel[j + 1] = s;
    }

    uint32_t taken[5] = {0};  // bit x: a higher-priority sprite already has column x
    for (int k = 0; k < n; k++) {
        const uint8_t *o = g->oam + sel[k] * 4;
        int sx = o[1] - 8, row = ly - (o[0] - 16);
        uint8_t tile = o[2], attr = o[3];
        if (h == 16) tile &= 0xFE;
        if (attr & 0x40) row = h - 1 - row;
        uint8_t c[8];
        decode8(g->vram + tile * 16 + row * 2, c);
        uint8_t op = g->io[(attr & 0x10) ? 0x49 : 0x48];
        bool behind = attr & 0x80, flip = attr & 0x20;
        for (int i = 0; i < 8; i++) {
            int x = sx + i;
            if (x < 0 || x >= GB_W || TAKEN(x)) continue;
            uint8_t ci = c[flip ? 7 - i : i];
            // transparent pixels let a lower-priority sprite through; an opaque one
            // claims the column even when it ends up hidden behind the background
            if (!ci) continue;
            TAKE(x);
            if (behind && bgi[x]) continue;
            out[x] = (op >> (ci * 2)) & 3;
        }
    }
}

// ---------------------------------------------------------------- Game Boy Color

// One tile row with its map attribute applied (bank, flips) as finished line
// values, written as two words: bit 7 the attribute's BG-over-sprite flag,
// bits 2-4 the palette, bits 0-1 the colour.
GB_INLINE void cgb_tile(const gb_t *g, uint8_t lcdc, uint8_t tile, uint8_t attr, int y, uint32_t *dst) {
    const uint8_t *base = (attr & 0x08) ? g->vram + 0x2000 : g->vram;
    const uint8_t *data = (lcdc & 0x10) ? base + tile * 16 : base + 0x1000 + (int8_t)tile * 16;
    if (attr & 0x40) y = 7 - y;
    uint32_t px = spread[data[y * 2]] | spread[data[y * 2 + 1]] << 1;
    uint32_t hi = ((attr & 0x80) | (attr & 7) << 2) * 0x01010101u;
    if (attr & 0x20) {  // X flip: byte-reverse each half and swap them
        dst[0] = __builtin_bswap32(expand4[px & 0xFF]) | hi;
        dst[1] = __builtin_bswap32(expand4[px >> 8]) | hi;
    } else {
        dst[0] = expand4[px >> 8] | hi;
        dst[1] = expand4[px & 0xFF] | hi;
    }
}

// dst[i] = the four bytes at byte offset off + 4i of src. Both are word
// aligned; an unaligned offset is handled by shifting neighbouring words.
GB_INLINE void copy_words(uint32_t *dst, const uint32_t *src, int off, int n) {
    src += off >> 2;
    int sh = (off & 3) * 8;
    if (!sh) {
        for (int i = 0; i < n; i++) dst[i] = src[i];
        return;
    }
    uint32_t a = src[0];
    for (int i = 0; i < n; i++) {
        uint32_t b = src[i + 1];
        dst[i] = a >> sh | b << (32 - sh);
        a = b;
    }
}

// Pixels: BG = palette * 4 + colour (0..31), sprites 32 + palette * 4 + colour.
// The line is built straight in the frame buffer, with BG pixels keeping their
// priority bit (7) until the sprites are done.
static void GB_NO_LIBCALLS GB_FAST(render_line_cgb)(gb_t *g) {
    uint8_t ly = g->ly, lcdc = LCDC;
    if (!g->win_triggered && ly == g->io[0x4A]) g->win_triggered = true;
    int wx = g->io[0x4B] - 7;
    bool win = (lcdc & 0x20) && g->win_triggered && wx < GB_W;
    int start = !win ? GB_W : wx < 0 ? 0 : wx;  // first column the window covers
    int ws = start >> 2;                        // first output word with window pixels
    uint32_t *out = (uint32_t *)(g->fb + ly * GB_W);
    uint8_t *out8 = (uint8_t *)out;

    // Background, only as far as the window leaves it visible. On the Color,
    // LCDC bit 0 doesn't hide it: it only decides whether it can cover sprites.
    uint32_t bgw[44];  // 21 tiles, plus a word the shifted copy may read past them
    uint8_t y = ly + g->io[0x42], scx = g->io[0x43];
    int moff = ((lcdc & 0x08) ? 0x1C00 : 0x1800) + (y >> 3) * 32, tx = scx >> 3;
    int tiles = (start + (scx & 7) + 7) >> 3;
    for (int t = 0; t < tiles; t++) {
        int m = moff + ((tx + t) & 31);
        cgb_tile(g, lcdc, g->vram[m], g->vram[0x2000 + m], y & 7, bgw + t * 2);
    }
    copy_words(out, bgw, scx & 7, win ? ws : GB_W / 4);

    if (win) {
        // Window pixel p is at byte 4 + p of ww; the leading word lets the
        // shifted copy start a few pixels left of the window.
        uint32_t ww[44];
        uint8_t wy = g->win_line++;
        int wmoff = ((lcdc & 0x40) ? 0x1C00 : 0x1800) + (wy >> 3) * 32;
        int wtiles = ((GB_W - 1 - wx) >> 3) + 1;
        for (int t = 0; t < wtiles; t++) {
            int m = wmoff + t;
            cgb_tile(g, lcdc, g->vram[m], g->vram[0x2000 + m], wy & 7, ww + 1 + t * 2);
        }
        copy_words(out + ws, ww, 4 + ws * 4 - wx, GB_W / 4 - ws);
        const uint8_t *bg8 = (const uint8_t *)bgw + (scx & 7);
        for (int x = ws * 4; x < start; x++) out8[x] = bg8[x];  // the word shared with the background
    }

    if (lcdc & 2) {
        int h = (lcdc & 4) ? 16 : 8;
        uint8_t cover = (lcdc & 1) ? 0x83 : 0;  // which BG bits can hide a sprite pixel
        uint32_t taken[5] = {0};
        int n = 0;
        for (int i = 0; i < 40 && n < 10; i++) {  // on the Color, OAM order is also priority order
            const uint8_t *o = g->oam + i * 4;
            int sy = o[0] - 16;
            if (ly < sy || ly >= sy + h) continue;
            n++;
            int sx = o[1] - 8, row = ly - sy;
            if (sx <= -8 || sx >= GB_W) continue;
            uint8_t tile = o[2], attr = o[3];
            if (h == 16) tile &= 0xFE;
            if (attr & 0x40) row = h - 1 - row;
            uint8_t sc[8];
            decode8(g->vram + ((attr & 0x08) ? 0x2000 : 0) + tile * 16 + row * 2, sc);
            uint8_t opal = 32 + ((attr & 7) << 2);
            bool flip = attr & 0x20;
            // A sprite pixel loses to a non-zero BG pixel when the BG tile or the
            // sprite asks for it (and LCDC bit 0 allows it at all).
            uint8_t mask = (attr & 0x80) ? (cover & 3) : cover;
            for (int k = 0; k < 8; k++) {
                int x = sx + k;
                if (x < 0 || x >= GB_W || TAKEN(x)) continue;
                uint8_t s = sc[flip ? 7 - k : k];
                if (!s) continue;
                TAKE(x);
                uint8_t b = out8[x] & mask;
                if ((b & 3) && (b & 0x80 || (attr & 0x80))) continue;  // background wins
                out8[x] = opal | s;
            }
        }
    }
    // Drop the BG priority bits. Sprite pixels never have bit 7 set.
    for (int i = 0; i < GB_W / 4; i++) out[i] &= 0x7F7F7F7Fu;
}

void GB_FAST(gb_ppu_event)(gb_t *g) {
    if (!(LCDC & 0x80)) {
        if (g->lcd_was_on) {  // LCD switched off: LY resets, nothing is drawn
            g->lcd_was_on = false;
            g->ly = 0;
            g->mode = M_HBLANK;
            memset(g->fb, 0, GB_W * GB_H);
        }
        g->dot = 0;
        g->ppu_next = 456;  // just keep `dot` from growing
        return;
    }
    if (!g->lcd_was_on) {
        g->lcd_was_on = true;
        g->ly = 0;
        g->dot = 0;
        g->mode = M_OAM;
        g->win_line = 0;
        g->win_triggered = false;
        update_stat(g);
    }

    for (;;) {
        if (g->ly < GB_H) {
            if (g->mode == M_OAM && g->dot >= 80) {
                g->mode = M_DRAW;
                uint32_t t0 = GB_NOW();
                if (g->cgb) {
                    // snapshot palette RAM at line 0: the whole frame is shown with these
                    if (g->ly == 0) {
                        for (int i = 0; i < 64; i++) {
                            g->frame_pal[i] = g->bg_pal[i];
                            g->frame_pal[64 + i] = g->obj_pal[i];
                        }
                    }
                    render_line_cgb(g);
                } else {
                    render_line(g);
                }
                gb_prof_ppu_us += GB_NOW() - t0;
            }
            if (g->mode == M_DRAW && g->dot >= 252) {
                g->mode = M_HBLANK;
                update_stat(g);
                if (g->hdma_active) gb_hdma_hblank(g);
            }
        }
        if (g->dot < 456) break;
        g->dot -= 456;
        g->ly++;
        if (g->ly == GB_H) {
            g->mode = M_VBLANK;
            g->io[0x0F] |= 1;
            g->frame++;
        } else if (g->ly > 153) {
            g->ly = 0;
            g->win_line = 0;
            g->win_triggered = false;
            g->mode = M_OAM;
        } else if (g->ly < GB_H) {
            g->mode = M_OAM;
        }
        update_stat(g);
    }
    g->ppu_next = g->ly >= GB_H ? 456 : g->mode == M_OAM ? 80 : g->mode == M_DRAW ? 252 : 456;
}
