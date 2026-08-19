/*
 * bm_flash.h — 内建 flash 状态块（自制 NVS）+ ROM 分区写入 + otadata
 */
#ifndef BM_FLASH_H
#define BM_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/* 状态块（固化在 nvs 分区起始 sector） */
typedef struct {
    char     cur_name[64];   /* 当前 ota_0 里的 ROM 名 */
    uint32_t cur_size;       /* 对应文件大小 */
    bool     valid;
} bm_state_t;

/* 读取状态块（校验 magic+crc） */
void bm_state_load(bm_state_t *st);

/* 写入状态块 */
void bm_state_save(const char *name, uint32_t size);

/* 清除状态块 */
void bm_state_clear(void);

/* 检查 ota_0 是否已有有效 app（magic 0xE9） */
bool bm_ota0_has_app(void);

/* 从已打开的 SD 文件把 [off, off+size) 写入 ota_0 分区（流式 chunk）。
 * 返回写入字节数，失败返回 0。 */
uint32_t bm_flash_rom_to_ota0(bm_file_t *f, uint32_t off, uint32_t size,
                              void (*progress)(int pct));

/* 从已打开的 SD 文件把 [off, off+size) 写入任意 app 分区（ota_0/ota_1）。
 * 返回写入字节数，失败返回 0。 */
uint32_t bm_flash_rom_to_part(bm_file_t *f, uint32_t off, uint32_t size,
                              const esp_partition_t *part,
                              void (*progress)(int pct));

/* 写 otadata 让 bootloader 启动 ota_0 */
bool bm_ota_set_boot_ota0(void);

#endif /* BM_FLASH_H */