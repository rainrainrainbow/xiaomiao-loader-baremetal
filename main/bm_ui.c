/*
 * bm_ui.c — 极简文本 UI
 * 直接操作帧缓冲，无 LVGL。布局：
 *   - 顶部标题条（8px 高，深棕底奶油字）
 *   - 内容区（列表 / 进度 / 信息）
 *   - 底部提示条
 */
#include <stdio.h>
#include <string.h>

#include "bm_config.h"
#include "bm_st7735.h"
#include "bm_ui.h"

static bm_lcd_t *s_lcd;

#define TITLE_H 10
#define HINT_H  10
#define LINE_H  8
#define MARGIN  4

void bm_ui_init(bm_lcd_t *lcd)
{
    s_lcd = lcd;
    bm_lcd_fill(s_lcd, UI_YELLOW);
    bm_lcd_flush(s_lcd);
}

void bm_ui_clear(void)
{
    bm_lcd_fill(s_lcd, UI_YELLOW);
}

/* 标题条 + 底部提示 */
static void draw_frame(const char *title, const char *hint)
{
    /* 标题条 */
    bm_lcd_fill_rect(s_lcd, 0, 0, LCD_W, TITLE_H, UI_BROWN);
    if (title) {
        bm_lcd_draw_text(s_lcd, MARGIN, 1, title, UI_CREAM);
    }
    /* 底部提示 */
    bm_lcd_fill_rect(s_lcd, 0, LCD_H - HINT_H, LCD_W, HINT_H, UI_BROWN);
    if (hint) {
        bm_lcd_draw_text(s_lcd, MARGIN, LCD_H - HINT_H + 1, hint, UI_CREAM);
    }
}

void bm_ui_flush(void)
{
    bm_lcd_flush(s_lcd);
}

void bm_ui_splash(void)
{
    bm_lcd_fill(s_lcd, UI_YELLOW);
    const char *lines[] = {
        "XIAOMIAO",
        "ROM LOADER",
        "",
        "Loading...",
    };
    int y = 40;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        int w = (int)strlen(lines[i]) * 6;
        int x = (LCD_W - w) / 2;
        bm_lcd_draw_text(s_lcd, x, y, lines[i], UI_BROWN);
        y += LINE_H;
    }
    bm_lcd_flush(s_lcd);
}

void bm_ui_rom_list(rom_entry_t *roms, int count, int sel, int loaded_idx)
{
    bm_ui_clear();
    draw_frame("ROM LOADER", "A:Load  L:About");

    int y = TITLE_H + 2;
    int visible = (LCD_H - TITLE_H - HINT_H) / LINE_H - 1;   /* 留一行空 */
    int start = sel - visible / 2;                            /* 居中滚动 */
    if (start < 0) start = 0;

    for (int i = start; i < count && i < start + visible; i++) {
        char line[40];
        const char *mark = (i == loaded_idx) ? ">" : " ";
        const char *valid = roms[i].valid ? "" : " [x]";
        int len = snprintf(line, sizeof(line), "%s%s%s", mark, roms[i].name, valid);
        if (len > 26) len = 26;

        /* 高亮当前项 */
        if (i == sel) {
            bm_lcd_fill_rect(s_lcd, 0, y, LCD_W, LINE_H, UI_BROWN);
            bm_lcd_draw_text(s_lcd, MARGIN, y, line, UI_CREAM);
        } else {
            bm_lcd_draw_text(s_lcd, MARGIN, y, line, UI_BROWN);
        }
        y += LINE_H;
    }
    bm_lcd_flush(s_lcd);
}

void bm_ui_progress(const char *title, int pct, const char *detail)
{
    bm_ui_clear();
    draw_frame(title, detail ? detail : "");

    /* 居中进度条 */
    int bw = 120, bh = 12;
    int bx = (LCD_W - bw) / 2;
    int by = 60;
    bm_lcd_progress(s_lcd, bx, by, bw, bh,
                    pct * 10, UI_GREEN, UI_CREAM);
    bm_lcd_draw_rect(s_lcd, bx - 1, by - 1, bw + 2, bh + 2, UI_BROWN);

    /* 百分比 */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int w = (int)strlen(buf) * 6;
    bm_lcd_draw_text(s_lcd, (LCD_W - w) / 2, by + bh + 4, buf, UI_BROWN);

    bm_lcd_flush(s_lcd);
}

void bm_ui_info(const char *text)
{
    bm_ui_clear();
    draw_frame("ABOUT", "L:Back");

    int y = TITLE_H + 2;
    /* 简单换行绘制（最多显示 13 行） */
    const char *p = text;
    int lines = 0;
    while (*p && lines < 13) {
        const char *nl = strchr(p, '\n');
        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
        if (ln > 26) ln = 26;
        char line[28];
        memcpy(line, p, ln);
        line[ln] = 0;
        bm_lcd_draw_text(s_lcd, MARGIN, y, line, UI_BROWN);
        y += LINE_H;
        lines++;
        if (nl) p = nl + 1;
        else break;
    }
    bm_lcd_flush(s_lcd);
}