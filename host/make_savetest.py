"""Builds roms/savetest.gb, a 32 KB MBC1+RAM+BATTERY ROM for checking battery
saves end to end. Every boot it adds 1 to a counter in cart RAM and shows
(counter & 3) as the background shade, so the shade changes on each restart
only if the save survived."""
rom = bytearray(0x8000)
rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])        # nop; jp $0150
rom[0x134:0x13C] = b"SAVETEST"
rom[0x147] = 0x03                                          # MBC1 + RAM + battery
rom[0x148] = 0x00                                          # 32 KB
rom[0x149] = 0x02                                          # 8 KB RAM
code = bytes([
    0x3E, 0x0A, 0xEA, 0x00, 0x00,   # ld a,$0A ; ld ($0000),a   enable cart RAM
    0xFA, 0x00, 0xA0, 0x3C,         # ld a,($A000) ; inc a
    0xEA, 0x00, 0xA0,               # ld ($A000),a              boot counter
    0xE6, 0x03, 0xE0, 0x47,         # and 3 ; ldh ($47),a       BG colour 0 = counter & 3
    0x76, 0x18, 0xFD,               # halt ; jr -3
])
rom[0x150:0x150 + len(code)] = code
rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF          # header checksum
assert len(rom) == 0x8000
open("roms/savetest.gb", "wb").write(rom)
print("roms/savetest.gb written")
