/*
 * bm_ui.h — 极简文本 UI（基于 bm_st7735 帧缓冲）
 * 屏幕 160x128，5x7 字体一行最多 26 字符，共 16 行。
 */
#ifndef BM_UI_H
#define BM_UI_H

#include "bm_st7735.h"
#include "bm_rom.h"

/* 配色（与原始 Loader 黄主题一致） */
#define UI_YELLOW  bm_rgb565(0xF6, 0xD3, 0x4A)
#define UI_BLACK   bm_rgb565(0x1B, 0x17, 0x13)
#define UI_BROWN   bm_rgb565(0x5C, 0x42, 0x20)
#define UI_RED     bm_rgb565(0xE6, 0x4B, 0x3C)
#define UI_CREAM   bm_rgb565(0xFF, 0xF3, 0xB0)
#define UI_GREEN   bm_rgb565(0x2D, 0xD4, 0x66)

/* 初始化 UI（绑定 lcd） */
void bm_ui_init(bm_lcd_t *lcd);

/* 全屏清空为背景色 */
void bm_ui_clear(void);

/* 绘制 splash（boot 界面） */
void bm_ui_splash(void);

/* 绘制 ROM 列表（高亮第 sel 项，loaded 标记 ">"，最多 visible 行） */
void bm_ui_rom_list(rom_entry_t *roms, int count, int sel, int loaded_idx);

/* 绘制进度屏（title + 百分比 + 进度条） */
void bm_ui_progress(const char *title, int pct, const char *detail);

/* 绘制信息页（多行文本，自动滚动到末尾——跳过实现，简单显示） */
void bm_ui_info(const char *text);

/* 刷新屏幕 */
void bm_ui_flush(void);

#endif /* BM_UI_H */