"""Downloads free Game Boy and Game Boy Color homebrew games from Homebrew Hub
(https://hh.gbdev.io), keeping only the ones PicoGB can run.

    python fetch_homebrew.py                   # every game, into ../homebrew
    python fetch_homebrew.py --list            # show what would be fetched
    python fetch_homebrew.py --licensed-only   # only games with an explicit licence
    python fetch_homebrew.py --platform GBC --tag puzzle --max-kb 1024
    python fetch_homebrew.py --upload          # then send them to the Sprig

Games already in the folder are not downloaded again. index.json in the
folder records each game's author, licence and web page.
"""
import argparse
import concurrent.futures
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://hh3.gbdev.io/api"
# The API refuses Python's default User-Agent.
HEADERS = {"User-Agent": "PicoGB-homebrew-fetch/1.0 (+https://github.com/zeyadhost/picogb)"}
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "..", "homebrew")

# Mirrors gb_cart_info() in firmware/PicoGB/gb.cpp.
MAPPERS = {0x00, 0x08, 0x09, 0x01, 0x02, 0x03, 0x05, 0x06, 0x0F, 0x10, 0x11, 0x12, 0x13,
           0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E}
RAM_SIZES = [0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000]
MAX_CART_RAM = 0x8000


def fetch(url, tries=3):
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=60) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError) as e:
            code = getattr(e, "code", None)  # retry network errors, 5xx and 429
            if attempt == tries - 1 or (code and code < 500 and code != 429):
                raise
            time.sleep(2 ** attempt)


def entries(platforms):
    """Every game entry for the given platforms, once each."""
    seen = {}
    for plat in platforms:
        page = 1
        while True:
            q = urllib.parse.urlencode({"type": "game", "platform": plat, "page": page, "page_elements": 10})
            d = json.loads(fetch(f"{API}/search?{q}"))
            for e in d["entries"]:
                e.setdefault("platform", plat)  # a few entries leave it out
                seen.setdefault(e["slug"], e)
            print(f"\rListing {plat} games: page {page} of {d['page_total']}", end="", file=sys.stderr)
            if page >= d["page_total"]:
                break
            page += 1
        print(file=sys.stderr)
    return list(seen.values())


def rom_file(e):
    files = [f for f in e.get("files", []) if f.get("playable")]
    files.sort(key=lambda f: not f.get("default"))
    for f in files:
        if f["filename"].lower().endswith((".gb", ".gbc", ".cgb")):
            return f["filename"]
    return None


def developer(e):
    d = e.get("developer")
    if isinstance(d, list):
        return ", ".join(x.get("name", "?") if isinstance(x, dict) else str(x) for x in d)
    return d or "unknown"


def has_licence(e):
    return bool((e.get("license") or "").strip())


def file_name(e, ext):
    # The Sprig keeps names up to 63 bytes; leave room for the extension.
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "", e["title"]).strip(" .") or e["slug"]
    return name[:56].rstrip(" .") + ext


