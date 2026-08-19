/*
 * bm_config.h — 裸机 Loader 编译期配置
 * 引脚/分区/字体均在此定义，便于按硬件调整。
 */
#ifndef BM_CONFIG_H
#define BM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── 屏幕 ─────────────────────────────────────────────── */
#define LCD_SPI_HOST         SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ   (60 * 1000 * 1000)
#define LCD_NATIVE_W         128
#define LCD_NATIVE_H         160
#define LCD_W                160   /* 旋转90°后逻辑宽 */
#define LCD_H                128   /* 旋转90°后逻辑高 */
#define LCD_CMD_BITS         8
#define LCD_PARAM_BITS       8

#define PIN_LCD_SCLK         GPIO_NUM_18
#define PIN_LCD_MOSI         GPIO_NUM_23
#define PIN_LCD_MISO         GPIO_NUM_19
#define PIN_LCD_CS           GPIO_NUM_5
#define PIN_LCD_DC           GPIO_NUM_4
#define PIN_SD_CS            GPIO_NUM_22

/* ── 按键（低有效，带内部上拉） ─────────────────────── */
#define BTN_UP_GPIO          GPIO_NUM_2
#define BTN_DOWN_GPIO        GPIO_NUM_13
#define BTN_LEFT_GPIO        GPIO_NUM_27
#define BTN_RIGHT_GPIO       GPIO_NUM_35
#define BTN_A_GPIO           GPIO_NUM_34   /* A / ENTER */
#define BTN_B_GPIO           GPIO_NUM_12   /* B / ESC   */

#define BTN_ACTIVE_LEVEL     0
#define BTN_DEBOUNCE_MS      25

/* ── SD / FAT32 ───────────────────────────────────────── */
#define SD_SPI_MAX_FREQ_KHZ  10000
#define ROM_DIR              "/boot"       /* SD 卡内路径（无挂载点前缀） */
#define MAX_ROMS             32

/* ── flash 布局（与 partitions.csv 一致） ─────────────── */
#define OTA0_PART_LABEL      "launcher"    /* 分区表里的名字 */
#define OTA1_PART_LABEL      "retro-core"
#define FLASH_SECTOR_SZ      0x1000
#define ROM_CHUNK_SZ         4096

/* ── 状态块（自制 NVS，占用 nvs 分区前一个 sector） ─── */
#define STATE_SECTOR_OFF     0xA000
#define STATE_MAGIC          0x5A4F4C44    /* "DLZ" 之类 */
#define STATE_KEY_CURNAME    1
#define STATE_KEY_CURSIZE    2

/* ── WiFi 更新（由 Kconfig 注入，未启用时为 0 长度） ── */
#ifdef CONFIG_XIAOMIAO_ENABLE_WIFI
#define BM_WIFI_ENABLED      1
#else
#define BM_WIFI_ENABLED      0
#endif

#ifndef CONFIG_XIAOMIAO_WIFI_SSID
#define CONFIG_XIAOMIAO_WIFI_SSID  ""
#endif
#ifndef CONFIG_XIAOMIAO_WIFI_PASS
#define CONFIG_XIAOMIAO_WIFI_PASS  ""
#endif
#ifndef CONFIG_XIAOMIAO_WIFI_HOST
#define CONFIG_XIAOMIAO_WIFI_HOST  ""
#endif
#ifndef CONFIG_XIAOMIAO_WIFI_PORT
#define CONFIG_XIAOMIAO_WIFI_PORT  80
#endif
#ifndef CONFIG_XIAOMIAO_WIFI_PATH
#define CONFIG_XIAOMIAO_WIFI_PATH  "/latest.bin"
#endif

#endif /* BM_CONFIG_H */