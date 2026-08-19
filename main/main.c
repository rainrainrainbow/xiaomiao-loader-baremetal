/*
 * xiaomiao-loader 裸机版主入口
 *
 * app_main 流程：
 *   1. 快速启动检测：ota_0 已有有效 ROM 且 B 键未按 → 直接 set_boot(ota_0) 重启
 *   2. 初始化 LCD + 自制 UI
 *   3. 挂载 SD，扫描 ROM
 *   4. 循环：按键选择 ROM → A 加载 / L 看信息 / R WiFi 更新（若启用）
 *   5. 加载 ROM：解析 → 写 ota_0（单 app）或 ota_0+ota_1（双 app）→ set_boot → 重启
 *
 * 体积关键：不依赖 LVGL / fatfs / nvs_flash / app_update（WiFi 版除外）。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bm_config.h"
#include "bm_flash.h"
#include "bm_rom.h"
#include "bm_sd.h"
#include "bm_st7735.h"
#include "bm_ui.h"

#if BM_WIFI_ENABLED
#include "bm_wifi_ota.h"
#endif

#define TAG "loader"

/* ── 按键状态机 ────────────────────────────────────────── */
#define KEY_NONE   (-1)
#define KEY_UP     0
#define KEY_DOWN   1
#define KEY_LEFT   2
#define KEY_RIGHT  3
#define KEY_A      4
#define KEY_B      5

static const gpio_num_t s_btn_gpios[] = {
    BTN_UP_GPIO, BTN_DOWN_GPIO, BTN_LEFT_GPIO,
    BTN_RIGHT_GPIO, BTN_A_GPIO, BTN_B_GPIO,
};

/* 读取当前按下的键（低有效），返回 KEY_* 或 KEY_NONE */
static int keys_read_raw(void)
{
    for (int i = 0; i < 6; i++) {
        if (gpio_get_level(s_btn_gpios[i]) == BTN_ACTIVE_LEVEL) {
            return i;
        }
    }
    return KEY_NONE;
}

/* 带消抖的按键事件：返回 0=无事件，>0 键码，-1 释放 */
static int s_last_raw = KEY_NONE;
static uint32_t s_raw_changed_ms = 0;
static int s_stable = KEY_NONE;

static int keys_poll(void)
{
    int raw = keys_read_raw();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (raw != s_last_raw) {
        s_last_raw = raw;
        s_raw_changed_ms = now;
        if (raw < 0) {
            s_stable = KEY_NONE;
        }
    }
    if (now - s_raw_changed_ms >= BTN_DEBOUNCE_MS) {
        s_stable = s_last_raw;
    }
    return s_stable;
}

/* 组合键检测：A + B 同时按下（WiFi 更新模式，仅 WiFi 版使用） */
#if BM_WIFI_ENABLED
static bool keys_a_b_pressed(void)
{
    return gpio_get_level(BTN_A_GPIO) == BTN_ACTIVE_LEVEL &&
           gpio_get_level(BTN_B_GPIO) == BTN_ACTIVE_LEVEL;
}
#endif

/* ── 全局状态 ──────────────────────────────────────────── */
static bm_lcd_t s_lcd;
static bm_sd_t  s_sd;
static rom_entry_t s_roms[MAX_ROMS];
static int s_rom_count = 0;
static int s_sel = 0;             /* ROM 列表光标 */
static int s_loaded_idx = -1;     /* ota_0 里已加载的 ROM 索引 */

/* ── 进度回调（UI） ────────────────────────────────────── */
static void flash_progress(int pct)
{
    bm_ui_progress("FLASHING", pct, "Do not power off!");
}

/* ── 主界面循环 ────────────────────────────────────────── */
static void show_main(void)
{
    bm_ui_rom_list(s_roms, s_rom_count, s_sel, s_loaded_idx);
}

static void about_screen(void)
{
    char info[300];
    snprintf(info, sizeof(info),
             "Xiaomiao ROM Loader\n"
             "Bare-metal edition\n"
             "No LVGL / fatfs\n"
             "SD: %s\n"
             "ROMs: %d\n"
             "\n"
             "A: Load   L: Back\n"
             "R: WiFi OTA%s",
             s_sd.mounted ? s_sd.name : "none",
             s_rom_count,
             BM_WIFI_ENABLED ? " [on]" : " [off]");
    bm_ui_info(info);
}

/* 加载单个 ROM（单 app 或双 app），成功返回 true */
static bool load_rom(const rom_entry_t *rom)
{
    bm_file_t f;
    if (!bm_file_open(&s_sd, rom->path, &f)) {
        ESP_LOGE(TAG, "open %s failed", rom->path);
        return false;
    }

    if (rom->type == ROM_DUAL_IMG) {
        /* 双 app：launcher→ota_0，retro-core→ota_1，然后 set_boot(ota_0) */
        const esp_partition_t *ota0 = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        const esp_partition_t *ota1 = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
        if (!ota0 || !ota1) {
            ESP_LOGE(TAG, "ota0/ota1 not found");
            return false;
        }
        /* 写 launcher → ota_0 */
        uint32_t w0 = bm_flash_rom_to_part(&f, rom->img_launcher_off,
                                           rom->img_launcher_size, ota0,
                                           flash_progress);
        if (w0 != rom->img_launcher_size) {
            ESP_LOGE(TAG, "launcher write failed (%lu/%lu)",
                     (unsigned long)w0, (unsigned long)rom->img_launcher_size);
            return false;
        }
        /* 写 retro-core → ota_1 */
        uint32_t w1 = bm_flash_rom_to_part(&f, rom->img_core_off,
                                           rom->img_core_size, ota1,
                                           flash_progress);
        if (w1 != rom->img_core_size) {
            ESP_LOGE(TAG, "retro-core write failed (%lu/%lu)",
                     (unsigned long)w1, (unsigned long)rom->img_core_size);
            return false;
        }
        if (!bm_ota_set_boot_ota0()) {
            return false;
        }
        return true;
    } else {
        /* 单 app */
        uint32_t off = rom->app_offset;
        uint32_t size = rom->app_size;
        uint32_t w = bm_flash_rom_to_ota0(&f, off, size, flash_progress);
        if (w != size) {
            ESP_LOGE(TAG, "ota_0 write failed (%lu/%lu)",
                     (unsigned long)w, (unsigned long)size);
            return false;
        }
        if (!bm_ota_set_boot_ota0()) {
            return false;
        }
        return true;
    }
}

