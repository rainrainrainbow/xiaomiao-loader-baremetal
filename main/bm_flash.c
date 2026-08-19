/*
 * bm_flash.c — 裸机 flash 操作
 *
 * 1. 状态块（自制 NVS）：固化在 nvs 分区起始扇区，magic + crc32 保护。
 * 2. ROM 写入 ota_0：通过 esp_partition API 直接写（不含 esp_ota/app_update）。
 * 3. otadata 写入：手写 ESP-IDF 标准 otadata 条目，bootloader 据此启动 ota_0。
 *
 * otadata 格式（与 IDF 一致，esp_ota_select_entry_t，32 字节）：
 *   offset 0 : ota_seq    (uint32)
 *   offset 4 : seq_label  (20B)
 *   offset 24: ota_state  (uint32)
 *   offset 28: crc        (crc32 of bytes[0..27])
 * otadata 分区 8KB = 两个 4KB 槽位；槽1 @0x0，槽2 @0x1000。
 * 有效条目 = crc 校验通过 && ota_seq != 0 && ota_seq != 0xFFFFFFFF。
 */
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#include "bm_config.h"
#include "bm_flash.h"
#include "bm_sd.h"

#define TAG "bm_flash"

/* ── 标准 CRC32（逐位法，省 flash 表） ────────────────── */
static uint32_t crc32_bits(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(crc & 1));
        }
    }
    return ~crc;
}

/* ── 状态块布局 ────────────────────────────────────────── */
#define STATE_MAGIC_32 0x58424D53    /* "SMBX" 之类 */

typedef struct {
    uint32_t magic;
    char     cur_name[64];
    uint32_t cur_size;
    uint32_t crc;                   /* crc32(magic..cur_size) */
} bm_state_blk_t;

static const esp_partition_t *find_part(const char *label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_OTA_0, label);
}

void bm_state_load(bm_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->valid = false;

    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (!nvs) {
        ESP_LOGW(TAG, "nvs partition not found");
        return;
    }
    bm_state_blk_t blk;
    esp_err_t err = esp_partition_read(nvs, 0, &blk, sizeof(blk));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "state read fail: %s", esp_err_to_name(err));
        return;
    }
    if (blk.magic != STATE_MAGIC_32) {
        return;
    }
    uint32_t got = crc32_bits(0, &blk, offsetof(bm_state_blk_t, crc));
    if (got != blk.crc) {
        ESP_LOGW(TAG, "state crc mismatch");
        return;
    }
    strncpy(st->cur_name, blk.cur_name, sizeof(st->cur_name) - 1);
    st->cur_size = blk.cur_size;
    st->valid = true;
}

void bm_state_save(const char *name, uint32_t size)
{
    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (!nvs) {
        ESP_LOGW(TAG, "nvs partition not found");
        return;
    }
    bm_state_blk_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.magic = STATE_MAGIC_32;
    strncpy(blk.cur_name, name, sizeof(blk.cur_name) - 1);
    blk.cur_size = size;
    blk.crc = crc32_bits(0, &blk, offsetof(bm_state_blk_t, crc));

    esp_partition_erase_range(nvs, 0, nvs->size);
    esp_partition_write(nvs, 0, &blk, sizeof(blk));
}

void bm_state_clear(void)
{
    const esp_partition_t *nvs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (nvs) {
        esp_partition_erase_range(nvs, 0, nvs->size);
    }
}

bool bm_ota0_has_app(void)
{
    const esp_partition_t *part = find_part(OTA0_PART_LABEL);
    if (!part) {
        return false;
    }
    uint8_t magic = 0xFF;
    if (esp_partition_read(part, 0, &magic, 1) != ESP_OK) {
        return false;
    }
    return magic == 0xE9;
}

/* ── otadata 写入（手写，等效 esp_ota_set_boot_partition(ota_0)） ── */
bool bm_ota_set_boot_ota0(void)
{
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!ota) {
        ESP_LOGE(TAG, "otadata partition not found");
        return false;
    }

    /* 构造一个有效条目：seq=1, state=VALID */
    uint8_t entry[32];
    memset(entry, 0, sizeof(entry));
    uint32_t seq = 1;
    uint32_t state = 2;   /* ESP_OTA_IMG_VALID */
    memcpy(entry, &seq, 4);
    memcpy(entry + 24, &state, 4);
    /* resp: seq_label 全 0 即可（bootloader 只比对 ota_seq / crc） */
    uint32_t crc = crc32_bits(0, entry, 28);
    memcpy(entry + 28, &crc, 4);

    /* 槽位 1 在 offset 0；先整分区擦除，再写入 32 字节 + 其余 0xFF */
    esp_partition_erase_range(ota, 0, ota->size);

    uint8_t buf[512];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, entry, sizeof(entry));
    esp_partition_write(ota, 0, buf, sizeof(buf));

    ESP_LOGI(TAG, "otadata written: seq=%lu state=%lu crc=%08lx",
             (unsigned long)seq, (unsigned long)state, (unsigned long)crc);
    return true;
}

/* ── ROM 流式写入（通用分区版） ────────────────────────── */
uint32_t bm_flash_rom_to_part(bm_file_t *f, uint32_t off, uint32_t size,
                              const esp_partition_t *part,
                              void (*progress)(int pct))
{
    if (!part) {
        ESP_LOGE(TAG, "partition NULL");
        return 0;
    }
    if (size > part->size) {
        ESP_LOGE(TAG, "ROM %lu bytes > partition %lu", (unsigned long)size,
                 (unsigned long)part->size);
        return 0;
    }

    if (!bm_file_seek(f, off)) {
        ESP_LOGE(TAG, "seek to %lu failed", (unsigned long)off);
        return 0;
    }

    /* 先擦除整个分区 */
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase partition failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint8_t chunk[ROM_CHUNK_SZ];
    uint32_t written = 0;
    while (written < size) {
        uint32_t want = size - written;
        if (want > ROM_CHUNK_SZ) want = ROM_CHUNK_SZ;
        size_t got = bm_file_read(f, chunk, want);
        if (got == 0) {
            ESP_LOGE(TAG, "read 0 at %lu", (unsigned long)written);
            return written;   /* 返回已写，调用方可判断 < size 失败 */
        }
        err = esp_partition_write(part, written, chunk, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write partition @%lu failed: %s",
                     (unsigned long)written, esp_err_to_name(err));
            return written;
        }
        written += (uint32_t)got;
        if (progress) {
            progress((int)((uint64_t)written * 100 / size));
        }
    }
    return written;
}

uint32_t bm_flash_rom_to_ota0(bm_file_t *f, uint32_t off, uint32_t size,
                              void (*progress)(int pct))
{
    const esp_partition_t *part = find_part(OTA0_PART_LABEL);
    return bm_flash_rom_to_part(f, off, size, part, progress);
}