// Probes the Sprig's ST7735 LCD. loop() runs a visual test of different ways
// to init the panel (see below). report() and try_spi() read the panel's ID
// and status registers and print them over USB serial, before and after an
// init at a given SPI clock. The reads are bit-banged because the panel's
// SDA line is bidirectional.
#include <initializer_list>
#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <hardware/clocks.h>

#define PIN_SCK   18
#define PIN_MOSI  19
#define PIN_CS    20
#define PIN_DC    22
#define PIN_RST   26
#define PIN_BL    17
#define PIN_SD_CS 21

// Puts every pin back to plain GPIO for bit-banging.
static void pins_gpio() {
    const int out[] = {PIN_SCK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST, PIN_BL, PIN_SD_CS};
    for (int p : out) { gpio_init(p); gpio_set_dir(p, GPIO_OUT); }
    gpio_put(PIN_SD_CS, 1); gpio_put(PIN_CS, 1); gpio_put(PIN_SCK, 0); gpio_put(PIN_BL, 1);
}

static void bb_delay() { busy_wait_us_32(2); }

static void bb_write8(uint8_t v) {
    for (int i = 7; i >= 0; i--) {
        gpio_put(PIN_MOSI, (v >> i) & 1);
        bb_delay(); gpio_put(PIN_SCK, 1); bb_delay(); gpio_put(PIN_SCK, 0);
    }
}

static void bb_cmd(uint8_t c, const uint8_t *d = nullptr, int n = 0) {
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0); bb_write8(c);
    gpio_put(PIN_DC, 1);
    for (int i = 0; i < n; i++) bb_write8(d[i]);
    gpio_put(PIN_CS, 1);
}

// Sends a read command, then clocks in what the panel drives on SDA. dummy
// adds one extra clock before the data.
static uint32_t bb_read(uint8_t c, int bits, bool dummy) {
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0); bb_write8(c);
    gpio_set_dir(PIN_MOSI, GPIO_IN);
    gpio_pull_down(PIN_MOSI);  // reads 0 if nothing drives the line
    gpio_put(PIN_DC, 1);
    if (dummy) { bb_delay(); gpio_put(PIN_SCK, 1); bb_delay(); gpio_put(PIN_SCK, 0); }
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        bb_delay(); gpio_put(PIN_SCK, 1); bb_delay();
        v = v << 1 | gpio_get(PIN_MOSI);
        gpio_put(PIN_SCK, 0);
    }
    gpio_put(PIN_CS, 1);
    gpio_disable_pulls(PIN_MOSI);
    gpio_set_dir(PIN_MOSI, GPIO_OUT);
    return v;
}

// Same as bb_read with a choice of pull-up or pull-down. Reading both ways
// tells a line driven low apart from one left floating.
static uint32_t read_pulled(uint8_t c, int bits, bool dummy, bool up) {
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0); bb_write8(c);
    gpio_set_dir(PIN_MOSI, GPIO_IN);
    if (up) gpio_pull_up(PIN_MOSI); else gpio_pull_down(PIN_MOSI);
    gpio_put(PIN_DC, 1);
    if (dummy) { bb_delay(); gpio_put(PIN_SCK, 1); bb_delay(); gpio_put(PIN_SCK, 0); }
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        bb_delay(); gpio_put(PIN_SCK, 1); bb_delay();
        v = v << 1 | gpio_get(PIN_MOSI);
        gpio_put(PIN_SCK, 0);
    }
    gpio_put(PIN_CS, 1);
    gpio_disable_pulls(PIN_MOSI);
    gpio_set_dir(PIN_MOSI, GPIO_OUT);
    return v;
}

static void hw_reset() {
    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(10);
    gpio_put(PIN_RST, 1); sleep_ms(120);
}

static void report(const char *when) {
    pins_gpio();
    Serial.printf("[%s]\n", when);
    Serial.printf("  RDDID   pull-down %06lx  pull-up %06lx\n",
                  (unsigned long)read_pulled(0x04, 24, true, false), (unsigned long)read_pulled(0x04, 24, true, true));
    Serial.printf("  RDDST   pull-down %08lx  pull-up %08lx\n",
                  (unsigned long)read_pulled(0x09, 32, true, false), (unsigned long)read_pulled(0x09, 32, true, true));
    Serial.printf("  MADCTL  %02lx   COLMOD %02lx   POWER MODE %02lx\n",
                  (unsigned long)bb_read(0x0B, 8, true), (unsigned long)bb_read(0x0C, 8, true),
                  (unsigned long)bb_read(0x0A, 8, true));
}

// The panel init sequence the firmware uses (see firmware/PicoGB/sprig_hw.cpp),
// sent through cmd so it can run either bit-banged or over the SPI peripheral.

static void spi_cmd(uint8_t cmd, std::initializer_list<uint8_t> d = {}) {
    gpio_put(PIN_DC, 0);
    spi_write_blocking(spi0, &cmd, 1);
    gpio_put(PIN_DC, 1);
    if (d.size()) spi_write_blocking(spi0, d.begin(), d.size());
}