/* ── WiFi 更新（可选） ─────────────────────────────────── */
#if BM_WIFI_ENABLED
static void wifi_progress(int pct)
{
    bm_ui_progress("WIFI OTA", pct, "Downloading...");
}
#endif

/* ── app_main ──────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "Bare-metal Loader boot");

    /* 1. 快速启动：ota_0 已有 ROM 且 B 未按 → 直接进 ROM */
    bm_state_t st;
    bm_state_load(&st);

    bool b_pressed = gpio_get_level(BTN_B_GPIO) == BTN_ACTIVE_LEVEL;
    if (st.valid && !b_pressed && bm_ota0_has_app()) {
        if (bm_ota_set_boot_ota0()) {
            ESP_LOGI(TAG, "fast boot to %s", st.cur_name);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }

    /* 2. 初始化硬件 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_UP_GPIO) | (1ULL << BTN_DOWN_GPIO) |
                        (1ULL << BTN_LEFT_GPIO) | (1ULL << BTN_RIGHT_GPIO) |
                        (1ULL << BTN_A_GPIO) | (1ULL << BTN_B_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* 帧缓冲放 PSRAM（40KB） */
    uint16_t *fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    if (!fb) {
        fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_8BIT);
    }
    assert(fb && "framebuffer alloc failed");

    if (!bm_lcd_init(&s_lcd, fb)) {
        ESP_LOGE(TAG, "LCD init failed");
        return;
    }
    bm_ui_init(&s_lcd);
    bm_ui_splash();

    /* 3. 挂载 SD + 扫描 ROM */
    if (bm_sd_init(&s_sd)) {
        char names[MAX_ROMS][64];
        s_rom_count = bm_sd_list_roms(&s_sd, names, MAX_ROMS);
        for (int i = 0; i < s_rom_count && i < MAX_ROMS; i++) {
            memset(&s_roms[i], 0, sizeof(s_roms[i]));
            snprintf(s_roms[i].name, sizeof(s_roms[i].name), "%s", names[i]);
            snprintf(s_roms[i].path, sizeof(s_roms[i].path), "/boot/%s", names[i]);

            bm_file_t f;
            memset(&f, 0, sizeof(f));
            if (bm_file_open(&s_sd, s_roms[i].path, &f)) {
                bm_rom_parse(&s_sd, &f, &s_roms[i]);
                s_roms[i].file_size = f.file_size;
            }
        }
    } else {
        ESP_LOGW(TAG, "SD init failed");
    }

    /* 4. 找当前 ota_0 里的 ROM 索引（用于 ">" 标记） */
    if (st.valid) {
        for (int i = 0; i < s_rom_count; i++) {
            if (strcmp(s_roms[i].name, st.cur_name) == 0) {
                s_loaded_idx = i;
                break;
            }
        }
    }

    show_main();

    /* 5. 主循环 */
    while (1) {
        int key = keys_poll();

        if (key == KEY_UP) {
            if (s_sel > 0) s_sel--;
            show_main();
        } else if (key == KEY_DOWN) {
            if (s_sel < s_rom_count - 1) s_sel++;
            show_main();
        } else if (key == KEY_LEFT) {
            about_screen();
        } else if (key == KEY_RIGHT) {
#if BM_WIFI_ENABLED
            if (keys_a_b_pressed()) {
                /* 加载 WiFi 配置（从 SD 卡 /boot/wifi.conf） */
                bm_wifi_config_t wcfg;
                bm_ui_progress("WIFI OTA", 0, "Loading config...");
                if (!bm_wifi_load_config(&s_sd, &wcfg)) {
                    bm_ui_progress("WIFI OTA", 0, "No wifi.conf on SD!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    show_main();
                } else {
                    bm_ui_progress("WIFI OTA", 0, "Connecting...");
                    bool ok = bm_wifi_ota_update(&wcfg, wifi_progress);
                    if (ok) {
                        bm_ui_progress("WIFI OTA", 100, "Done! Rebooting...");
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    } else {
                        bm_ui_progress("WIFI OTA", 0, "FAILED");
                        vTaskDelay(pdMS_TO_TICKS(1500));
                        show_main();
                    }
                }
            }
#else
            /* 无 WiFi 时 RIGHT 无操作 */
#endif
        } else if (key == KEY_A) {
            if (s_rom_count > 0 && s_sel >= 0 && s_sel < s_rom_count) {
                const rom_entry_t *rom = &s_roms[s_sel];
                if (!rom->valid) {
                    bm_ui_progress("ROM", 0, "INVALID IMAGE");
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    show_main();
                    continue;
                }
                if (load_rom(rom)) {
                    bm_state_save(rom->name, rom->file_size);
                    bm_ui_progress("DONE", 100, "Rebooting...");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                } else {
                    bm_ui_progress("FAIL", 0, "Write error");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    show_main();
                }
            }
        } else if (key == KEY_B) {
            /* B：返回主列表（from about） */
            show_main();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}