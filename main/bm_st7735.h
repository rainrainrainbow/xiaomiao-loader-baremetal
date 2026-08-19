/*
 * bm_st7735.h — ST7735 寄存器驱动 + 帧缓冲绘制
 * 不含 LVGL，直接写 RGB565 帧缓冲，SPI DMA 送出。
 */
#ifndef BM_ST7735_H
#define BM_ST7735_H

#include <stdbool.h>
#include <stdint.h>

/* 帧缓冲：160x128 RGB565 = 40KB（调用方分配，建议 PSRAM） */
typedef struct {
    uint16_t *fb;      /* 像素数组，大小 LCD_W*LCD_H */
} bm_lcd_t;

/* 初始化：挂 SPI 总线、配置面板、清屏 */
bool bm_lcd_init(bm_lcd_t *lcd, uint16_t *framebuf);

/* 全屏刷新（把 fb 通过 SPI 送出去） */
void bm_lcd_flush(bm_lcd_t *lcd);

/* 纯色填充整个屏幕 */
void bm_lcd_fill(bm_lcd_t *lcd, uint16_t color);

/* 在 (x,y) 画一个 w×h 实心矩形 */
void bm_lcd_fill_rect(bm_lcd_t *lcd, int x, int y, int w, int h, uint16_t color);

/* 画 1px 空心矩形边框 */
void bm_lcd_draw_rect(bm_lcd_t *lcd, int x, int y, int w, int h, uint16_t color);

/* 画水平进度条（x,y,w,h, 0..1000 千分比） */
void bm_lcd_progress(bm_lcd_t *lcd, int x, int y, int w, int h,
                     int permille, uint16_t fg, uint16_t bg);

/* 在 (x,y) 画一个 ASCII 字符（8x8），返回下一个 x */
int bm_lcd_draw_char(bm_lcd_t *lcd, int x, int y, char c, uint16_t color);

/* 在 (x,y) 画字符串（8x8 字体），自动换行到 (x0, y+8) */
void bm_lcd_draw_text(bm_lcd_t *lcd, int x, int y, const char *s, uint16_t color);

/* 便捷颜色（RGB565） */
static inline uint16_t bm_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

#endif /* BM_ST7735_H */