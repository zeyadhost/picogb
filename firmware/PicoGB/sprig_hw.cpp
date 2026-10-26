#include "sprig_hw.h"
#include <hardware/spi.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/clocks.h>
#include <pico/time.h>
#include <pico/mutex.h>
#include <initializer_list>

#define LCD_SPI   spi0
#define PIN_SCK   18
#define PIN_MOSI  19
#define PIN_CS    20
#define PIN_DC    22
#define PIN_RST   26
#define PIN_BL    17
#define PIN_SD_CS 21   // the microSD card shares the bus, so keep it deselected
// clk_sys / 10. Sprig's stock firmware uses about 20.8 MHz; at 25 MHz every frame gets through.
#define LCD_HZ    25000000

static const uint8_t button_pins[NUM_BUTTONS] = {5, 6, 7, 8, 12, 13, 14, 15};  // W A S D I J K L
static int lcd_dma;
static mutex_t bus;

static void lcd_cmd(uint8_t cmd, std::initializer_list<uint8_t> d = {}) {
    gpio_put(PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &cmd, 1);
    gpio_put(PIN_DC, 1);
    if (d.size()) spi_write_blocking(LCD_SPI, d.begin(), d.size());
}

void sprig_backlight(bool on) { gpio_put(PIN_BL, on); }

void sprig_init(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
    }
    gpio_init(PIN_SD_CS); gpio_set_dir(PIN_SD_CS, GPIO_OUT); gpio_put(PIN_SD_CS, 1);
    gpio_init(PIN_CS);    gpio_set_dir(PIN_CS, GPIO_OUT);    gpio_put(PIN_CS, 1);
    gpio_init(PIN_DC);    gpio_set_dir(PIN_DC, GPIO_OUT);    gpio_put(PIN_DC, 1);
    gpio_init(PIN_RST);   gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_init(PIN_BL);    gpio_set_dir(PIN_BL, GPIO_OUT);    gpio_put(PIN_BL, 0);

    // arduino-pico runs clk_peri from the 48 MHz USB PLL, which caps SPI at
    // 12 MHz. Run it from clk_sys instead.
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
    spi_init(LCD_SPI, LCD_HZ);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(10);
    gpio_put(PIN_RST, 1); sleep_ms(10);
    mutex_init(&bus);
    sprig_bus_take(BUS_LCD);

    // ST7735 init: software reset, sleep out, frame rate, power, gamma, display on.
    lcd_cmd(0x01); sleep_ms(150);
    lcd_cmd(0x11); sleep_ms(500);
    lcd_cmd(0xB1, {0x01, 0x2C, 0x2D});
    lcd_cmd(0xB2, {0x01, 0x2C, 0x2D});
    lcd_cmd(0xB3, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D});
    lcd_cmd(0xB4, {0x07});
    lcd_cmd(0xC0, {0xA2, 0x02, 0x84});
    lcd_cmd(0xC1, {0xC5});
    lcd_cmd(0xC2, {0x0A, 0x00});
    lcd_cmd(0xC3, {0x8A, 0x2A});
    lcd_cmd(0xC4, {0x8A, 0xEE});
    lcd_cmd(0xC5, {0x0E});
    lcd_cmd(0x20);
    // MADCTL: Sprig's firmware uses MX|ML|BGR with its own pixel packing. We send
    // plain big-endian RGB565, which needs BGR off, and rotate in software, so
    // no column mirror either.
    lcd_cmd(0x36, {0x10});
    lcd_cmd(0x3A, {0x05});                                     // RGB565
    lcd_cmd(0xE0, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10});
    lcd_cmd(0xE1, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10});
    lcd_cmd(0x13); sleep_ms(10);
    lcd_cmd(0x29); sleep_ms(100);

    lcd_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(lcd_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
    dma_channel_configure(lcd_dma, &c, &spi_get_hw(LCD_SPI)->dr, nullptr, 0, false);
    sprig_bus_give(BUS_LCD);
}

void sprig_bus_take(int who) {
    mutex_enter_blocking(&bus);
    if (who == BUS_LCD) {
        spi_set_baudrate(LCD_SPI, LCD_HZ);
        gpio_put(PIN_CS, 0);
    } else {
        spi_set_baudrate(LCD_SPI, SD_HZ);
        spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    }
}

void sprig_bus_give(int who) {
    if (who == BUS_LCD) {
        while (sprig_lcd_busy()) tight_loop_contents();
        gpio_put(PIN_CS, 1);  // the LCD must not see SD traffic
        // LCD DMA only transmits, so the RX FIFO fills with junk and overflows.
        // The SD driver would take that junk for the card's replies, so drain it
        // and clear the overrun flag.
        while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
        spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;
    }
    mutex_exit(&bus);
}

bool sprig_lcd_busy(void) { return dma_channel_is_busy(lcd_dma) || spi_is_busy(LCD_SPI); }

void sprig_lcd_rows(int row0, int nrows, const uint16_t *px) {
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    lcd_cmd(0x2A, {0, 0, 0, LCD_COLS - 1});
    lcd_cmd(0x2B, {0, (uint8_t)row0, 0, (uint8_t)(row0 + nrows - 1)});
    lcd_cmd(0x2C);
    spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);  // RGB565, high byte first
    dma_channel_transfer_from_buffer_now(lcd_dma, px, LCD_COLS * nrows);
}

static volatile uint8_t held;

void sprig_buttons_sample(void) {
    static uint8_t hist[NUM_BUTTONS];
    uint32_t all = gpio_get_all();
    uint8_t h = held;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        hist[i] = hist[i] << 1 | !((all >> button_pins[i]) & 1);  // buttons are active low
        uint8_t bit = 1 << i;
        if ((hist[i] & 0x0F) == 0x0F) h |= bit;  // debounce: same state for 4 ms
        else if ((hist[i] & 0x0F) == 0) h &= ~bit;
    }
    held = h;
}

uint8_t sprig_buttons_held(void) { return held; }