static void init_seq(void (*cmd)(uint8_t, std::initializer_list<uint8_t>)) {
    cmd(0x01, {}); sleep_ms(150);
    cmd(0x11, {}); sleep_ms(500);
    cmd(0xB1, {0x01, 0x2C, 0x2D});
    cmd(0xB2, {0x01, 0x2C, 0x2D});
    cmd(0xB3, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D});
    cmd(0xB4, {0x07});
    cmd(0xC0, {0xA2, 0x02, 0x84});
    cmd(0xC1, {0xC5});
    cmd(0xC2, {0x0A, 0x00});
    cmd(0xC3, {0x8A, 0x2A});
    cmd(0xC4, {0x8A, 0xEE});
    cmd(0xC5, {0x0E});
    cmd(0x20, {});
    cmd(0x36, {0x10});
    cmd(0x3A, {0x05});
    cmd(0xE0, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10});
    cmd(0xE1, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10});
    cmd(0x13, {}); sleep_ms(10);
    cmd(0x29, {}); sleep_ms(100);
}

static void spi_fill(uint16_t color) {
    spi_cmd(0x2A, {0, 0, 0, 127});
    spi_cmd(0x2B, {0, 0, 0, 159});
    spi_cmd(0x2C);
    uint8_t px[2] = {(uint8_t)(color >> 8), (uint8_t)color};
    for (int i = 0; i < 128 * 160; i++) spi_write_blocking(spi0, px, 2);
}

static void try_spi(uint32_t hz, bool peri_from_sys, uint16_t color, const char *name) {
    pins_gpio();
    hw_reset();
    if (peri_from_sys)
        clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys), clock_get_hz(clk_sys));
    uint32_t got = spi_init(spi0, hz);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_put(PIN_CS, 0);
    init_seq(spi_cmd);
    spi_fill(color);
    gpio_put(PIN_CS, 1);
    Serial.printf("\n== %s: SPI init at %lu Hz (clk_peri %lu Hz), filled %04x\n", name, (unsigned long)got,
                  (unsigned long)clock_get_hz(clk_peri), color);
    report("after SPI init");
    spi_deinit(spi0);
}

static void bb_cmd_list(uint8_t c, std::initializer_list<uint8_t> d) { bb_cmd(c, d.begin(), d.size()); }

// Visual test. It loops forever and needs no serial connection. Each step
// resets the panel, inits it one way, fills it with one color and holds that
// for 4 s. Before each step, the two Sprig LEDs (GP4 blue, GP28 white) blink
// the step number.
#define LED_BLUE  4
#define LED_WHITE 28

static void blink(int n) {
    for (int i = 0; i < n; i++) {
        gpio_put(LED_BLUE, 1); gpio_put(LED_WHITE, 1); sleep_ms(200);
        gpio_put(LED_BLUE, 0); gpio_put(LED_WHITE, 0); sleep_ms(250);
    }
}

static void bb_fill(uint16_t color) {
    const uint8_t ca[4] = {0, 0, 0, 127}, ra[4] = {0, 0, 0, 159};
    bb_cmd(0x2A, ca, 4);
    bb_cmd(0x2B, ra, 4);
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0); bb_write8(0x2C);
    gpio_put(PIN_DC, 1);
    for (int i = 0; i < 128 * 160; i++) { bb_write8(color >> 8); bb_write8(color); }
    gpio_put(PIN_CS, 1);
}

// Like try_spi, but leaves the image on screen instead of reporting, and puts
// clk_peri back on the 48 MHz USB PLL when not running it from clk_sys.
static void spi_step(uint32_t hz, bool peri_from_sys, uint16_t color) {
    pins_gpio();
    hw_reset();
    if (peri_from_sys)
        clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, clock_get_hz(clk_sys), clock_get_hz(clk_sys));
    else
        clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, 48000000, 48000000);
    spi_init(spi0, hz);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_put(PIN_CS, 0);
    init_seq(spi_cmd);
    spi_fill(color);
}

void setup() {
    Serial.begin(115200);
    gpio_init(LED_BLUE);  gpio_set_dir(LED_BLUE, GPIO_OUT);
    gpio_init(LED_WHITE); gpio_set_dir(LED_WHITE, GPIO_OUT);
}

void loop() {
    // 1, red: slow bit-banged init, toggling CS per command like Sprig's driver
    blink(1);
    pins_gpio(); hw_reset(); init_seq(bb_cmd_list); bb_fill(0xF800);
    Serial.println("step 1 red: bit-banged");
    sleep_ms(4000);

    // 2, green: SPI peripheral at 4 MHz
    blink(2);
    spi_step(4000000, false, 0x07E0);
    Serial.println("step 2 green: SPI 4 MHz");
    sleep_ms(4000);

    // 3, blue: the SPI clock Sprig's own firmware uses, 20.8 MHz
    blink(3);
    spi_step(21000000, true, 0x001F);
    Serial.println("step 3 blue: SPI 20.8 MHz");
    sleep_ms(4000);
    spi_deinit(spi0);
}
