"""PicoGB loader, command-line version (the web page is loader/index.html).

    python gbload.py list
    python gbload.py upload game.gb [more.gb ...]
    python gbload.py run 0             # index from `list`
    python gbload.py delete 0
    python gbload.py quit              # back to the Sprig's menu
    python gbload.py shot out.png      # save what the Sprig is showing
    python gbload.py info
    python gbload.py ls [folder]       # files on the SD card
    python gbload.py get path [out]    # download one of them
    python gbload.py backup [dir]      # everything on the SD card except the games
    python gbload.py perf [reset]      # emulator timing counters

Add --port COM10 if the Sprig isn't found automatically.
"""
import argparse
import os
import struct
import sys
import time
import zlib

import serial
import serial.tools.list_ports

PICO_VID = 0x2E8A
CHUNK = 1024


# Packets both ways are a 3-byte magic ("PGC" to the Sprig, "PGR" back), the
# type letter, a 16-bit payload length, the payload, and a CRC-16 of
# everything after the magic. See firmware/PicoGB/link.h.
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


class PicoGB:
    def __init__(self, port=None, timeout=10.0):
        port = port or self.find_port()
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.ser.dtr = True
        if hasattr(self.ser, "set_buffer_size"):
            self.ser.set_buffer_size(rx_size=1 << 22)
        self.buf = bytearray()
        self.timeout = timeout
        self.last_frame = None
        time.sleep(0.2)

    @staticmethod
    def find_port():
        for p in serial.tools.list_ports.comports():
            if p.vid == PICO_VID:
                return p.device
        raise SystemExit("PicoGB not found: is the Sprig plugged in?")

    def send(self, cmd, payload=b""):
        body = bytes([ord(cmd)]) + struct.pack("<H", len(payload)) + payload
        self.ser.write(b"PGC" + body + struct.pack("<H", crc16(body)))

    def packets(self):
        """Yields (type, payload) for each packet, and (None, None) whenever
        the line goes quiet so callers can time out. Screen packets also
        update last_frame. Packets with a bad CRC are skipped."""
        while True:
            data = self.ser.read(max(1, self.ser.in_waiting))
            if not data:
                yield None, None
                continue
            self.buf += data
            while True:
                k = self.buf.find(b"PGR")
                if k < 0:
                    del self.buf[:max(0, len(self.buf) - 2)]
                    break
                del self.buf[:k]
                if len(self.buf) < 6:
                    break
                n = struct.unpack_from("<H", self.buf, 4)[0]
                if len(self.buf) < 8 + n:
                    break
                body = bytes(self.buf[3:6 + n])
                crc = struct.unpack_from("<H", self.buf, 6 + n)[0]
                if crc16(body) != crc:
                    del self.buf[:1]
                    continue
                del self.buf[:8 + n]
                t, payload = chr(body[0]), body[3:]
                if t == "V":
                    self.last_frame = payload
                yield t, payload

    def request(self, cmd, payload=b"", timeout=None):
        self.send(cmd, payload)
        return self.wait(cmd, timeout)

    def wait(self, cmd, timeout=None):
        end = time.time() + (timeout or self.timeout)
        for t, p in self.packets():
            if t == cmd:
                return p
            if time.time() > end:
                raise TimeoutError(f"no reply to {cmd!r}")

    def check(self, p, skip=0):
        if p[0] != 0:
            raise RuntimeError(p[1 + skip:].decode(errors="replace"))
        return p

    def info(self):
        p = self.request("I")
        ver, state, game, count, free = struct.unpack_from("<BBBBI", p)
        kind, a, b = struct.unpack_from("<III", p, 8)
        info = {"version": ver, "state": ["menu", "game", "paused"][state], "game": game if game != 255 else None,
                "count": count, "flash_free": free,
                "last_reset": [None, f"crash at {a:08x}", f"froze in stage {a}"][kind]}
        if len(p) >= 26:
            info.update(sd=bool(p[20]), sd_free_kb=struct.unpack_from("<I", p, 21)[0], flash_cached=p[25])
        return info

    def list(self):
        p = self.request("L")
        games = []
        for o in range(0, len(p) - 86, 87):
            idx, size, flags = struct.unpack_from("<BIB", p, o)
            name = p[o + 6:o + 70].split(b"\0")[0].decode(errors="replace")
            title = p[o + 70:o + 87].split(b"\0")[0].decode(errors="replace")
            games.append({"index": idx, "size": size, "save": bool(flags & 1), "cached": bool(flags & 2),
                          "unsupported": bool(flags & 4), "name": name, "title": title})
        return games

    def upload(self, path, progress=None, window=8):
        data = open(path, "rb").read()
        name = os.path.basename(path)[:63]
        self.check(self.request("U", struct.pack("<I", len(data)) + name.encode()))
        # Pipelined: keep up to `window` chunks in flight instead of waiting
        # for each reply. USB flow control holds us back if the Sprig falls
        # behind, and its replies come back in order.
        offsets = list(range(0, len(data), CHUNK))
        sent = done = 0
        while done < len(offsets):
            while sent < len(offsets) and sent - done < window:
                off = offsets[sent]
                self.send("D", struct.pack("<I", off) + data[off:off + CHUNK])
                sent += 1
            self.check(self.wait("D"), skip=4)
            done += 1
            if progress:
                progress(min(offsets[done - 1] + CHUNK, len(data)), len(data))
        p = self.check(self.request("F", struct.pack("<I", zlib.crc32(data)), timeout=30), skip=1)
        return p[1]

    def run(self, index):
        # Long timeout: the first launch of a game copies it into flash.
        self.check(self.request("R", bytes([index]), timeout=60))

    def delete(self, index):
        self.check(self.request("X", bytes([index])))

    def quit(self):
        self.request("Q")

    def ls(self, folder=""):
        p = self.check(self.request("O", folder.encode()))
        out = []
        for line in p[1:].decode(errors="replace").splitlines():
            name, size, kind = line.split("\t")
            out.append((name, int(size), kind == "d"))
        return out

    def get(self, path):
        """Downloads a file from /PicoGB on the SD card, one reply at a time."""
        data, size = bytearray(), None
        while size is None or len(data) < size:
            p = self.check(self.request("G", struct.pack("<I", len(data)) + path.encode()))
            size = struct.unpack_from("<I", p, 1)[0]
            chunk = p[5:]
            if not chunk and len(data) < size:
                raise RuntimeError(f"short read on {path}")
            data += chunk
        return bytes(data)

    def perf(self, reset=False):
        p = self.request("A", bytes([1 if reset else 0]))
        v = struct.unpack("<%dI" % (len(p) // 4), p)
        keys = ["frames", "underruns", "slow_frames", "max_emu_us", "max_frame_us", "avg_emu_us", "stalls",
                "max_stall_ms", "avg_ppu_us", "avg_apu_us", "dropped_frames"]
        return dict(zip(keys, v))

    def keys(self, mask):
        self.send("K", bytes([mask]))

    def frame(self, timeout=3.0):
        """Waits for the next screen packet; returns (kind, emu_us, frame_no, rgb rows)."""
        end = time.time() + timeout
        for t, p in self.packets():
            if t == "V":
                return decode_frame(p)
            if time.time() > end:
                raise TimeoutError("no screen packets")


# Color frames arrive half a picture at a time, so each packet updates its
# rows of this buffer.
_cgb_px = [0] * (160 * 144)


def decode_frame(p):
    kind, volume, emu_us, frame_no = struct.unpack_from("<BBHI", p, 0)
    # Color: 64 RGB565 colours, the first row number, then one byte per pixel.
    # Otherwise 16 colours, then a game screen at 2 bits per pixel or the
    # 160x128 menu at 4.
    if kind == 2:
        pal565 = struct.unpack_from("<64H", p, 8)
        pal = [((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31) for c in pal565]
        row0, rows = p[136], p[137:]
        _cgb_px[row0 * 160:row0 * 160 + len(rows)] = rows
        return kind, emu_us, frame_no, 160, 144, list(_cgb_px), pal
    pal565 = struct.unpack_from("<16H", p, 8)
    pal = [((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31) for c in pal565]
    px = p[40:]
    if kind == 0:
        w, h = 160, 144
        idx = [(b >> s) & 3 for b in px for s in (0, 2, 4, 6)]
    else:
        w, h = 160, 128
        idx = [(b >> s) & 15 for b in px for s in (0, 4)]
    return kind, emu_us, frame_no, w, h, idx, pal


def save_png(path, w, h, idx, pal, scale=3):
    os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
    import pygame
    s = pygame.Surface((w, h))
    for i, c in enumerate(idx):
        s.set_at((i % w, i // w), pal[c])
    pygame.image.save(pygame.transform.scale(s, (w * scale, h * scale)), path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("cmd", choices=["list", "upload", "run", "delete", "quit", "shot", "info", "ls", "get",
                                    "backup", "perf"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    gb = PicoGB(a.port)

    if a.cmd == "info":
        print(gb.info())
    elif a.cmd == "list":
        games = gb.list()
        if not games:
            print("no games")
        for g in games:
            tags = " ".join(t for t, on in (("save", g["save"]), ("in-flash", g["cached"]),
                                            ("UNSUPPORTED", g["unsupported"])) if on)
            print(f"{g['index']:2}  {g['name']:<40} {g['size'] // 1024:5} KB  {tags}")
    elif a.cmd == "upload":
        for path in a.args:
            t0 = time.time()

            def show(done, total):
                print(f"\r{os.path.basename(path)}: {done * 100 // total:3}%", end="", flush=True)

            try:
                idx = gb.upload(path, show)
                print(f"\r{os.path.basename(path)}: added as #{idx} ({time.time() - t0:.1f} s)")
            except RuntimeError as e:
                print(f"\r{os.path.basename(path)}: {e}")
    elif a.cmd == "run":
        gb.run(int(a.args[0]))
    elif a.cmd == "delete":
        gb.delete(int(a.args[0]))
    elif a.cmd == "quit":
        gb.quit()
    elif a.cmd == "ls":
        for name, size, is_dir in gb.ls(a.args[0] if a.args else ""):
            print(f"{name}/" if is_dir else f"{name:<50} {size:>9}")
    elif a.cmd == "get":
        data = gb.get(a.args[0])
        out = a.args[1] if len(a.args) > 1 else os.path.basename(a.args[0])
        open(out, "wb").write(data)
        print(f"{a.args[0]} -> {out} ({len(data)} bytes)")
    elif a.cmd == "backup":  # everything but the games themselves
        dest = a.args[0] if a.args else "sprig-backup-" + time.strftime("%Y%m%d-%H%M%S")
        n = 0
        for folder in ["", "saves", "states", "shots"]:
            for name, size, is_dir in gb.ls(folder):
                if is_dir:
                    continue
                rel = f"{folder}/{name}" if folder else name
                os.makedirs(os.path.join(dest, folder), exist_ok=True)
                open(os.path.join(dest, folder, name), "wb").write(gb.get(rel))
                print(f"  {rel} ({size} bytes)")
                n += 1
        print(f"{n} files backed up to {dest}")
    elif a.cmd == "perf":
        print(gb.perf(reset="reset" in a.args))
    elif a.cmd == "shot":
        kind, emu_us, frame_no, w, h, idx, pal = gb.frame()
        save_png(a.args[0] if a.args else "shot.png", w, h, idx, pal)
        print(f"saved {'game' if kind == 0 else 'menu'} frame {frame_no} (emulation {emu_us / 1000:.2f} ms/frame)")


if __name__ == "__main__":
    sys.exit(main())
