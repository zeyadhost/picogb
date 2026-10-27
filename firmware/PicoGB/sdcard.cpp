#include "sdcard.h"
#include "sprig_hw.h"
#include "gb.h"
#include <SPI.h>
#include <SdFat.h>
#include <string.h>
#include <ctype.h>

#define PIN_SD_CS 21
#define ROOT "/PicoGB"

static SdFat32 sd;
static File32 rf, wf, sf, gf;
static bool mounted;
static uint32_t free_kb;
static char wpath[96], wtmp[100];

// Holds the shared SPI bus for the scope it is declared in.
struct Bus {
    Bus() { sprig_bus_take(BUS_SD); }
    ~Bus() { sprig_bus_give(BUS_SD); }
};

bool sd_ok(void) { return mounted; }
uint32_t sd_free_kb(void) { return free_kb; }

bool sd_init(void) {
    Bus b;
    SPI.setRX(16);
    SPI.setTX(19);
    SPI.setSCK(18);
    mounted = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_HZ, &SPI));
    if (!mounted) return false;
    static const char *dirs[] = {ROOT, ROOT "/games", ROOT "/saves", ROOT "/states", ROOT "/shots"};
    for (const char *d : dirs)
        if (!sd.exists(d)) sd.mkdir(d);
    int32_t fc = sd.freeClusterCount();
    free_kb = fc > 0 ? (uint32_t)((uint64_t)fc * sd.bytesPerCluster() / 1024) : 0;
    return true;
}

void sd_path(char *out, const char *dir, const char *name, const char *ext) {
    snprintf(out, 96, ROOT "/%s/%s%s", dir, name, ext);
}

bool sd_exists(const char *path) {
    if (!mounted) return false;
    Bus b;
    return sd.exists(path);
}

bool sd_remove(const char *path) {
    if (!mounted) return false;
    Bus b;
    File32 f = sd.open(path, O_RDONLY);
    uint32_t size = f ? f.fileSize() : 0;
    f.close();
    bool ok = sd.remove(path);
    if (ok) free_kb += size / 1024;
    return ok;
}

