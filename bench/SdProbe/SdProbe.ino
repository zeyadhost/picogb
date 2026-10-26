// Probes the Sprig's microSD slot (SPI0, CS on GP21, shared with the LCD) and
// reports over USB serial which SPI clocks the card accepts and how fast it
// reads and writes at each. Send any byte to run it; the output ends with
// "DONE".
#include <SPI.h>
#include <SdFat.h>
#include <hardware/clocks.h>

#define PIN_SD_CS 21
#define PIN_LCD_CS 20

SdFat32 sd;
static uint8_t buf[4096];

// Writes 256 KB to a file in /PicoGB, reads it back, and prints both speeds
// and the number of bytes that didn't match.
static void speed_test() {
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = i * 7;
    sd.mkdir("/PicoGB");
    File32 w = sd.open("/PicoGB/speedtest.bin", O_WRONLY | O_CREAT | O_TRUNC);
    uint32_t t0 = micros();
    for (int i = 0; i < 64; i++) w.write(buf, sizeof buf);
    w.close();
    uint32_t wus = micros() - t0;

    File32 r = sd.open("/PicoGB/speedtest.bin", O_RDONLY);
    t0 = micros();
    uint32_t bad = 0, n, total = 0;
    while ((n = r.read(buf, sizeof buf)) > 0) {
        for (uint32_t i = 0; i < n; i++) bad += buf[i] != (uint8_t)(i * 7);
        total += n;
    }
    uint32_t rus = micros() - t0;
    r.close();
    sd.remove("/PicoGB/speedtest.bin");
    Serial.printf("    write %lu KB/s, read %lu KB/s, %lu of %lu bytes bad\n", (unsigned long)(256000000ull / wus),
                  (unsigned long)(256000000ull / rus), (unsigned long)bad, (unsigned long)total);
}

static void run() {
    gpio_init(PIN_LCD_CS); gpio_set_dir(PIN_LCD_CS, GPIO_OUT); gpio_put(PIN_LCD_CS, 1);  // keep the LCD off the bus
    // Run clk_peri from clk_sys, as the firmware does, so SPI can go above 12 MHz.
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys), clock_get_hz(clk_sys));
    SPI.setRX(16); SPI.setTX(19); SPI.setSCK(18);
    const uint32_t speeds[] = {20900000, 17900000, 15700000, 12000000};
    for (uint32_t hz : speeds) {
        sd.end();
        bool ok = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, hz, &SPI));
        Serial.printf("asked %lu Hz, got %lu Hz: %s", (unsigned long)hz, (unsigned long)spi_get_baudrate(spi0),
                      ok ? "OK\n" : "FAILED");
        if (!ok) { Serial.printf(" (err 0x%02x)\n", sd.sdErrorCode()); continue; }
        for (int rep = 0; rep < 3; rep++) speed_test();
    }
    Serial.printf("free: %lu KB\n", (unsigned long)((uint64_t)sd.freeClusterCount() * sd.bytesPerCluster() / 1024));
}

void setup() { Serial.begin(115200); }
void loop() {
    if (!Serial || Serial.available() <= 0) { delay(10); return; }
    while (Serial.available() > 0) Serial.read();
    run();
    Serial.println("DONE");
}
