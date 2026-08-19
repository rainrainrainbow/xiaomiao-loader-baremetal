/*
 * bm_rom.c — ROM 文件识别与解析
 * 支持三种输入：
 *   1. app-only .bin（magic 0xE9 @0）→ 直接写 ota_0
 *   2. merged .bin（magic 0xE9 @0x10000）→ 提取 app 段写 ota_0
 *   3. retro-go .img（内嵌分区表 @0x9000 或 0x8000）→ 提取 launcher→ota_0, retro-core→ota_1
 * 体积关键：不实现 SHA-256（bootloader 会用镜像自带 hash 验证），
 *           仅解析段表算大小，并做魔数/CRC 快速检查。
 */
#include <string.h>

#include "esp_log.h"

#include "bm_config.h"
#include "bm_rom.h"
#include "bm_sd.h"

#define TAG "bm_rom"

/* ── 工具 ──────────────────────────────────────────────── */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 读文件一个字节 */
static bool read_byte(bm_file_t *f, uint32_t off, uint8_t *b)
{
    if (!bm_file_seek(f, off)) {
        return false;
    }
    return bm_file_read(f, b, 1) == 1;
}

/* 读 n 字节到 dst */
static bool read_at(bm_file_t *f, uint32_t off, void *dst, size_t n)
{
    if (!bm_file_seek(f, off)) {
        return false;
    }
    return bm_file_read(f, dst, n) == n;
}

/* ── app 镜像大小计算 ──────────────────────────────────── */
uint32_t bm_rom_calc_app_size(bm_file_t *f, uint32_t offset)
{
    uint8_t hdr[24];
    if (!read_at(f, offset, hdr, sizeof(hdr))) {
        return 0;
    }
    if (hdr[0] != ESP_IMAGE_MAGIC) {
        return 0;
    }
    uint8_t seg_count = hdr[1];
    uint8_t hash_appended = hdr[23];

    uint32_t pos = sizeof(hdr);
    for (uint8_t i = 0; i < seg_count; i++) {
        uint8_t sh[8];
        if (!read_at(f, offset + pos, sh, sizeof(sh))) {
            return 0;
        }
        uint32_t seg_len = le32(&sh[4]);
        pos += sizeof(sh) + seg_len;
    }
    pos = (pos + 15) & ~(uint32_t)15;   /* pad 16 */
    pos += 1;                            /* checksum */
    pos = (pos + 15) & ~(uint32_t)15;
    if (hash_appended) {
        pos += 32;                       /* SHA-256 */
    }
    return pos;
}

/* ── 检测 app 偏移 ─────────────────────────────────────── */
static uint32_t rom_detect_app_offset(bm_file_t *f)
{
    uint8_t b0;
    if (read_byte(f, 0, &b0) && b0 == ESP_IMAGE_MAGIC) {
        return 0;
    }
    uint8_t bm;
    if (read_byte(f, APP_OFFSET_MERGED, &bm) && bm == ESP_IMAGE_MAGIC) {
        return APP_OFFSET_MERGED;
    }
    return (uint32_t)-1;
}

/* ── .img 分区表解析（retro-go full-flash） ─────────────── */
#define IMG_PT_ENTRY_SZ 32
#define IMG_PT_MAX      32

/* 解析一个分区表位置；成功填 r->img_* 并返回 true */
static bool img_parse_at(bm_file_t *f, rom_entry_t *r, uint32_t pt_off)
{
    bool found_launcher = false, found_core = false;
    uint32_t launcher_off = 0, core_off = 0;

    for (int i = 0; i < IMG_PT_MAX; i++) {
        uint8_t e[IMG_PT_ENTRY_SZ];
        if (!read_at(f, pt_off + i * IMG_PT_ENTRY_SZ, e, IMG_PT_ENTRY_SZ)) {
            break;
        }
        if (e[0] == 0xEB) break;                 /* MDT 结束标记 */
        if (e[0] != 0xAA || e[1] != 0x50) break; /* 不是分区表 */
        if (e[2] != 0) continue;                 /* 跳过非 app */
        uint32_t off = le32(&e[4]);
        char label[17];
        memcpy(label, &e[12], 16);
        label[16] = 0;
        if (strcmp(label, "launcher") == 0) {
            launcher_off = off;
            found_launcher = true;
        } else if (strcmp(label, "retro-core") == 0) {
            core_off = off;
            found_core = true;
        }
    }
    if (!found_launcher || !found_core) {
        return false;
    }
    r->img_launcher_off = launcher_off;
    r->img_core_off = core_off;
    r->img_launcher_size = bm_rom_calc_app_size(f, launcher_off);
    r->img_core_size = bm_rom_calc_app_size(f, core_off);
    return r->img_launcher_size > 0 && r->img_core_size > 0;
}

static bool img_parse_partitions(bm_file_t *f, rom_entry_t *r)
{
    if (img_parse_at(f, r, IMG_PT_OFFSET)) {
        return true;
    }
    return img_parse_at(f, r, IMG_PT_OFFSET_LEGACY);
}

/* ── 主解析入口 ────────────────────────────────────────── */
bool bm_rom_parse(bm_sd_t *sd, bm_file_t *f, rom_entry_t *r)
{
    (void)sd;
    memset(r, 0, sizeof(*r));

    if (img_parse_partitions(f, r)) {
        r->type = ROM_DUAL_IMG;
        r->valid = true;
        return true;
    }

    uint32_t off = rom_detect_app_offset(f);
    if (off != (uint32_t)-1) {
        r->app_offset = off;
        r->app_size = bm_rom_calc_app_size(f, off);
        r->valid = r->app_size > 0;
        r->type = ROM_SINGLE;
        ESP_LOGI(TAG, "single app @0x%lx size=%lu",
                 (unsigned long)off, (unsigned long)r->app_size);
        return r->valid;
    }
    ESP_LOGW(TAG, "not a valid ESP32 image");
    return false;
}