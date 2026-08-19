/*
 * bm_st7735.c — ST7735 寄存器驱动 + RGB565 帧缓冲
 *
 * 依赖：esp_driver_spi（SPI2 主机）、esp_lcd_panel_io（可选，直接用
 *       esp_lcd_panel_io_spi 的发命令/发数据接口）。
 * 说明：为避免引入 esp_lcd 组件（它附带面板驱动代码），这里用
 *       esp_lcd_panel_io_spi 的底层 io 接口手动发 ST7735 命令。
 *       若不想依赖 esp_lcd，可改用 driver/spi_master 的 device 事务自己拼
 *       DC 位，代码量差不多——本实现选择 io 接口（更简洁）。
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bm_config.h"
#include "bm_st7735.h"

#define TAG "bm_lcd"

/* ST7735 寄存器命令 */
#define ST7735_SWRESET  0x01
#define ST7735_SLPOUT   0x11
#define ST7735_NORON    0x13
#define ST7735_INVOFF   0x20
#define ST7735_DISPOFF  0x28
#define ST7735_DISPON   0x29
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A

#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_RGB 0x00

/* 每行像素字节数（RGB565） */
#define LCD_PIX_BYTES (LCD_W * 2)

static esp_lcd_panel_io_handle_t s_io;

static void tx_cmd(uint8_t cmd)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0);
}

static void tx_param(uint8_t cmd, const void *param, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, param, n);
}

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* 设置窗口（列/行地址） */
static void set_window(int x0, int y0, int x1, int y1)
{
    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0,
                         (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0,
                         (uint8_t)(y1 >> 8), (uint8_t)y1 };
    tx_param(ST7735_CASET, caset, 4);
    tx_param(ST7735_RASET, raset, 4);
}

/* 向指定矩形窗口写入像素（不走 DMA 的简单版；为省内存可逐行） */
static void write_pixels(const uint16_t *data, size_t n)
{
    esp_lcd_panel_io_tx_color(s_io, ST7735_RAMWR, data, n * 2);
}

bool bm_lcd_init(bm_lcd_t *lcd, uint16_t *framebuf)
{
    lcd->fb = framebuf;

    /* SPI 总线（与 SD 共用 SPI2） */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2,
    };
    if (spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                 &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed");
        return false;
    }

    /* ST7735 初始化序列（取自原版，Black Tab 反向面板） */
    static const uint8_t frmctr[] = {0x01, 0x2C, 0x2D};
    static const uint8_t pwctr1[] = {0xA2, 0x02, 0x84};
    static const uint8_t vmctr1[] = {0x0E};
    static const uint8_t gp[]   = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32,
                                   0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39,
                                   0x00, 0x01, 0x03, 0x10};
    static const uint8_t gn[]   = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C,
                                   0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F,
                                   0x00, 0x00, 0x02, 0x10};
    static const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    static const uint8_t colmod[]   = {0x05};
    static const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};

    tx_cmd(ST7735_DISPOFF);
    tx_cmd(ST7735_SWRESET);
    delay_ms(150);
    tx_cmd(ST7735_SLPOUT);
    delay_ms(500);
    tx_param(0xB1, frmctr, sizeof(frmctr));
    tx_param(0xC0, pwctr1, sizeof(pwctr1));
    tx_param(0xC5, vmctr1, sizeof(vmctr1));
    tx_param(0xE0, gp, sizeof(gp));
    tx_param(0xE1, gn, sizeof(gn));
    tx_cmd(ST7735_INVOFF);
    tx_param(ST7735_MADCTL, madctl_d, sizeof(madctl_d));
    tx_param(ST7735_COLMOD, colmod, sizeof(colmod));
    tx_cmd(ST7735_NORON);
    delay_ms(10);
    tx_param(ST7735_MADCTL, madctl_r, sizeof(madctl_r));

    /* 清屏并点亮 */
    memset(framebuf, 0, LCD_W * LCD_H * 2);
    bm_lcd_flush(lcd);
    tx_cmd(ST7735_DISPON);
    return true;
}

void bm_lcd_flush(bm_lcd_t *lcd)
{
    set_window(0, 0, LCD_W - 1, LCD_H - 1);
    write_pixels(lcd->fb, LCD_W * LCD_H);
}

void bm_lcd_fill(bm_lcd_t *lcd, uint16_t color)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        lcd->fb[i] = color;
    }
}

void bm_lcd_fill_rect(bm_lcd_t *lcd, int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            lcd->fb[yy * LCD_W + xx] = color;
        }
    }
}

void bm_lcd_draw_rect(bm_lcd_t *lcd, int x, int y, int w, int h, uint16_t color)
{
    bm_lcd_fill_rect(lcd, x, y, w, 1, color);
    bm_lcd_fill_rect(lcd, x, y + h - 1, w, 1, color);
    bm_lcd_fill_rect(lcd, x, y, 1, h, color);
    bm_lcd_fill_rect(lcd, x + w - 1, y, 1, h, color);
}

void bm_lcd_progress(bm_lcd_t *lcd, int x, int y, int w, int h,
                     int permille, uint16_t fg, uint16_t bg)
{
    bm_lcd_fill_rect(lcd, x, y, w, h, bg);
    int fw = (int)((int64_t)w * permille / 1000);
    if (fw > 0) bm_lcd_fill_rect(lcd, x, y, fw, h, fg);
}

/* ── 8x8 字体 ──────────────────────────────────────────── */
/* 用 5x7 精简字体（每个字符 5 字节，宽 5 高 7，放大 1 倍显示） */
#include "bm_font5x7.h"

int bm_lcd_draw_char(bm_lcd_t *lcd, int x, int y, char c, uint16_t color)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = bm_font5x7[(uint8_t)c - 32];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H) {
                    lcd->fb[py * LCD_W + px] = color;
                }
            }
        }
    }
    return x + 6;   /* 5 像素宽 + 1 间隔 */
}

void bm_lcd_draw_text(bm_lcd_t *lcd, int x, int y, const char *s, uint16_t color)
{
    int cx = x;
    int cy = y;
    while (*s) {
        if (*s == '\n') {
            cx = x;
            cy += 8;
            s++;
            continue;
        }
        cx = bm_lcd_draw_char(lcd, cx, cy, *s, color);
        s++;
        if (cx + 6 > LCD_W) {   /* 换行 */
            cx = x;
            cy += 8;
        }
    }
}