def check_rom(data):
    """Returns (rom, reason). The ROM is padded to whole 16 KB banks, at least
    32 KB, as a real cartridge would be. reason is set if PicoGB can't run it."""
    if len(data) < 0x150:
        return None, "too small to be a ROM"
    if data[0x147] not in MAPPERS:
        return None, f"cartridge type 0x{data[0x147]:02x} not supported"
    ram = 512 if data[0x147] in (0x05, 0x06) else (RAM_SIZES[data[0x149]] if data[0x149] < 6 else 0)
    if ram > MAX_CART_RAM:
        return None, f"needs {ram // 1024} KB of save RAM"
    size = max(0x8000, -(-len(data) // 0x4000) * 0x4000)
    return data + b"\xff" * (size - len(data)), None


def download(e, out):
    fn = rom_file(e)
    stem = os.path.join(out, file_name(e, ""))
    for ext in (".gb", ".gbc"):
        if os.path.exists(stem + ext):
            return e, stem + ext, "already here"
    # The API's documented /entry/<slug>/<file> route returns 404; the files
    # are served from the database repositories instead.
    rel = f"entries/{e['slug']}/{urllib.parse.quote(fn)}"
    base = e.get("basepath") or "database-gb"
    urls = [f"https://hh3.gbdev.io/static/{base}/{rel}", f"https://raw.githubusercontent.com/gbdev/database/master/{rel}"]
    for url in urls:
        try:
            rom, reason = check_rom(fetch(url))
            break
        except Exception as err:
            error = err
    else:
        return e, None, f"download failed: {error}"
    if reason:
        return e, None, reason
    path = stem + (".gbc" if rom[0x143] & 0x80 else ".gb")  # Color-capable per the header
    with open(path + ".part", "wb") as f:
        f.write(rom)
    os.replace(path + ".part", path)
    return e, path, None


def upload(paths):
    sys.path.insert(0, HERE)
    from gbload import PicoGB
    pg = PicoGB()
    have = {g["name"] for g in pg.list()}
    for i, path in enumerate(paths, 1):
        name = os.path.basename(path)[:63]
        stem = os.path.splitext(name)[0]
        if stem in have or name in have:
            continue
        info = pg.info()
        if info.get("sd") and info["sd_free_kb"] * 1024 < os.path.getsize(path) + 64 * 1024:
            print(f"The SD card is full. Stopped before {name}.")
            break
        print(f"[{i}/{len(paths)}] {name}")
        try:
            pg.upload(path)
        except RuntimeError as err:
            print(f"  not added: {err}")
            if "full" in str(err) or "too many" in str(err):
                break


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=DEFAULT_OUT, help="folder for the games (default: homebrew/)")
    ap.add_argument("--platform", choices=["GB", "GBC", "all"], default="all")
    ap.add_argument("--licensed-only", action="store_true", help="only games that state a licence")
    ap.add_argument("--tag", action="append", default=[], help="only games with this tag (repeatable)")
    ap.add_argument("--max-kb", type=int, help="skip games bigger than this")
    ap.add_argument("--limit", type=int, help="stop after this many games")
    ap.add_argument("--jobs", type=int, default=4, help="downloads at the same time")
    ap.add_argument("--list", action="store_true", help="show the games, download nothing")
    ap.add_argument("--upload", action="store_true", help="send the games to the Sprig afterwards")
    a = ap.parse_args()
    sys.stdout.reconfigure(errors="replace")  # some titles are Japanese; the Windows console may not show them

    games = entries(["GB", "GBC"] if a.platform == "all" else [a.platform])
    tags = {t.lower() for t in a.tag}
    games = [e for e in games if rom_file(e)
             and (not a.licensed_only or has_licence(e))
             and (not tags or tags & {t.lower() for t in e.get("tags", [])})]
    games.sort(key=lambda e: e["title"].lower())
    if a.limit:
        games = games[:a.limit]

    if a.list:
        for e in games:
            print(f"{e['title'][:40]:40}  {e['platform']:3}  {developer(e)[:28]:28}  {e.get('license') or '-'}")
        print(f"{len(games)} games")
        return

    os.makedirs(a.out, exist_ok=True)
    index_path = os.path.join(a.out, "index.json")
    index = json.load(open(index_path, encoding="utf-8")) if os.path.exists(index_path) else {}
    got, skipped = [], []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        jobs = [pool.submit(download, e, a.out) for e in games]
        for n, job in enumerate(concurrent.futures.as_completed(jobs), 1):
            e, path, note = job.result()
            if path and a.max_kb and os.path.getsize(path) > a.max_kb * 1024:
                path, note = None, f"bigger than {a.max_kb} KB"
            if path:
                got.append(path)
                index[os.path.basename(path)] = {
                    "title": e["title"], "developer": developer(e), "license": e.get("license") or None,
                    "platform": e["platform"], "tags": e.get("tags", []), "website": e.get("website"),
                    "homebrew_hub": f"https://hh.gbdev.io/game/{e['slug']}"}
            else:
                skipped.append((e["title"], note))
            print(f"\r{n}/{len(games)} done, {len(skipped)} skipped", end="", file=sys.stderr)
    print(file=sys.stderr)
    with open(index_path, "w", encoding="utf-8") as f:
        json.dump(dict(sorted(index.items())), f, indent=1, ensure_ascii=False)

    total = sum(os.path.getsize(p) for p in got)
    print(f"{len(got)} games in {os.path.abspath(a.out)} ({total / 1048576:.1f} MB)")
    if skipped:
        print(f"{len(skipped)} skipped:")
        for title, note in sorted(skipped):
            print(f"  {title}: {note}")
    if a.upload:
        upload(sorted(got))


if __name__ == "__main__":
    main()
