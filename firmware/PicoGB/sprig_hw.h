// Hack Club Sprig hardware: ST7735 LCD on SPI0 and the 8 buttons, with the
// same pins and panel registers as Sprig's own firmware.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// The panel is portrait in memory (128 columns x 160 rows) and held
// landscape: view pixel (x, y) of the 160x128 picture is panel row x,
// panel column 127 - y.
#define LCD_COLS 128
#define LCD_ROWS 160

enum { BTN_W, BTN_A, BTN_S, BTN_D, BTN_I, BTN_J, BTN_K, BTN_L, NUM_BUTTONS };

void sprig_init(void);

// SPI0 is shared by the LCD (CS GP20) and the microSD card (CS GP21). Whoever
// talks on it holds the bus: core 1 while it sends a frame, core 0 around SD
// access. take() waits for the other user and sets clock, format and CS.
enum { BUS_LCD, BUS_SD };
#define SD_HZ 15700000  // the card fails at 20.8 MHz; 15.6 and 17.9 MHz test clean
void sprig_bus_take(int who);
void sprig_bus_give(int who);
void sprig_backlight(bool on);
// Starts DMA of panel rows [row0, row0 + nrows) from px (LCD_COLS * nrows pixels).
void sprig_lcd_rows(int row0, int nrows, const uint16_t *px);
bool sprig_lcd_busy(void);

void sprig_buttons_sample(void);   // call about once a millisecond
uint8_t sprig_buttons_held(void);  // debounced, bit n = button n