static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int sd_scan(sd_game_t *out, int max) {
    if (!mounted) return 0;
    int n = 0;
    {
        Bus b;
        File32 dir = sd.open(ROOT "/games", O_RDONLY), f;
        char fname[SD_NAME_LEN + 8];
        while (n < max && f.openNext(&dir, O_RDONLY)) {
            f.getName(fname, sizeof fname);
            int len = strlen(fname);
            const char *dot = strrchr(fname, '.');
            bool gb = dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
            if (f.isDir() || !gb || fname[0] == '.' || len - (int)strlen(dot) >= SD_NAME_LEN) { f.close(); continue; }
            sd_game_t *g = &out[n];
            memset(g, 0, sizeof *g);
            memcpy(g->name, fname, dot - fname);
            strncpy(g->ext, dot, sizeof g->ext - 1);
            g->size = f.fileSize();
            uint16_t date = 0, time = 0;
            f.getModifyDateTime(&date, &time);
            g->stamp = (uint32_t)date << 16 | time;
            static uint8_t hdr[0x150];
            if (f.read(hdr, sizeof hdr) == (int)sizeof hdr) {
                uint8_t mbc; bool bat, rtc; uint32_t ram;
                g->err = gb_cart_info(hdr, g->size, &mbc, &bat, &rtc, &ram);
                gb_title(hdr, g->title);
                g->gcsum = hdr[0x14E] << 8 | hdr[0x14F];
            } else {
                g->err = GB_ERR_SIZE;
            }
            f.close();
            n++;
        }
        dir.close();
        char p[96];
        for (int i = 0; i < n; i++) {
            sd_path(p, "saves", out[i].name, ".sav");
            out[i].has_save = sd.exists(p);
        }
    }
    for (int i = 1; i < n; i++) {
        sd_game_t t = out[i];
        int j = i - 1;
        while (j >= 0 && ci_cmp(out[j].name, t.name) > 0) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    return n;
}

// ---------------------------------------------------------------- read

bool sd_read_open(const char *path, uint32_t *size) {
    if (!mounted) return false;
    Bus b;
    rf.close();
    rf = sd.open(path, O_RDONLY);
    if (!rf) return false;
    if (size) *size = rf.fileSize();
    return true;
}

int sd_read(void *buf, uint32_t n) {
    Bus b;
    return rf.read(buf, n);
}

void sd_read_close(void) {
    Bus b;
    rf.close();
}

// ---------------------------------------------------------------- write

bool sd_write_open(const char *path) {
    if (!mounted) return false;
    Bus b;
    wf.close();
    strncpy(wpath, path, sizeof wpath - 1);
    snprintf(wtmp, sizeof wtmp, "%s.tmp", path);
    wf = sd.open(wtmp, O_WRONLY | O_CREAT | O_TRUNC);
    return (bool)wf;
}

bool sd_write(const void *buf, uint32_t n) {
    Bus b;
    return wf.write(buf, n) == n;
}

bool sd_write_commit(void) {
    Bus b;
    uint32_t size = wf.fileSize();
    bool ok = wf.close();
    if (!ok) return false;
    File32 old = sd.open(wpath, O_RDONLY);
    uint32_t old_size = old ? old.fileSize() : 0;
    old.close();
    if (sd.exists(wpath) && !sd.remove(wpath)) return false;
    ok = sd.rename(wtmp, wpath);
    free_kb = free_kb + old_size / 1024 > size / 1024 ? free_kb + old_size / 1024 - size / 1024 : 0;
    return ok;
}

void sd_write_abort(void) {
    Bus b;
    wf.close();
    sd.remove(wtmp);
}

// ---------------------------------------------------------------- battery save

bool sd_save_open(const char *path, const uint8_t *ram, uint32_t size) {
    if (!mounted) return false;
    {
        Bus b;
        sf.close();
        sf = sd.open(path, O_RDWR);
        if (sf && sf.fileSize() == size) return true;
        sf.close();
    }
    if (!sd_store(path, ram, size)) return false;
    Bus b;
    sf = sd.open(path, O_RDWR);
    return (bool)sf;
}

bool sd_save_block(uint32_t off, const uint8_t *data, uint32_t n) {
    Bus b;
    return sf && sf.seekSet(off) && sf.write(data, n) == n;
}

bool sd_save_sync(void) {
    Bus b;
    return sf && sf.sync();
}

void sd_save_close(void) {
    Bus b;
    sf.close();
}

// ---------------------------------------------------------------- small files

int sd_load(const char *path, void *buf, uint32_t max) {
    if (!mounted) return -1;
    Bus b;
    File32 f = sd.open(path, O_RDONLY);
    if (!f) {
        // A power cut between removing the old file and renaming the new one
        // leaves only the .tmp: use it.
        char tmp[100];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        f = sd.open(tmp, O_RDONLY);
        if (!f) return -1;
    }
    int n = f.read(buf, max);
    f.close();
    return n;
}

bool sd_store(const char *path, const void *buf, uint32_t n) {
    if (!sd_write_open(path)) return false;
    if (!sd_write(buf, n)) { sd_write_abort(); return false; }
    return sd_write_commit();
}

bool sd_append_line(const char *path, const char *line) {
    if (!mounted) return false;
    Bus b;
    File32 f = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (!f) return false;
    f.write(line, strlen(line));
    f.write("\r\n", 2);
    return f.close();
}

// ---------------------------------------------------------------- game ROM

// The ROM is read straight from the card's sectors. A seek on FAT walks the
// cluster chain from the start of the file, which takes tens of
// milliseconds for a big ROM, so at open the part of the file that will be
// read is mapped once into runs of consecutive sectors.
#define ROM_RUNS 64
static struct { uint32_t off, len, sector; } runs[ROM_RUNS];  // file bytes [off, off + len) start at `sector`
static int nruns;

static bool map_runs(uint32_t from, uint32_t size) {
    nruns = 0;
    Sector_t first, last;
    if (gf.contiguousRange(&first, &last)) {
        runs[nruns++] = {0, size, first};
        return true;
    }
    uint32_t cs = sd.bytesPerCluster(), spc = sd.sectorsPerCluster();
    for (uint32_t pos = from & ~(cs - 1); pos < size; pos += cs) {
        if (!gf.seekSet(pos + 1)) return false;  // leaves curCluster() at the cluster holding `pos`
        uint32_t sector = sd.dataStartSector() + (gf.curCluster() - 2) * spc;
        if (nruns && runs[nruns - 1].off + runs[nruns - 1].len == pos &&
            runs[nruns - 1].sector + runs[nruns - 1].len / 512 == sector) {
            runs[nruns - 1].len += cs;
        } else {
            if (nruns == ROM_RUNS) return false;
            runs[nruns++] = {pos, cs, sector};
        }
    }
    return true;
}

bool sd_rom_open(const char *path, uint32_t from, int *pieces) {
    if (!mounted) return false;
    Bus b;
    gf.close();
    gf = sd.open(path, O_RDONLY);
    if (!gf) return false;
    if (!map_runs(from, gf.fileSize())) nruns = 0;  // too fragmented: fall back to seeking
    *pieces = nruns;
    return true;
}

bool sd_rom_read(uint32_t off, void *buf, uint32_t n) {
    Bus b;
    uint8_t *dst = (uint8_t *)buf;
    if ((off | n) & 511) nruns = 0;
    while (n && nruns) {
        int i = 0;
        while (i < nruns && !(off >= runs[i].off && off < runs[i].off + runs[i].len)) i++;
        if (i == nruns) break;
        uint32_t k = runs[i].off + runs[i].len - off;
        if (k > n) k = n;
        if (!sd.card()->readSectors(runs[i].sector + (off - runs[i].off) / 512, dst, k / 512)) return false;
        off += k;
        dst += k;
        n -= k;
    }
    return !n || (gf.seekSet(off) && gf.read(dst, n) == (int)n);
}

void sd_rom_close(void) {
    Bus b;
    gf.close();
    nruns = 0;
}

// Only paths inside /PicoGB, without "..", are reachable from the loader.
static bool safe_path(char *out, const char *rel) {
    if (strstr(rel, "..") || strlen(rel) > 80) return false;
    snprintf(out, 96, ROOT "/%s", rel[0] == '/' ? rel + 1 : rel);
    return true;
}

int sd_read_at(const char *rel, uint32_t off, void *buf, uint32_t n, uint32_t *size) {
    char p[96];
    if (!mounted || !safe_path(p, rel)) return -1;
    Bus b;
    File32 f = sd.open(p, O_RDONLY);
    if (!f || f.isDir()) return -1;
    *size = f.fileSize();
    int got = 0;
    if (off < *size && f.seekSet(off)) got = f.read(buf, n);
    f.close();
    return got;
}

int sd_list(const char *rel, char *out, int max) {
    char p[96];
    if (!mounted || !safe_path(p, rel)) return -1;
    Bus b;
    File32 dir = sd.open(p, O_RDONLY), f;
    if (!dir || !dir.isDir()) return -1;
    int n = 0;
    char name[SD_NAME_LEN + 8];
    while (n < max - 90 && f.openNext(&dir, O_RDONLY)) {
        f.getName(name, sizeof name);
        n += snprintf(out + n, max - n, "%s\t%lu\t%c\n", name, (unsigned long)f.fileSize(), f.isDir() ? 'd' : 'f');
        f.close();
    }
    dir.close();
    return n;
}

int sd_diag(char *out, int max) {
    int n = snprintf(out, max, "mounted=%d err=0x%02x data=0x%02x free=%luKB fat=%d cluster=%lu\n", mounted,
                     sd.sdErrorCode(), sd.sdErrorData(), (unsigned long)free_kb, mounted ? sd.fatType() : 0,
                     mounted ? (unsigned long)sd.bytesPerCluster() : 0ul);
    if (!mounted) return n;
    Bus b;
    static const char *dirs[] = {ROOT, ROOT "/games", ROOT "/saves", ROOT "/states", ROOT "/shots"};
    for (const char *d : dirs) {
        bool e = sd.exists(d);
        bool made = e || sd.mkdir(d);
        n += snprintf(out + n, max - n, "%s exists=%d mkdir=%d err=0x%02x\n", d, e, made, sd.sdErrorCode());
    }
    File32 t = sd.open(ROOT "/games/diag test (1).gb.tmp", O_WRONLY | O_CREAT | O_TRUNC);
    n += snprintf(out + n, max - n, "create long name: %d err=0x%02x\n", (bool)t, sd.sdErrorCode());
    if (t) { t.close(); sd.remove(ROOT "/games/diag test (1).gb.tmp"); }
    File32 dir = sd.open(ROOT, O_RDONLY), f;
    char name[64];
    while (n < max - 80 && f.openNext(&dir, O_RDONLY)) {
        f.getName(name, sizeof name);
        n += snprintf(out + n, max - n, "  %s%s %lu\n", name, f.isDir() ? "/" : "", (unsigned long)f.fileSize());
        f.close();
    }
    dir.close();
    return n;
}

void sd_clean_name(char *dst, const char *src, int max) {
    int n = 0;
    for (; *src && n < max - 1; src++) {
        unsigned char c = *src;
        if (c < 32 || strchr("\\/:*?\"<>|", c)) c = '_';
        dst[n++] = c;
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '.')) n--;  // FAT doesn't allow trailing dots or spaces
    if (!n) dst[n++] = '_';
    dst[n] = 0;
}
