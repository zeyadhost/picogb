// Runs a ROM in the PicoGB core on the PC. Whatever the ROM writes to the
// serial port is printed (Blargg's test ROMs report their results there), and
// the last screen can be saved as a PGM (PPM for Color games).
//   g++ -O2 -o gbtest gbtest.cpp ../firmware/PicoGB/gb.cpp ../firmware/PicoGB/gb_ppu.cpp ../firmware/PicoGB/gb_apu.cpp
//   gbtest rom.gb [frames] [screen.pgm] [frame:mask ...]
// Each frame:mask sets the held buttons from that frame on. Set GB_STATETEST
// to also check a save-state round trip, or GB_TRACE to dump the CPU state.
#include "../firmware/PicoGB/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static gb_t gb;
static uint8_t fb[GB_W * GB_H] __attribute__((aligned(4))), cart_ram[GB_MAX_CART_RAM];
static char serial[4096];
static int serial_n;

static void on_serial(uint8_t c) {
    if (serial_n < (int)sizeof serial - 1) serial[serial_n++] = c;
    putchar(c);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: gbtest rom.gb [frames] [screen.pgm]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    static uint8_t rom[8 << 20];
    uint32_t size = fread(rom, 1, sizeof rom, f);
    fclose(f);
    int frames = argc > 2 ? atoi(argv[2]) : 3600;

    gb.serial_out = on_serial;
    int err = gb_init(&gb, rom, size, cart_ram, fb);
    if (err) { printf("gb_init error %d\n", err); return 2; }
    char title[17];
    gb_title(rom, title);
    printf("[%s] mapper %d, %u KB ROM, %u B RAM\n", title, gb.mbc, size / 1024, gb.ram_size);

    clock_t t0 = clock();
    long samples = 0;
    int i;
    for (i = 0; i < frames; i++) {
        for (int a = 4; a < argc; a++) {
            int fr, mask;
            if (sscanf(argv[a], "%d:%i", &fr, &mask) == 2 && fr == i) gb_set_buttons(&gb, mask);
        }
        gb_run_frame(&gb);
        samples += gb.audio_n;
        serial[serial_n] = 0;
        if (strstr(serial, "Passed") || strstr(serial, "Failed")) { i++; break; }
    }
    if (getenv("GB_STATETEST")) {
        // Snapshot, play 300 frames, restore the snapshot and play the same
        // 300 frames again. Both runs have to end in the same state.
        static gb_t snap, after;
        static uint8_t snap_ram[GB_MAX_CART_RAM], fb_a[GB_W * GB_H];
        snap = gb;
        memcpy(snap_ram, cart_ram, sizeof cart_ram);
        for (int k = 0; k < 300; k++) { gb_set_buttons(&gb, (k / 40) & 1 ? 0x10 : 0x01); gb_run_frame(&gb); }
        memcpy(fb_a, fb, sizeof fb);
        after = gb;
        // Restore the way the firmware does: copy the struct, then put the
        // pointers back.
        const uint8_t *rom = gb.rom, *b0 = gb.rom_bank0;
        uint8_t *ram = gb.cart_ram, *f = gb.fb;
        void (*so)(uint8_t) = gb.serial_out;
        memcpy(&gb, &snap, sizeof gb);
        gb.rom = rom; gb.rom_bank0 = b0; gb.cart_ram = ram; gb.fb = f; gb.serial_out = so;
        gb_remap(&gb);
        memcpy(cart_ram, snap_ram, sizeof cart_ram);
        for (int k = 0; k < 300; k++) { gb_set_buttons(&gb, (k / 40) & 1 ? 0x10 : 0x01); gb_run_frame(&gb); }
        bool same_fb = !memcmp(fb_a, fb, sizeof fb);
        bool same_cpu = after.pc == gb.pc && after.sp == gb.sp && !memcmp(after.r, gb.r, 8) && after.f == gb.f;
        bool same_mem = !memcmp(after.wram, gb.wram, sizeof gb.wram) && !memcmp(after.vram, gb.vram, sizeof gb.vram);
        printf("\n[state round trip: screen %s, cpu %s, memory %s, gb_t is %u bytes]\n", same_fb ? "same" : "DIFFERENT",
               same_cpu ? "same" : "DIFFERENT", same_mem ? "same" : "DIFFERENT", (unsigned)sizeof(gb_t));
    }
    if (getenv("GB_TRACE")) {
        printf("\nPC=%04x SP=%04x A=%02x F=%02x IE=%02x IF=%02x IME=%d halted=%d TAC=%02x TIMA=%02x TMA=%02x DIV=%04x LCDC=%02x LY=%d\n",
               gb.pc, gb.sp, gb.r[7], gb.f, gb.ie, gb.io[0x0F], gb.ime, gb.halted, gb.io[7], gb.io[5], gb.io[6], gb.div,
               gb.io[0x40], gb.ly);
        for (int k = 0; k < 16; k++) printf("%02x ", gb.rd[(gb.pc + k) >> 12] ? gb.rd[(gb.pc + k) >> 12][(gb.pc + k) & 0xFFF] : 0);
        printf("\n");
    }
    double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("\n[%d frames, %.3f ms/frame on this PC, %.1f audio samples/frame]\n", i, ms / i, (double)samples / i);

    if (argc > 3) {
        FILE *o = fopen(argv[3], "wb");
        // In Color mode the frame buffer holds indexes into this frame's
        // palette RAM (RGB555), so write a colour PPM instead.
        if (gb.cgb) {
            fprintf(o, "P6\n%d %d\n255\n", GB_W, GB_H);
            for (int p = 0; p < GB_W * GB_H; p++) {
                uint16_t c = gb.frame_pal[fb[p] * 2] | gb.frame_pal[fb[p] * 2 + 1] << 8;
                for (int s = 0; s < 15; s += 5) {
                    int v = (c >> s) & 31;
                    fputc(v << 3 | v >> 2, o);
                }
            }
        } else {
            fprintf(o, "P5\n%d %d\n255\n", GB_W, GB_H);
            static const uint8_t shade[4] = {255, 170, 85, 0};
            for (int p = 0; p < GB_W * GB_H; p++) fputc(shade[fb[p] & 3], o);
        }
        fclose(o);
    }
    return strstr(serial, "Failed") ? 1 : 0;
}
