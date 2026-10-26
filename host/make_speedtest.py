"""Builds roms/speedtest.gbc, a check for Color double-speed mode. It counts
busy-loop iterations from LY=0 to LY=100, switches to double speed (KEY1 +
STOP) and counts again, then sends over the serial port: count1 (2 bytes),
KEY1 after the switch, count2 (2 bytes). The CPU runs twice as fast in double
speed but LY doesn't, so count2 should be about twice count1."""
code = bytearray()
labels, fixups = {}, []


def emit(*b):
    code.extend(b)


def label(name):
    labels[name] = 0x150 + len(code)


def jr_nz(name):
    emit(0x20, 0)
    fixups.append(("rel", len(code) - 1, name))


def call(name):
    emit(0xCD, 0, 0)
    fixups.append(("abs", len(code) - 2, name))


def count_block():
    call("wait_ly0")
    emit(0x11, 0, 0)            # ld de,0
    label("loop%d" % len(labels))
    name = list(labels)[-1]
    emit(0x13)                  # inc de
    emit(0xF0, 0x44)            # ldh a,(LY)
    emit(0xFE, 100)             # cp 100
    jr_nz(name)
    emit(0x7A); call("ser")     # ld a,d
    emit(0x7B); call("ser")     # ld a,e


count_block()
emit(0x3E, 0x01, 0xE0, 0x4D)    # ld a,1 ; ldh (KEY1),a
emit(0x10, 0x00)                # stop
emit(0xF0, 0x4D); call("ser")   # ldh a,(KEY1)
count_block()
label("end")
emit(0x76, 0x18, 0xFD)          # halt ; jr -3

label("wait_ly0")
emit(0xF0, 0x44, 0xFE, 0x00)    # ldh a,(LY) ; cp 0
jr_nz("wait_ly0")
emit(0xC9)
label("ser")
emit(0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02, 0xC9)  # ldh (SB),a ; ld a,$81 ; ldh (SC),a ; ret

for kind, at, name in fixups:
    target = labels[name]
    if kind == "rel":
        code[at] = (target - (0x150 + at + 1)) & 0xFF
    else:
        code[at], code[at + 1] = target & 0xFF, target >> 8

rom = bytearray(0x8000)
rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])        # nop; jp $0150
rom[0x134:0x13D] = b"SPEEDTEST"
rom[0x143] = 0x80                                          # Color-capable
rom[0x150:0x150 + len(code)] = code
rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF          # header checksum
assert len(rom) == 0x8000
open("roms/speedtest.gbc", "wb").write(rom)
print("roms/speedtest.gbc written")
